#pragma once

#include <string>

namespace iso15118::config {

enum class TlsNegotiationStrategy {
    ACCEPT_CLIENT_OFFER = 0,
    ENFORCE_TLS = 1,
    ENFORCE_NO_TLS = 2,
};

enum class CertificateBackend {
    CUSTOM = 0,
};

struct SSLConfig {
    CertificateBackend backend{CertificateBackend::CUSTOM};
    std::string path_certificate_chain;
    std::string path_certificate_key;
    std::string private_key_password;
    std::string path_certificate_v2g_root;
    std::string path_certificate_mo_root;
    bool enable_ssl_logging{false};
    bool enable_tls_key_logging{false};
    std::string tls_key_logging_path;
};

} // namespace iso15118::config
