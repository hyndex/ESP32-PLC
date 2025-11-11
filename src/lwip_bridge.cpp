#include "lwip_bridge.h"

#include <Arduino.h>
#include <string.h>

#include "main.h"

static bool s_bridge_ready = false;

#ifdef ESP_PLATFORM
#include "esp_netif.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"

static struct netif s_qca_netif;
static bool s_tcpip_started = false;

static err_t qca_linkoutput(struct netif *netif, struct pbuf *p) {
    uint16_t len = p->tot_len;
    if (len > QCA7K_BUFFER_SIZE) {
        return ERR_MEM;
    }
    if (pbuf_copy_partial(p, txbuffer, len, 0) != len) {
        return ERR_BUF;
    }
    qcaspi_write_burst(txbuffer, len);
    return ERR_OK;
}

static err_t qca_netif_init(struct netif *netif) {
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, myMac, 6);
    netif->mtu = 1500;
    netif->name[0] = 'q';
    netif->name[1] = 'c';
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_MLD6;
    netif->linkoutput = qca_linkoutput;
    netif->output = etharp_output;
    netif->output_ip6 = ethip6_output;
    return ERR_OK;
}
#endif

void lwip_bridge_init() {
#ifdef ESP_PLATFORM
    if (!s_tcpip_started) {
        esp_netif_init();
        tcpip_init(nullptr, nullptr);
        s_tcpip_started = true;
    }
    netif_add(&s_qca_netif, nullptr, nullptr, nullptr, nullptr, qca_netif_init, tcpip_input);
    netif_set_default(&s_qca_netif);
    netif_set_up(&s_qca_netif);
    netif_set_link_up(&s_qca_netif);
    netif_create_ip6_linklocal_address(&s_qca_netif, 1);
    s_bridge_ready = true;
#else
    s_bridge_ready = false;
#endif
}

void lwip_bridge_poll() {
    // Placeholder for future TX/RX queue handling.
}

bool lwip_bridge_ready() {
    return s_bridge_ready;
}

void lwip_bridge_on_frame(const uint8_t *frame, uint16_t len) {
#ifdef ESP_PLATFORM
    if (!s_bridge_ready || !frame || len == 0) return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (!p) return;
    if (pbuf_take(p, frame, len) != ERR_OK) {
        pbuf_free(p);
        return;
    }
    if (s_qca_netif.input(p, &s_qca_netif) != ERR_OK) {
        pbuf_free(p);
    }
#else
    (void)frame;
    (void)len;
#endif
}
