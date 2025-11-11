#pragma once

#include <stdint.h>

enum class IsoWatchdogResult : uint8_t {
    Ok = 0,
    Timeout = 1,
    Fatal = 2
};

void iso_watchdog_configure(uint32_t timeout_ms, uint8_t max_retries);
void iso_watchdog_start(uint8_t state, uint32_t now_ms);
void iso_watchdog_clear(void);
IsoWatchdogResult iso_watchdog_check(uint32_t now_ms, uint8_t *state_out);
