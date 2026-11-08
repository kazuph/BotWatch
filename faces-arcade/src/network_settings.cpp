#include "network_settings.h"
#include "nvs.h"
#include <cstring>

namespace network_settings {
namespace {
constexpr char kNamespace[] = "arcade_net";
struct Wifi {
    char ssid[33];
    char password[65];
};
}
void wipe(void* data, size_t size) {
    volatile char* p = static_cast<volatile char*>(data);
    while (size--) *p++ = 0;
}

esp_err_t set_wifi(const char* ssid, const char* password) {
    if (!ssid || !password || !*ssid || strlen(ssid) >= sizeof(Wifi::ssid) ||
        strlen(password) >= sizeof(Wifi::password)) return ESP_ERR_INVALID_ARG;
    Wifi wifi = {};
    memcpy(wifi.ssid, ssid, strlen(ssid));
    memcpy(wifi.password, password, strlen(password));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "wifi", &wifi, sizeof(wifi));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    wipe(&wifi, sizeof(wifi));
    return err;
}

esp_err_t set_auth_key(const char* key) {
    if (!key || !*key || strlen(key) >= sizeof(Credentials::auth_key)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "auth_key", key);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

esp_err_t clear() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

esp_err_t load(Credentials* out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    Wifi wifi = {};
    size_t size = sizeof(wifi);
    err = nvs_get_blob(handle, "wifi", &wifi, &size);
    if (err == ESP_OK && size != sizeof(wifi)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && (!memchr(wifi.ssid, 0, sizeof(wifi.ssid)) ||
                          !memchr(wifi.password, 0, sizeof(wifi.password)))) err = ESP_ERR_INVALID_SIZE;
    size = sizeof(out->auth_key);
    if (err == ESP_OK) err = nvs_get_str(handle, "auth_key", out->auth_key, &size);
    if (err == ESP_OK) {
        memcpy(out->ssid, wifi.ssid, sizeof(wifi.ssid));
        memcpy(out->password, wifi.password, sizeof(wifi.password));
    } else wipe(out, sizeof(*out));
    wipe(&wifi, sizeof(wifi));
    nvs_close(handle);
    return err;
}
}
