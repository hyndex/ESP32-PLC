#pragma once

#include <cstdint>

class SPISettings {
public:
    SPISettings(uint32_t, uint8_t, uint8_t) {}
};

class SPIClass {
public:
    void begin(int = 0, int = 0, int = 0, int = 0) {}
    void beginTransaction(const SPISettings &) {}
    uint8_t transfer(uint8_t data) { return data; }
    void transfer(uint8_t *, size_t) {}
    uint16_t transfer16(uint16_t data) { return data; }
};

extern SPIClass SPI;
