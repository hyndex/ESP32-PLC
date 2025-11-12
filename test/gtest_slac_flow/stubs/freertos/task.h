#pragma once

#include "FreeRTOS.h"

inline BaseType_t xTaskCreate(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, void *) {
    return pdPASS;
}

inline void vTaskDelay(uint32_t) {}
