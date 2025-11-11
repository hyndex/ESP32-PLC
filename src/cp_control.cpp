#include "cp_control.h"

#include <Arduino.h>
#include <math.h>
#include <limits.h>
#include <algorithm>
#include "evse_config.h"
// Threshold anchors (millivolts)
static int g_t12 = CP_T12_DEFAULT_MV;
static int g_t9  = CP_T9_DEFAULT_MV;
static int g_t6  = g_t9 - CP_THRESHOLD_STEP_MV;
static int g_t3  = g_t6 - CP_THRESHOLD_STEP_MV;
static int g_t0  = g_t3 - CP_THRESHOLD_STEP_MV;

enum class CpMode : uint8_t { Manual = 0, DcAuto = 1 };
static CpMode g_mode = CpMode::DcAuto;
static bool g_manual_pwm_enabled = false;
static uint16_t g_manual_pwm_duty_pct = 100;

static uint32_t g_last_ledc_duty = 0xFFFFFFFFu;

static int g_ring[CP_RING_LEN];
static uint8_t g_ring_head = 0;
static uint8_t g_ring_count = 0;

static char g_last_state = 'A';
static int  g_last_cp_mv = 0;
static int  g_last_cp_mv_peak = 0;
static int  g_last_cp_mv_robust = 0;
static uint16_t g_belowB_run = 0;

static bool g_contactor_cmd = false;
static bool g_contactor_fb = false;

static inline uint32_t pct_to_duty(uint16_t pct) {
    if (pct == 0) return 0;
    if (pct >= 100) return CP_MAX_DUTY_CYCLE;
    return (uint32_t)((CP_MAX_DUTY_CYCLE * (uint32_t)pct) / 100U);
}

static inline void write_ledc_duty(uint32_t duty) {
    if (duty != g_last_ledc_duty) {
        ledcWrite(CP_PWM_CHANNEL, duty);
        g_last_ledc_duty = duty;
    }
}

static inline void apply_pwm_manual() {
    uint32_t duty = g_manual_pwm_enabled ? pct_to_duty(g_manual_pwm_duty_pct) : CP_MAX_DUTY_CYCLE;
    write_ledc_duty(duty);
}

static inline void apply_dc_auto_output(char st) {
    uint16_t duty = (st == 'B' || st == 'C' || st == 'D') ? CP_CONNECTED_DUTY_PCT : 100;
    write_ledc_duty(pct_to_duty(duty));
}

static inline void hw_contactor_setup() {
    pinMode(CONTACTOR_COIL_PIN, OUTPUT);
    digitalWrite(CONTACTOR_COIL_PIN,
                 CONTACTOR_COIL_ACTIVE_HIGH ? LOW : HIGH);
#if CONTACTOR_AUX_PIN >= 0
    pinMode(CONTACTOR_AUX_PIN, INPUT);
#endif
}

static inline void hw_contactor_set(bool on) {
    digitalWrite(CONTACTOR_COIL_PIN,
                 on ? (CONTACTOR_COIL_ACTIVE_HIGH ? HIGH : LOW)
                    : (CONTACTOR_COIL_ACTIVE_HIGH ? LOW : HIGH));
}

static inline bool hw_contactor_aux() {
#if CONTACTOR_AUX_PIN >= 0
    int v = digitalRead(CONTACTOR_AUX_PIN);
    return CONTACTOR_AUX_ACTIVE_HIGH ? (v == HIGH) : (v == LOW);
#else
    return g_contactor_cmd;
#endif
}

static inline int ring_push_and_max(int v) {
    g_ring[g_ring_head] = v;
    g_ring_head = (g_ring_head + 1) % CP_RING_LEN;
    if (g_ring_count < CP_RING_LEN) g_ring_count++;
    int mx = g_ring[0];
    for (uint8_t i = 1; i < g_ring_count; ++i) {
        if (g_ring[i] > mx) mx = g_ring[i];
    }
    return mx;
}

static inline char classify_state_from_mv(int mv) {
    if (mv >= g_t12) return 'A';
    if (mv >= g_t9)  return 'B';
    if (mv >= g_t6)  return 'C';
    if (mv >= g_t3)  return 'D';
    if (mv >= g_t0)  return 'E';
    return 'F';
}

static void read_cp_mv_burst(int &min_mv, int &plateau_mv, int &avg_mv, int &peak_mv) {
    int minv = INT32_MAX;
    int maxv = INT32_MIN;
    int64_t acc = 0;
    int topk[CP_TOPK];
    int tk = 0;

    auto insert_topk = [&](int v) {
        if (tk < CP_TOPK) {
            int i = tk++;
            while (i > 0 && topk[i - 1] > v) {
                topk[i] = topk[i - 1];
                --i;
            }
            topk[i] = v;
        } else if (v > topk[0]) {
            topk[0] = v;
            int i = 0;
            while (i + 1 < tk && topk[i] > topk[i + 1]) {
                int t = topk[i];
                topk[i] = topk[i + 1];
                topk[i + 1] = t;
                ++i;
            }
        }
    };

    (void)analogRead(CP_ADC_PIN);

    for (int i = 0; i < CP_SAMPLE_COUNT; ++i) {
        delayMicroseconds(CP_SAMPLE_DELAY_US);
        int v = analogReadMilliVolts(CP_ADC_PIN);
        acc += v;
        if (v < minv) minv = v;
        if (v > maxv) maxv = v;
        insert_topk(v);
    }

    int robust = (tk == 0) ? (maxv == INT32_MIN ? 0 : maxv) : topk[tk - 1];
    if (tk > 4) {
        int start = tk - std::max(3, tk / 6);
        int end = tk - 1;
        int64_t sum = 0;
        int cnt = 0;
        for (int i = start; i <= end; ++i) {
            sum += topk[i];
            cnt++;
        }
        if (cnt) robust = (int)(sum / cnt);
    }

    min_mv = (minv == INT32_MAX) ? 0 : minv;
    plateau_mv = robust;
    avg_mv = (int)(acc / CP_SAMPLE_COUNT);
    peak_mv = (maxv == INT32_MIN) ? 0 : maxv;
}

void cp_init() {
    pinMode(CP_ADC_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(CP_ADC_PIN, ADC_11db);

    ledcSetup(CP_PWM_CHANNEL, CP_PWM_FREQUENCY, CP_PWM_RESOLUTION);
    ledcAttachPin(CP_PWM_PIN, CP_PWM_CHANNEL);
    write_ledc_duty(CP_MAX_DUTY_CYCLE);

    hw_contactor_setup();
    g_contactor_cmd = false;
    g_contactor_fb = hw_contactor_aux();

    for (uint8_t i = 0; i < CP_RING_LEN; ++i) g_ring[i] = 0;
}

void cp_tick() {
    int min_mv = 0, plateau_mv = 0, avg_mv = 0, peak_mv = 0;
    read_cp_mv_burst(min_mv, plateau_mv, avg_mv, peak_mv);

    g_last_cp_mv = plateau_mv;
    g_last_cp_mv_peak = peak_mv;
    g_last_cp_mv_robust = ring_push_and_max(plateau_mv);

    bool burst_has_B = (peak_mv >= g_t9);
    g_belowB_run = burst_has_B ? 0 : (uint16_t)std::min<int>(g_belowB_run + 1, 1000);

    char tentative = classify_state_from_mv(g_last_cp_mv_robust);
    char new_state = tentative;
    if (g_last_state == 'B' && (tentative == 'C' || tentative == 'D' || tentative == 'E' || tentative == 'F')) {
        if (g_belowB_run < CP_B_DEMOTE_BURSTS) new_state = 'B';
    }

    if (new_state != g_last_state) {
        g_last_state = new_state;
        Serial.printf("[CP] state -> %c (robust=%d mv, peak=%d mv)\n", g_last_state, g_last_cp_mv_robust, g_last_cp_mv_peak);
    }

    if (g_mode == CpMode::Manual) apply_pwm_manual();
    else apply_dc_auto_output(g_last_state);

    if (!cp_is_connected() && g_contactor_cmd) {
        cp_contactor_command(false);
    }
}

char cp_get_state() {
    return g_last_state;
}

int cp_get_latest_mv() {
    return g_last_cp_mv_robust;
}

bool cp_is_connected() {
    return (g_last_state == 'B' || g_last_state == 'C' || g_last_state == 'D');
}

bool cp_contactor_command(bool on) {
    if (on == g_contactor_cmd && g_contactor_fb == hw_contactor_aux()) return g_contactor_fb;

    hw_contactor_set(on);
    delay(20);
    g_contactor_cmd = on;
    g_contactor_fb = hw_contactor_aux();
    if (on && !g_contactor_fb) {
        Serial.println("[CP] Contactor command failed (no aux confirmation)");
        return false;
    }
    if (!on) g_contactor_fb = false;
    return g_contactor_fb;
}

bool cp_contactor_feedback() {
    g_contactor_fb = hw_contactor_aux();
    return g_contactor_fb;
}

bool cp_is_contactor_commanded() {
    return g_contactor_cmd;
}

void cp_set_pwm_manual(bool enable, uint16_t duty_pct) {
    g_mode = enable ? CpMode::Manual : CpMode::DcAuto;
    g_manual_pwm_enabled = enable;
    if (duty_pct > 100) duty_pct = 100;
    g_manual_pwm_duty_pct = duty_pct;
    if (g_mode == CpMode::Manual) apply_pwm_manual();
    else apply_dc_auto_output(g_last_state);
}
