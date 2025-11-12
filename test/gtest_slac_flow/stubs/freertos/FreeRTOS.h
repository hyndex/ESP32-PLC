#pragma once

#include <cstdint>

using BaseType_t = int;
using UBaseType_t = unsigned;
using StackType_t = uint32_t;
using TaskFunction_t = void (*)(void *);

#define pdPASS 1
#define portTICK_PERIOD_MS 1
