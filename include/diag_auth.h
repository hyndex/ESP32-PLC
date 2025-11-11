#pragma once

#include <stdint.h>

void diag_auth_init(const char *token, uint32_t window_ms);
bool diag_auth_required(void);
bool diag_auth_attempt(const char *token, uint32_t now_ms);
bool diag_auth_is_valid(uint32_t now_ms);
void diag_auth_revoke(void);
