#include <Arduino.h>
#include <unity.h>

#include "diag_auth.h"
#include "iso_watchdog.h"

void setUp() {
    // Runs before each test
}

void tearDown() {
}

void test_diag_auth_optional(void) {
    diag_auth_init("", 1000);
    TEST_ASSERT_FALSE(diag_auth_required());
    TEST_ASSERT_TRUE(diag_auth_is_valid(0));
    TEST_ASSERT_TRUE(diag_auth_attempt("", 0));
}

void test_diag_auth_success_and_expiry(void) {
    diag_auth_init("token", 1000);
    TEST_ASSERT_TRUE(diag_auth_required());
    TEST_ASSERT_FALSE(diag_auth_attempt("bad", 0));
    TEST_ASSERT_FALSE(diag_auth_is_valid(10));
    TEST_ASSERT_TRUE(diag_auth_attempt("token", 100));
    TEST_ASSERT_TRUE(diag_auth_is_valid(500));
    TEST_ASSERT_FALSE(diag_auth_is_valid(1200));
}

void test_diag_auth_revoke(void) {
    diag_auth_init("abc", 500);
    TEST_ASSERT_TRUE(diag_auth_attempt("abc", 0));
    TEST_ASSERT_TRUE(diag_auth_is_valid(100));
    diag_auth_revoke();
    TEST_ASSERT_FALSE(diag_auth_is_valid(200));
}

void test_iso_watchdog_timeout_and_fatal(void) {
    iso_watchdog_configure(100, 2);
    iso_watchdog_clear();
    iso_watchdog_start(7, 0);
    uint8_t state = 0;
    TEST_ASSERT_EQUAL(IsoWatchdogResult::Ok, iso_watchdog_check(50, &state));
    TEST_ASSERT_EQUAL(0, state);
    TEST_ASSERT_EQUAL(IsoWatchdogResult::Timeout, iso_watchdog_check(200, &state));
    TEST_ASSERT_EQUAL(7, state);
    iso_watchdog_start(8, 300);
    TEST_ASSERT_EQUAL(IsoWatchdogResult::Fatal, iso_watchdog_check(450, &state));
    TEST_ASSERT_EQUAL(8, state);
}

void test_iso_watchdog_clear(void) {
    iso_watchdog_configure(100, 1);
    iso_watchdog_start(3, 0);
    iso_watchdog_clear();
    uint8_t state = 0;
    TEST_ASSERT_EQUAL(IsoWatchdogResult::Ok, iso_watchdog_check(500, &state));
    TEST_ASSERT_EQUAL(0, state);
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_diag_auth_optional);
    RUN_TEST(test_diag_auth_success_and_expiry);
    RUN_TEST(test_diag_auth_revoke);
    RUN_TEST(test_iso_watchdog_timeout_and_fatal);
    RUN_TEST(test_iso_watchdog_clear);
    UNITY_END();
}

void loop() {
}

#ifdef ESP_PLATFORM
extern "C" void app_main() {
    initArduino();
    setup();
    while (true) {
        loop();
        vTaskDelay(1);
    }
}
#endif
