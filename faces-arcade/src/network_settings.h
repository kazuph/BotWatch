#pragma once
#include <cstddef>
#include "esp_err.h"

namespace network_settings {
// ESP-IDF Wi-Fi station fields and MicroLink's auth-key buffer include a terminator.
struct Credentials {
    char ssid[33];
    char password[65];
    char auth_key[96];
};
esp_err_t set_wifi(const char* ssid, const char* password);
esp_err_t set_auth_key(const char* key);
esp_err_t clear();
esp_err_t load(Credentials* out);
void wipe(void* data, size_t size);
}
