#pragma once

#include <cstdint>
#include <cstdarg>
#include <cstdio>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}

void slac_test_advance_time(unsigned long ms);
void slac_test_set_millis(unsigned long value);

class SerialStub {
public:
    void begin(unsigned long = 0) {}
    void printf(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stdout, fmt, args);
        va_end(args);
    }
    void print(const char *s) { std::fputs(s, stdout); }
    void println(const char *s = "") {
        std::fputs(s, stdout);
        std::fputc('\n', stdout);
    }
};

extern SerialStub Serial;

unsigned long millis();
inline void delay(unsigned long ms) {
    extern void slac_test_advance_time(unsigned long ms);
    slac_test_advance_time(ms);
}

inline void randomSeed(unsigned long) {}
long random(long max);
long random(long min, long max);

#define PROGMEM
