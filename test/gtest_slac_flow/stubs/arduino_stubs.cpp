#include "Arduino.h"
#include "SPI.h"

#include <atomic>

SerialStub Serial;
SPIClass SPI;

static std::atomic<unsigned long> g_stub_millis{0};
static unsigned long lcg_state = 1;

unsigned long millis() {
    return g_stub_millis.load();
}

void slac_test_advance_time(unsigned long ms) {
    g_stub_millis.fetch_add(ms);
}

void slac_test_set_millis(unsigned long value) {
    g_stub_millis.store(value);
}

long random(long max) {
    if (max <= 0) return 0;
    lcg_state = lcg_state * 1103515245 + 12345;
    return static_cast<long>((lcg_state >> 16) % max);
}

long random(long min, long max) {
    if (max <= min) return min;
    long span = max - min;
    return min + random(span);
}
