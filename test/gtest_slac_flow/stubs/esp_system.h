#pragma once

#include <cstdint>

#define ESP_MAC_ETH 0

extern "C" int esp_read_mac(uint8_t *mac, int type);
