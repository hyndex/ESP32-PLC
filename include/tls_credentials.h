#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return pointer + size of the SECC default TLS certificate in PEM format.
 *        Applications may override these weak definitions to load certs from NVS/FS.
 */
const unsigned char *evse_tls_server_cert(size_t *len);

/**
 * @brief Return pointer + size of the SECC default TLS private key in PEM format.
 *        Applications may override these weak definitions to load keys securely.
 */
const unsigned char *evse_tls_server_key(size_t *len);

/**
 * @brief Return pointer + size of the trusted CA bundle used to verify EV certificates (optional).
 */
const unsigned char *evse_tls_trusted_ca(size_t *len);

void tls_credentials_reload(void);

#ifdef __cplusplus
}
#endif
