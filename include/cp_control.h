#pragma once

#include <stdint.h>

void cp_init();
void cp_tick();

char cp_get_state();
int  cp_get_latest_mv();
bool cp_is_connected();

bool cp_contactor_command(bool on);
bool cp_contactor_feedback();
bool cp_is_contactor_commanded();

void cp_set_pwm_manual(bool enable, uint16_t duty_pct);
