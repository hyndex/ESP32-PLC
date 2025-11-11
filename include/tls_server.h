#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
void tls_server_start(void);
bool tls_server_ready(void);
#else
static inline void tls_server_start(void) {}
static inline bool tls_server_ready(void) { return false; }
#endif

#ifdef __cplusplus
}
#endif

