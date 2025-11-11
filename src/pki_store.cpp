#include "pki_store.h"

#ifdef ESP_PLATFORM
#include <Arduino.h>
#include <Preferences.h>
#endif

#include <string>

#ifdef ESP_PLATFORM
namespace {
Preferences g_pki_prefs;
bool g_pki_ready = false;
constexpr const char *kNamespace = "pki";
constexpr const char *kServerCertKey = "srv_cert";
constexpr const char *kServerKeyKey = "srv_key";
constexpr const char *kRootKey = "root_ca";

bool ensure_ready() {
    if (g_pki_ready) return true;
    g_pki_ready = g_pki_prefs.begin(kNamespace, false);
    return g_pki_ready;
}

bool read_pref(const char *key, std::string &out) {
    if (!ensure_ready()) return false;
    String val = g_pki_prefs.getString(key, "");
    if (!val.length()) return false;
    out.assign(val.c_str(), val.length());
    return true;
}

bool write_pref(const char *key, const std::string &data) {
    if (!ensure_ready()) return false;
    return g_pki_prefs.putString(key, data.c_str()) > 0;
}
}  // namespace

bool pki_store_init() {
    return ensure_ready();
}

bool pki_store_get_server_cert(std::string &out) {
    return read_pref(kServerCertKey, out);
}

bool pki_store_set_server_cert(const std::string &pem) {
    return write_pref(kServerCertKey, pem);
}

bool pki_store_get_server_key(std::string &out) {
    return read_pref(kServerKeyKey, out);
}

bool pki_store_set_server_key(const std::string &pem) {
    return write_pref(kServerKeyKey, pem);
}

bool pki_store_get_root_ca(std::string &out) {
    return read_pref(kRootKey, out);
}

bool pki_store_set_root_ca(const std::string &pem) {
    return write_pref(kRootKey, pem);
}

#else

bool pki_store_init() { return true; }
bool pki_store_get_server_cert(std::string &) { return false; }
bool pki_store_set_server_cert(const std::string &) { return false; }
bool pki_store_get_server_key(std::string &) { return false; }
bool pki_store_set_server_key(const std::string &) { return false; }
bool pki_store_get_root_ca(std::string &) { return false; }
bool pki_store_set_root_ca(const std::string &) { return false; }

#endif
