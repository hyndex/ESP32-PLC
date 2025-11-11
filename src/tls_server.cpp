#include "tls_server.h"

#ifdef ESP_PLATFORM

#include <errno.h>
#include <string.h>

#include <Arduino.h>

#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/err.h"

#include "evse_config.h"
#include "lwip_bridge.h"
#include "tcp.h"
#include "tls_credentials.h"

static const char *kTag = "tls15118";

static bool s_tls_ready = false;
static esp_tls_t *s_active_tls = nullptr;

static void tls_socket_send_cb(const uint8_t *data, uint16_t len) {
    if (!s_active_tls || !data || !len) return;
    size_t offset = 0;
    while (offset < len) {
        int written = esp_tls_conn_write(s_active_tls, reinterpret_cast<const char *>(data + offset), len - offset);
        if (written > 0) {
            offset += written;
            continue;
        }
        if (written == ESP_TLS_ERR_SSL_WANT_READ || written == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        ESP_LOGE(kTag, "TLS write failed (%d)", written);
        break;
    }
}

static bool build_server_cfg(esp_tls_cfg_server_t *cfg) {
    if (!cfg) return false;
    memset(cfg, 0, sizeof(*cfg));
    size_t cert_len = 0;
    size_t key_len = 0;
    size_t ca_len = 0;
    cfg->servercert_buf = evse_tls_server_cert(&cert_len);
    cfg->servercert_bytes = cert_len;
    cfg->serverkey_buf = evse_tls_server_key(&key_len);
    cfg->serverkey_bytes = key_len;
    cfg->cacert_buf = evse_tls_trusted_ca(&ca_len);
    cfg->cacert_bytes = ca_len;
    if (!cfg->servercert_buf || !cfg->serverkey_buf || cert_len == 0 || key_len == 0) {
        ESP_LOGE(kTag, "TLS credentials missing");
        return false;
    }
    return true;
}

static void close_tls_connection(esp_tls_t *tls, int sock) {
    if (tls) {
        esp_tls_conn_delete(tls);
    }
    if (sock >= 0) {
        close(sock);
    }
    if (s_active_tls == tls) {
        s_active_tls = nullptr;
    }
    tcp_register_socket_sender(nullptr);
    tcp_transport_reset();
}

static void tls_server_task(void *param) {
    esp_tls_cfg_server_t cfg{};
    if (!build_server_cfg(&cfg)) {
        ESP_LOGE(kTag, "TLS server not started (bad configuration)");
        vTaskDelete(nullptr);
        return;
    }

    while (!lwip_bridge_ready()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listen_sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(kTag, "socket() failed: %d", errno);
        vTaskDelete(nullptr);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(TCP_TLS_PORT);
    addr.sin6_addr = in6addr_any;
    if (bind(listen_sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(kTag, "bind() failed: %d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(kTag, "listen() failed: %d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    s_tls_ready = true;
    ESP_LOGI(kTag, "TLS server listening on port %u", TCP_TLS_PORT);

    while (true) {
        struct sockaddr_in6 client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        int client_sock = accept(listen_sock, reinterpret_cast<struct sockaddr *>(&client_addr), &addrlen);
        if (client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        ESP_LOGI(kTag, "TLS client connected");
        tcp_transport_reset();

        esp_tls_t *tls = esp_tls_init();
        if (!tls) {
            ESP_LOGE(kTag, "esp_tls_init failed");
            close(client_sock);
            continue;
        }

        int ret = esp_tls_server_session_create(&cfg, client_sock, tls);
        if (ret < 0) {
            ESP_LOGE(kTag, "TLS handshake failed (%d)", ret);
            close_tls_connection(tls, client_sock);
            continue;
        }

        s_active_tls = tls;
        tcp_register_socket_sender(tls_socket_send_cb);
        tcp_transport_connected();

        uint8_t buffer[1024];
        while (true) {
            int received = esp_tls_conn_read(tls, reinterpret_cast<char *>(buffer), sizeof(buffer));
            if (received == ESP_TLS_ERR_SSL_WANT_READ || received == ESP_TLS_ERR_SSL_WANT_WRITE) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (received <= 0) {
                break;
            }
            tcp_process_socket_payload(buffer, static_cast<uint16_t>(received));
        }

        ESP_LOGI(kTag, "TLS client disconnected");
        close_tls_connection(tls, client_sock);
    }
}

void tls_server_start(void) {
    static bool started = false;
    if (started) return;
    started = true;
    xTaskCreatePinnedToCore(tls_server_task, "tls15118", 8192, nullptr, 5, nullptr, 1);
}

bool tls_server_ready(void) {
    return s_tls_ready;
}

#endif  // ESP_PLATFORM
