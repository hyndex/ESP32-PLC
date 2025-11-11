#pragma once

#include <stdbool.h>
#include <string>

bool pki_store_init();

bool pki_store_get_server_cert(std::string &out);
bool pki_store_set_server_cert(const std::string &pem);

bool pki_store_get_server_key(std::string &out);
bool pki_store_set_server_key(const std::string &pem);

bool pki_store_get_root_ca(std::string &out);
bool pki_store_set_root_ca(const std::string &pem);

