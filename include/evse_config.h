#pragma once

#include <Arduino.h>

// === Control Pilot PWM/ADC configuration ===
#ifndef CP_PWM_PIN
#define CP_PWM_PIN        38
#endif
#ifndef CP_PWM_CHANNEL
#define CP_PWM_CHANNEL    0
#endif
#ifndef CP_PWM_FREQUENCY
#define CP_PWM_FREQUENCY  1000
#endif
#ifndef CP_PWM_RESOLUTION
#define CP_PWM_RESOLUTION 12
#endif
#ifndef CP_MAX_DUTY_CYCLE
#define CP_MAX_DUTY_CYCLE 4095
#endif
#ifndef CP_CONNECTED_DUTY_PCT
#define CP_CONNECTED_DUTY_PCT 5
#endif

#ifndef CP_ADC_PIN
#define CP_ADC_PIN        1
#endif

#ifndef CP_T12_DEFAULT_MV
#define CP_T12_DEFAULT_MV 2440
#endif
#ifndef CP_T9_DEFAULT_MV
#define CP_T9_DEFAULT_MV  2080
#endif
#ifndef CP_THRESHOLD_STEP_MV
#define CP_THRESHOLD_STEP_MV 380
#endif

#ifndef CP_SAMPLE_COUNT
#define CP_SAMPLE_COUNT    256
#endif
#ifndef CP_SAMPLE_DELAY_US
#define CP_SAMPLE_DELAY_US 6
#endif
#ifndef CP_TOPK
#define CP_TOPK            40
#endif
#ifndef CP_RING_LEN
#define CP_RING_LEN        24
#endif
#ifndef CP_B_DEMOTE_BURSTS
#define CP_B_DEMOTE_BURSTS 18
#endif

// === Contactor wiring ===
#ifndef CONTACTOR_COIL_PIN
#define CONTACTOR_COIL_PIN 7
#endif
#ifndef CONTACTOR_COIL_ACTIVE_HIGH
#define CONTACTOR_COIL_ACTIVE_HIGH 1
#endif
#ifndef CONTACTOR_AUX_PIN
#define CONTACTOR_AUX_PIN -1
#endif
#ifndef CONTACTOR_AUX_ACTIVE_HIGH
#define CONTACTOR_AUX_ACTIVE_HIGH 1
#endif

#ifndef TCP_PLAIN_PORT
#define TCP_PLAIN_PORT 15118
#endif

#ifndef TCP_TLS_PORT
#define TCP_TLS_PORT 15119
#endif

#ifndef EVSE_ID
#define EVSE_ID "DE*JOULEPOINT*EVSE*0001"
#endif

#ifndef EVSE_SERVICE_NAME
#define EVSE_SERVICE_NAME "DC Charging"
#endif

// === CAN / Maxwell module configuration ===
#ifndef CAN_CS_PIN
#define CAN_CS_PIN 13
#endif
#ifndef CAN_RST_PIN
#define CAN_RST_PIN 14
#endif
#ifndef CAN_SCK_PIN
#define CAN_SCK_PIN 7
#endif
#ifndef CAN_MOSI_PIN
#define CAN_MOSI_PIN 15
#endif
#ifndef CAN_MISO_PIN
#define CAN_MISO_PIN 16
#endif
#ifndef CAN_SPI_HOST
#define CAN_SPI_HOST HSPI
#endif
#ifndef MCP2515_CLK_MHZ
#define MCP2515_CLK_MHZ 8
#endif

#ifndef MAXWELL_MONITOR_ADDR
#define MAXWELL_MONITOR_ADDR 0x1
#endif
#ifndef MAXWELL_GROUP_DEFAULT
#define MAXWELL_GROUP_DEFAULT 0x1
#endif
#ifndef MAX_MODULES
#define MAX_MODULES 8
#endif

#ifndef DC_V_RAMP_V_PER_S
#define DC_V_RAMP_V_PER_S 50.0f
#endif
#ifndef DC_I_RAMP_A_PER_S
#define DC_I_RAMP_A_PER_S 20.0f
#endif
#ifndef DC_RAMP_TICK_MS
#define DC_RAMP_TICK_MS 100
#endif
