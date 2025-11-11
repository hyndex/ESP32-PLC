#pragma once

void dc_can_init();
void dc_can_tick();

void dc_enable_output(bool enable);
bool dc_is_enabled();

void dc_set_targets(float voltage_v, float current_a);
float dc_get_set_voltage();
float dc_get_set_current();

float dc_get_bus_voltage();
float dc_get_bus_current();

void dc_emergency_stop();
bool dc_is_available();
