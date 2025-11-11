#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Lightweight scaffold for the eventual QCA700x ↔ lwIP integration.
 * For now, the functions are stubs so that the rest of the firmware can
 * start invoking them without impacting current behavior.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize lwIP/QCA bridge. Safe to call even if lwIP is not yet enabled. */
void lwip_bridge_init();

/** Poll routine to be called from the 20 ms task once lwIP is enabled. */
void lwip_bridge_poll();

/** Returns true once the lwIP bridge has brought up the IPv6 netif. */
bool lwip_bridge_ready();
void lwip_bridge_on_frame(const uint8_t *frame, uint16_t len);

#ifdef __cplusplus
}
#endif
