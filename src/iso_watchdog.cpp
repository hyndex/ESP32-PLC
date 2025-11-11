#include "iso_watchdog.h"

namespace {
uint32_t g_timeout_ms = 0;
uint8_t g_max_retries = 0;
uint8_t g_active_state = 0;
uint32_t g_state_start_ms = 0;
uint8_t g_retry_count = 0;
bool g_active = false;
}

void iso_watchdog_configure(uint32_t timeout_ms, uint8_t max_retries) {
    g_timeout_ms = timeout_ms;
    g_max_retries = max_retries;
    iso_watchdog_clear();
}

void iso_watchdog_start(uint8_t state, uint32_t now_ms) {
    g_active_state = state;
    g_state_start_ms = now_ms;
    g_active = true;
}

void iso_watchdog_clear(void) {
    g_active = false;
    g_retry_count = 0;
    g_state_start_ms = 0;
    g_active_state = 0;
}

IsoWatchdogResult iso_watchdog_check(uint32_t now_ms, uint8_t *state_out) {
    if (!g_active || g_timeout_ms == 0) return IsoWatchdogResult::Ok;
    if (state_out) *state_out = g_active_state;
    if (static_cast<int32_t>(now_ms - (g_state_start_ms + g_timeout_ms)) <= 0) {
        return IsoWatchdogResult::Ok;
    }
    g_retry_count++;
    g_active = false;
    if (g_retry_count >= g_max_retries && g_max_retries > 0) {
        return IsoWatchdogResult::Fatal;
    }
    return IsoWatchdogResult::Timeout;
}
