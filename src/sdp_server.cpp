#include "sdp_server.h"

#ifdef ESP_PLATFORM

#include <Arduino.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "lwip_bridge.h"
#include "ipv6.h"

static void sdp_server_task(void *param) {
    const int kPort = 15118;
    while (!lwip_bridge_ready()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        Serial.println("[SDP] socket() failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(kPort);
    addr.sin6_addr = in6addr_any;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        Serial.println("[SDP] bind() failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    Serial.println("[SDP] UDP server ready on port 15118 (lwIP)");
    while (true) {
        uint8_t buffer[256];
        struct sockaddr_in6 src{};
        socklen_t srclen = sizeof(src);
        int received = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&src, &srclen);
        if (received < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (received < 10) continue;

        // Check V2GTP header
        if (buffer[0] != 0x01 || buffer[1] != 0xFE) continue;
        uint16_t payloadType = (buffer[2] << 8) | buffer[3];
        if (payloadType != 0x9000) continue;
        uint32_t payloadLen = ((uint32_t)buffer[4] << 24) |
                              ((uint32_t)buffer[5] << 16) |
                              ((uint32_t)buffer[6] << 8) |
                              (uint32_t)buffer[7];
        if (payloadLen != 2 || received < 10) continue;

        const uint8_t *payload = buffer + 8;
        // Update global EV IP/port to reuse existing state machines.
        memcpy(EvccIp, src.sin6_addr.s6_addr, 16);
        evccPort = ntohs(src.sin6_port);

        if (!handleSdpRequestBuffer(payload, payloadLen, src.sin6_addr.s6_addr, ntohs(src.sin6_port))) {
            continue;
        }

        uint8_t response[64];
        uint16_t respLen = buildSdpResponseFrame(response, sizeof(response));
        if (respLen == 0) continue;

        sendto(sock, response, respLen, 0, (struct sockaddr *)&src, srclen);
    }
}

void sdp_server_start() {
    xTaskCreatePinnedToCore(sdp_server_task, "sdp_udp", 4096, nullptr, 4, nullptr, 1);
}

#else
void sdp_server_start() {}
#endif
