#include "dc_can.h"

#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <mcp2515.h>
#include "evse_config.h"

static SPIClass canSPI(CAN_SPI_HOST);
static MCP2515 g_mcp2515(CAN_CS_PIN, 8000000, &canSPI);

#if MCP2515_CLK_MHZ == 8
static const CAN_CLOCK kCanClock = MCP_8MHZ;
#elif MCP2515_CLK_MHZ == 20
static const CAN_CLOCK kCanClock = MCP_20MHZ;
#else
static const CAN_CLOCK kCanClock = MCP_16MHZ;
#endif

static const uint8_t  MAXWELL_PROTO = 0x1;
static const uint32_t CAN_ID_MASK_ALL = 0x1FFFFFFFUL;

struct MaxwellModule {
    uint8_t  addr = 0;
    uint32_t last_seen_ms = 0;
    uint32_t last_status = 0;
    uint32_t last_v_mv = 0;
    uint32_t last_i_ma = 0;
};

static MaxwellModule g_modules[MAX_MODULES];
static uint8_t g_module_count = 0;

static bool g_can_ok = false;
static bool g_dc_enabled = false;
static bool g_dc_available = false;

static float g_dc_v_target = 0.0f;
static float g_dc_i_target = 0.0f;
static float g_dc_v_set = 0.0f;
static float g_dc_i_set = 0.0f;
static uint8_t g_group_addr = MAXWELL_GROUP_DEFAULT;

static uint32_t g_last_dc_ramp_ms = 0;
static uint32_t g_last_dc_poll_ms = 0;

static inline uint32_t build_can_id(uint8_t monitor, uint8_t module, uint8_t prodDay = 0, uint16_t snLow9 = 0) {
    uint32_t id = 0;
    id |= ((uint32_t)(MAXWELL_PROTO & 0x0F) << 25);
    id |= ((uint32_t)(monitor & 0x0F) << 21);
    id |= ((uint32_t)(module  & 0x7F) << 14);
    id |= ((uint32_t)(prodDay & 0x1F) <<  9);
    id |= ((uint32_t)(snLow9  & 0x1FF));
    return id;
}

static inline uint8_t pack_group_type(uint8_t group, uint8_t msgType) {
    return (uint8_t)(((group & 0x0F) << 4) | (msgType & 0x0F));
}

static inline void be_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static inline void be_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static bool maxwell_send(uint32_t id, const uint8_t *data, uint8_t len) {
    if (!g_can_ok) return false;
    struct can_frame frame;
    frame.can_id = (id & CAN_ID_MASK_ALL) | CAN_EFF_FLAG;
    frame.can_dlc = len;
    for (uint8_t i = 0; i < len && i < 8; ++i) frame.data[i] = data[i];
    return g_mcp2515.sendMessage(&frame) == MCP2515::ERROR_OK;
}

static bool cmd_allset(uint8_t moduleAddr, uint8_t onoff_hilo, uint16_t i_0p1A, uint16_t vbat_0p1V, uint16_t vout_0p1V) {
    uint8_t d[8] = { pack_group_type(g_group_addr, 0x0B), onoff_hilo, 0,0, 0,0, 0,0 };
    be_put_u16(&d[2], i_0p1A);
    be_put_u16(&d[4], vbat_0p1V);
    be_put_u16(&d[6], vout_0p1V);
    return maxwell_send(build_can_id(MAXWELL_MONITOR_ADDR, moduleAddr), d, 8);
}

static bool cmd_onoff(uint8_t moduleAddr, bool on) {
    uint8_t d[8] = { pack_group_type(g_group_addr, 0x0), 0x04, 0,0, 0,0,0,0 };
    be_put_u32(&d[4], on ? 0 : 1);
    return maxwell_send(build_can_id(MAXWELL_MONITOR_ADDR, moduleAddr), d, 8);
}

static bool cmd_read(uint8_t moduleAddr, uint8_t what) {
    uint8_t d[8] = { pack_group_type(g_group_addr, 0x2), what, 0,0,0,0,0,0 };
    return maxwell_send(build_can_id(MAXWELL_MONITOR_ADDR, moduleAddr), d, 8);
}

static void modules_upsert(uint8_t addr) {
    for (uint8_t i = 0; i < g_module_count; ++i) {
        if (g_modules[i].addr == addr) {
            g_modules[i].last_seen_ms = millis();
            return;
        }
    }
    if (g_module_count < MAX_MODULES) {
        g_modules[g_module_count].addr = addr;
        g_modules[g_module_count].last_seen_ms = millis();
        g_module_count++;
    }
}

static void handle_can_frame(const struct can_frame &f) {
    const uint32_t id = f.can_id & CAN_ID_MASK_ALL;
    const uint8_t proto = (id >> 25) & 0x0F;
    const uint8_t moduleAddr = (id >> 14) & 0x7F;
    if (proto != MAXWELL_PROTO || moduleAddr == 0) return;
    modules_upsert(moduleAddr);
    if (f.can_dlc < 2) return;
    const uint8_t b0 = f.data[0];
    const uint8_t msgType = (b0 & 0x0F);
    if (msgType != 0x03) return;
    const uint8_t cmd = f.data[1];
    uint32_t value = 0;
    if (f.can_dlc >= 8) {
        value = ((uint32_t)f.data[4] << 24) |
                ((uint32_t)f.data[5] << 16) |
                ((uint32_t)f.data[6] << 8)  |
                (uint32_t)f.data[7];
    }
    for (uint8_t i = 0; i < g_module_count; ++i) {
        if (g_modules[i].addr != moduleAddr) continue;
        if (cmd == 0x00) g_modules[i].last_v_mv = value;
        else if (cmd == 0x01) g_modules[i].last_i_ma = value;
        else if (cmd == 0x08) g_modules[i].last_status = value;
    }
}

static void dc_discover(uint16_t window_ms) {
    g_module_count = 0;
    cmd_read(0x00, 0x08);
    uint32_t until = millis() + window_ms;
    struct can_frame f;
    while ((int32_t)(millis() - until) < 0) {
        if (g_mcp2515.readMessage(&f) == MCP2515::ERROR_OK) handle_can_frame(f);
    }
}

static void dc_apply_setpoints(bool turnOffOnly) {
    uint8_t onoff = turnOffOnly ? 0x01 : 0x00;
    uint16_t i_0p1A = (uint16_t)lroundf(fabsf(g_dc_i_set) * 10.0f);
    uint16_t v_0p1V = (uint16_t)lroundf(fabsf(g_dc_v_set) * 10.0f);
    cmd_allset(0x00, onoff, i_0p1A, v_0p1V, v_0p1V);
}

static void dc_ramp_tick() {
    if (!g_can_ok) return;
    if ((int32_t)(millis() - g_last_dc_ramp_ms) < (int32_t)DC_RAMP_TICK_MS) return;
    g_last_dc_ramp_ms = millis();

    auto approach = [](float current, float target, float step) {
        if (current < target) return fminf(target, current + step);
        if (current > target) return fmaxf(target, current - step);
        return current;
    };

    const float stepV = DC_V_RAMP_V_PER_S * (DC_RAMP_TICK_MS / 1000.0f);
    const float stepI = DC_I_RAMP_A_PER_S * (DC_RAMP_TICK_MS / 1000.0f);

    float prevV = g_dc_v_set;
    float prevI = g_dc_i_set;

    float tgtV = g_dc_enabled ? g_dc_v_target : 0.0f;
    float tgtI = g_dc_enabled ? g_dc_i_target : 0.0f;

    g_dc_v_set = approach(g_dc_v_set, tgtV, stepV);
    g_dc_i_set = approach(g_dc_i_set, tgtI, stepI);

    if (fabsf(g_dc_v_set - prevV) > 0.5f || fabsf(g_dc_i_set - prevI) > 0.5f) {
        dc_apply_setpoints(false);
    }
}

static void dc_poll_tick() {
    if (!g_can_ok) return;
    uint32_t now = millis();
    if ((int32_t)(now - g_last_dc_poll_ms) > 300) {
        g_last_dc_poll_ms = now;
        cmd_read(0x00, 0x00);
        cmd_read(0x00, 0x01);
        cmd_read(0x00, 0x08);
    }
    struct can_frame f;
    while (g_mcp2515.readMessage(&f) == MCP2515::ERROR_OK) {
        handle_can_frame(f);
    }
}

void dc_can_init() {
#if CAN_RST_PIN >= 0
    pinMode(CAN_RST_PIN, OUTPUT);
    digitalWrite(CAN_RST_PIN, LOW);
    delay(5);
    digitalWrite(CAN_RST_PIN, HIGH);
    delay(5);
#endif
    canSPI.begin(CAN_SCK_PIN, CAN_MISO_PIN, CAN_MOSI_PIN, CAN_CS_PIN);

    MCP2515::ERROR err = g_mcp2515.reset();
    if (err == MCP2515::ERROR_OK) {
        err = g_mcp2515.setBitrate(CAN_125KBPS, kCanClock);
    }
    if (err == MCP2515::ERROR_OK) {
        g_mcp2515.setNormalMode();
        g_can_ok = true;
        g_dc_available = true;
        Serial.println("[DC CAN] MCP2515 ready @125kbps");
        dc_discover(200);
    } else {
        Serial.println("[DC CAN] MCP2515 init failed");
    }
}

void dc_can_tick() {
    if (!g_can_ok) return;
    dc_ramp_tick();
    dc_poll_tick();
}

void dc_enable_output(bool enable) {
    if (!g_can_ok) {
        g_dc_enabled = false;
        return;
    }
    if (enable == g_dc_enabled) return;
    g_dc_enabled = enable;
    if (!enable) {
        g_dc_v_target = 0.0f;
        g_dc_i_target = 0.0f;
        g_dc_v_set = 0.0f;
        g_dc_i_set = 0.0f;
        dc_apply_setpoints(true);
    }
}

bool dc_is_enabled() {
    return g_dc_enabled;
}

void dc_set_targets(float voltage_v, float current_a) {
    if (voltage_v < 0) voltage_v = 0;
    if (current_a < 0) current_a = 0;
    g_dc_v_target = voltage_v;
    g_dc_i_target = current_a;
}

float dc_get_set_voltage() {
    return g_dc_v_set;
}

float dc_get_set_current() {
    return g_dc_i_set;
}

float dc_get_bus_voltage() {
    if (g_module_count == 0 || g_modules[0].last_v_mv == 0) return g_dc_v_set;
    return g_modules[0].last_v_mv / 1000.0f;
}

float dc_get_bus_current() {
    if (g_module_count == 0 || g_modules[0].last_i_ma == 0) return g_dc_i_set;
    return g_modules[0].last_i_ma / 1000.0f;
}

void dc_emergency_stop() {
    if (!g_can_ok) return;
    cmd_onoff(0x00, false);
    g_dc_enabled = false;
    g_dc_v_target = 0.0f;
    g_dc_i_target = 0.0f;
    g_dc_v_set = 0.0f;
    g_dc_i_set = 0.0f;
}

bool dc_is_available() {
    return g_dc_available;
}
