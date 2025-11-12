cmake_minimum_required(VERSION 3.22)
project(slac_flow_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

set(CBV2G_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../../lib/libcbv2g)
set(CB_V2G_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CB_V2G_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CBV2G_ROOT_DIR} ${CMAKE_CURRENT_BINARY_DIR}/cbv2g_lib)

set(firmware_sources
    ../../src/main.cpp
    ../../src/tcp.cpp
    ../../src/ipv6.cpp
    ../../src/iso_watchdog.cpp
    ../../src/diag_auth.cpp
)

add_library(firmware_under_test OBJECT
    ${firmware_sources}
)

target_compile_definitions(firmware_under_test PRIVATE UNIT_TEST APP_NO_MAIN)
target_include_directories(firmware_under_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ../../include
    ../../src
    ../../lib/libcbv2g/include
)

add_library(test_stubs
    stubs/arduino_stubs.cpp
    stubs/peripheral_stubs.cpp
)
target_include_directories(test_stubs PRIVATE
    ../../include
    ../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ../../lib/libcbv2g/include
)

target_link_libraries(firmware_under_test PRIVATE
    test_stubs
    cbv2g_din
    cbv2g_iso2
    cbv2g_tp
)

add_executable(slac_flow_gtest
    slac_flow_test.cpp
    iso_flow_test.cpp
)

target_include_directories(slac_flow_gtest PRIVATE
    ../../include
    ../../lib/libcbv2g/include
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_link_libraries(slac_flow_gtest PRIVATE
    firmware_under_test
    test_stubs
    cbv2g_din
    cbv2g_iso2
    cbv2g_tp
    GTest::gtest_main
)

target_compile_definitions(slac_flow_gtest PRIVATE
    DEMO_CHARGING_LOG_PATH="${CMAKE_CURRENT_LIST_DIR}/../../temp/ccs32berta/doc/2023-07-04_demoChargingWorks.log"
)

include(GoogleTest)
gtest_discover_tests(slac_flow_gtest)
