#include "tls_credentials.h"

#include <Arduino.h>
#include <cstring>
#include <string>
#include <pgmspace.h>

#include "pki_store.h"

static const char kDefaultTlsCertPem[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIICpjCCAY4CCQCzxHPBzfzZ2DANBgkqhkiG9w0BAQsFADAVMRMwEQYDVQQDDApF
U1AzMi1TRUNDMB4XDTI1MTExMTE5NTAyMFoXDTM1MTEwOTE5NTAyMFowFTETMBEG
A1UEAwwKRVNQMzItU0VDQzCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEB
AMUEwGsgr+cjHfsgHM9jG4wh2C2J1q/Zlbho1vZHfGMlPkHUYBSC8xE2/cnFnGQo
Xreu1flcuHK1UaLZomsNuUX0YCJ6ZGaxSiZzCH6EEctZrARrxmYE+uOp2sXUb1qc
64uvsaBeBXCgiYtxgLKuaRa8cqkxDEZuemjZqCtmYGQYEO+yQ/4FBLtuVYJASymC
6+Ntv8KzG1mYJQixsT/MHNRsStuLWrgVAFaOQ28SgpTE5fHICL8TkRhQVSxkkDaN
g91ZujiOnX8ML65eCBCb03zkrQEUY7Yd/itESVwe2/3aWP5L/xoI/J5ugKP418AP
X8FxvB2n12To9hK+ieZv4P0CAwEAATANBgkqhkiG9w0BAQsFAAOCAQEArj8M31jd
9eNczveEU7s08rRBpHmZxz3dwPCVhwKu2Ck6zX+gtFa2NfW5+166KIS7mga4fbJA
PRv/VIfuoYsTEoXN9cHRfu9TdJJvrdRy6XD8atJiqovKOfeShW3At4VAQ/68R6EW
R1y5ywFdMNeH9NJnErmlufiqgWwPqy2MmlLuWr5wS+0L8dOT+mC/h93uZSk3yEWg
i9KCVf/W2D556oiKL1z27xeJreK8Y5lv+id8RrnmDFok45eFTTJ7zOG0WhYt7Y+Z
V8K7sfDfkZoFLP+DGodB9aBdUch0OqeRNNkdXOJwWcbeNmwZevaqSuEWyRZDlnBu
SjzuFF1fCT3N7w==
-----END CERTIFICATE-----
)PEM";

static const char kDefaultTlsKeyPem[] PROGMEM = R"PEM(
-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDFBMBrIK/nIx37
IBzPYxuMIdgtidav2ZW4aNb2R3xjJT5B1GAUgvMRNv3JxZxkKF63rtX5XLhytVGi
2aJrDblF9GAiemRmsUomcwh+hBHLWawEa8ZmBPrjqdrF1G9anOuLr7GgXgVwoImL
cYCyrmkWvHKpMQxGbnpo2agrZmBkGBDvskP+BQS7blWCQEspguvjbb/CsxtZmCUI
sbE/zBzUbErbi1q4FQBWjkNvEoKUxOXxyAi/E5EYUFUsZJA2jYPdWbo4jp1/DC+u
XggQm9N85K0BFGO2Hf4rRElcHtv92lj+S/8aCPyeboCj+NfAD1/Bcbwdp9dk6PYS
vonmb+D9AgMBAAECggEAKkLsq44lbWVBBx9KTsopnJOd9Za9sJbx4M7MXaPT8MiK
ZECUI5I7ZZPwOJnlBC8MskYUrBrRjfmK+23Hw0L8XR8giATNCKI7D6hZSBo4XvBr
T792nWewEanbvdfl0wAaHqqfZZEFhbVKC9lbC/kRncjqp5RX17vXEiquQjEBuDZ4
c81HU5LuRDFM9ki0mcLuRAkegtHGMgXK+Y0bXIEF6B4QFiP9Gz+yaGQrBj//RrYp
NWoT+thKC+2kCQTEPLFVuLQZkINeHe/GU2NPWkBC1LqTpEv3EukOeaA7mxJs7mhl
XMhKyJ2RRFVYjP5w8YlVWN0yFV241BVS6gcydqpggQKBgQD6DDaR3jgUajMPDdS8
RXrbwUdOZLiYX2MhPj6yuAgwxUngXFCH5sN5zPzekpO2qP4l/MHm7ZmaGzPZWB+C
jQ4Qu94GX7ZlYr86eLiAiKsBxof1/OtNLoaLNFpzsG1/I1du9tXCMDuF4P2va8iR
vmpaMCqIPrmdCfSTWYayVAoizQKBgQDJtWEykF8rjZaYzyZX0HtxW9Cpmf6Sfwh0
TkpCbOFFA4RPisYSrT9boeLp7zJlF8hake/H6mIX1gmnquGjiuA/fFsQmVi8XcAU
ErCD0vlbumpm/AJrvN9au3+ldmi0I3AEHwhBfFo7OwtCvHMt3Cmvid/4NCh/TYyR
pRyp1IWW8QKBgBQ+jOR+c1fjyUJ8wi6ECZBlM5q7ON7NSj9UxMq/b5pTPsn1b2ex
XT4tRIPYpjDxubHlpuVFc1wwu5/rLJHrTao5K56kfSX0OrtHLtjpN78rDbLyyTI0
hBwdHv2i2RqkB94qCeBw+0C5mJBtT43NNtsabzccrPZz6eNMKkWh3Dg5AoGAO/rT
jFWh9zGDNq1imXpFOtAynDxOlwFvYiZlrprx4bPKBF0fyS37SSQ6dZXLRoRr74K1
6pynzq628ETAFAGX7UjtS2JOILVACLgGBS0XOU+VlEob7i2bvT9EFc/AEtD23kLc
EZ0It9Q25QFkvp5ZRvmYwBXCdRh6VFTk0RuBHgECgYEA6hz+wm3j9THTbE0d/vbL
udmQ5M08+Lh6x4xxeTdPR29QPk6+3Ic+J7lj5xGzMPRNVRJnSWVjPdF7PbB3uJKH
2AD56FwAgW1diwbhCOLGF+BtPRKZsLmUY2dDFiTmd7cLTRiq8qeUEGzlHF+IQ10m
NNMrBFOs5mq+dqwTUogbm8Q=
-----END PRIVATE KEY-----
)PEM";

#ifdef ESP_PLATFORM
const unsigned char *evse_tls_server_cert(size_t *len) __attribute__((weak));
const unsigned char *evse_tls_server_key(size_t *len) __attribute__((weak));
const unsigned char *evse_tls_trusted_ca(size_t *len) __attribute__((weak));
#endif

namespace {
std::string g_server_cert;
std::string g_server_key;
std::string g_trusted_ca;
bool g_cert_loaded = false;
bool g_key_loaded = false;
bool g_ca_loaded = false;

void ensure_server_cert() {
    if (g_cert_loaded) return;
    std::string pem;
    if (pki_store_get_server_cert(pem)) {
        g_server_cert = pem;
    } else {
        g_server_cert.assign(kDefaultTlsCertPem, strlen_P(kDefaultTlsCertPem));
    }
    g_cert_loaded = true;
}

void ensure_server_key() {
    if (g_key_loaded) return;
    std::string pem;
    if (pki_store_get_server_key(pem)) {
        g_server_key = pem;
    } else {
        g_server_key.assign(kDefaultTlsKeyPem, strlen_P(kDefaultTlsKeyPem));
    }
    g_key_loaded = true;
}

void ensure_trusted_ca() {
    if (g_ca_loaded) return;
    std::string pem;
    if (pki_store_get_root_ca(pem)) {
        g_trusted_ca = pem;
    } else {
        g_trusted_ca.clear();
    }
    g_ca_loaded = true;
}
}  // namespace

const unsigned char *evse_tls_server_cert(size_t *len) {
    ensure_server_cert();
    if (len) *len = g_server_cert.size();
    return reinterpret_cast<const unsigned char *>(g_server_cert.c_str());
}

const unsigned char *evse_tls_server_key(size_t *len) {
    ensure_server_key();
    if (len) *len = g_server_key.size();
    return reinterpret_cast<const unsigned char *>(g_server_key.c_str());
}

const unsigned char *evse_tls_trusted_ca(size_t *len) {
    ensure_trusted_ca();
    if (g_trusted_ca.empty()) {
        if (len) *len = 0;
        return nullptr;
    }
    if (len) *len = g_trusted_ca.size();
    return reinterpret_cast<const unsigned char *>(g_trusted_ca.c_str());
}

void tls_credentials_reload(void) {
    g_cert_loaded = false;
    g_key_loaded = false;
    g_ca_loaded = false;
}
