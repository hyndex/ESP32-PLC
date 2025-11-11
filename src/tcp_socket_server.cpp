#include "tcp_socket_server.h"

#ifdef ESP_PLATFORM
#include <Arduino.h>
#include <errno.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "lwip_bridge.h"
#include "evse_config.h"
#include "tcp.h"

static int s_clientSock = -1;

static void socket_send_cb(const uint8_t *data, uint16_t len) {
    if (s_clientSock >= 0) {
        size_t offset = 0;
        while (offset < len) {
            int written = send(s_clientSock, data + offset, len - offset, 0);
            if (written > 0) {
                offset += written;
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EINTR)) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            break;
        }
    }
}

static void tcp_server_task(void *param) {
    const int port = TCP_PLAIN_PORT;
    while (!lwip_bridge_ready()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listenSock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock < 0) {
        Serial.println("[TCP] socket() failed");
        vTaskDelete(nullptr);
        return;
    }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;
    if (bind(listenSock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        Serial.println("[TCP] bind failed");
        close(listenSock);
        vTaskDelete(nullptr);
        return;
    }

    if (listen(listenSock, 1) < 0) {
        Serial.println("[TCP] listen failed");
        close(listenSock);
        vTaskDelete(nullptr);
        return;
    }
    Serial.printf("[TCP] lwIP server listening on %d\n", port);

    while (true) {
        struct sockaddr_in6 clientAddr{};
        socklen_t addrlen = sizeof(clientAddr);
        int client = accept(listenSock, (struct sockaddr *)&clientAddr, &addrlen);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        Serial.println("[TCP] client connected (lwIP)");
        s_clientSock = client;
        tcp_transport_reset();
        tcp_transport_connected();
        tcp_register_socket_sender(socket_send_cb);

        uint8_t buffer[512];
        int received;
        while ((received = recv(client, buffer, sizeof(buffer), 0)) > 0) {
            tcp_process_socket_payload(buffer, received);
        }
        close(client);
        s_clientSock = -1;
        tcp_register_socket_sender(nullptr);
        tcp_transport_reset();
        Serial.println("[TCP] client disconnected (lwIP)");
    }
}

void tcp_socket_server_start() {
    xTaskCreatePinnedToCore(tcp_server_task, "tcp15118", 8192, nullptr, 5, nullptr, 1);
}

#else
void tcp_socket_server_start() {}
#endif
