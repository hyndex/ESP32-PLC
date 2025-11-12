#include "cp_control.h"
#include "dc_can.h"
#include "diag_auth.h"
#include "iso15118_dc.h"
#include "iso_watchdog.h"
#include "lwip_bridge.h"
#include "pki_store.h"
#include "sdp_server.h"
#include "tcp.h"
#include "tcp_socket_server.h"
#include "tls_credentials.h"
#include "tls_server.h"

#include <cstring>

void cp_init() {}
void cp_tick() {}
char cp_get_state() { return 0; }
int cp_get_latest_mv() { return 0; }
bool cp_is_connected() { return true; }
bool cp_contactor_command(bool) { return true; }
bool cp_contactor_feedback() { return true; }
bool cp_is_contactor_commanded() { return false; }
void cp_set_pwm_manual(bool, uint16_t) {}

static float g_stub_bus_voltage = 400.0f;
static float g_stub_bus_current = 0.0f;
static float g_stub_target_voltage = 0.0f;
static float g_stub_target_current = 0.0f;
static bool g_stub_output_enabled = false;

extern "C" void dc_stub_set_bus_voltage(float voltage) {
    g_stub_bus_voltage = voltage;
}

extern "C" void dc_stub_set_bus_current(float current) {
    g_stub_bus_current = current;
}

extern "C" void dc_stub_reset_measurements(void) {
    g_stub_bus_voltage = 400.0f;
    g_stub_bus_current = 0.0f;
}

void dc_can_init() {}
void dc_can_tick() {}
bool dc_is_enabled() { return false; }
void dc_enable_output(bool on) { g_stub_output_enabled = on; }
void dc_set_targets(float voltage, float current) {
    g_stub_target_voltage = voltage;
    g_stub_target_current = current;
}
float dc_get_bus_voltage() { return g_stub_bus_voltage; }
float dc_get_bus_current() { return g_stub_bus_current; }

void lwip_bridge_init() {}
void lwip_bridge_poll() {}
bool lwip_bridge_ready() { return true; }
void lwip_bridge_on_frame(const uint8_t *, uint16_t) {}

bool pki_store_init() { return true; }
bool pki_store_set_server_cert(const std::string &) { return true; }
bool pki_store_set_server_key(const std::string &) { return true; }
bool pki_store_set_root_ca(const std::string &) { return true; }
bool pki_store_get_server_cert(std::string &) { return false; }
bool pki_store_get_server_key(std::string &) { return false; }
bool pki_store_get_root_ca(std::string &) { return false; }

void tls_credentials_reload() {}

void iso20_init() {}
void iso20_loop() {}

extern "C" int esp_read_mac(uint8_t *mac, int) {
    std::memset(mac, 0, 6);
    return 0;
}
