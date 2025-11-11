#include "diag_auth.h"

#include <string>

namespace {
std::string g_token;
uint32_t g_window_ms = 0;
uint32_t g_valid_until_ms = 0;
}

void diag_auth_init(const char *token, uint32_t window_ms) {
    g_token = token ? token : "";
    g_window_ms = window_ms;
    g_valid_until_ms = 0;
}

bool diag_auth_required(void) {
    return !g_token.empty();
}

static bool diag_auth_is_expired(uint32_t now_ms) {
    if (g_valid_until_ms == 0) return true;
    return static_cast<int32_t>(now_ms - g_valid_until_ms) > 0;
}

bool diag_auth_is_valid(uint32_t now_ms) {
    if (!diag_auth_required()) return true;
    return !diag_auth_is_expired(now_ms);
}

bool diag_auth_attempt(const char *token, uint32_t now_ms) {
    if (!diag_auth_required()) return true;
    if (!token) return false;
    if (g_token == token) {
        g_valid_until_ms = now_ms + g_window_ms;
        return true;
    }
    diag_auth_revoke();
    return false;
}

void diag_auth_revoke(void) {
    g_valid_until_ms = 0;
}
