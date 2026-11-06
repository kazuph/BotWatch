#include "network_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <algorithm>
#include <cstring>

namespace network_wifi {
namespace {
constexpr EventBits_t kConnected = BIT0;
constexpr EventBits_t kDisconnected = BIT1;
EventGroupHandle_t events = nullptr;
esp_netif_t* netif = nullptr;
esp_event_handler_instance_t wifi_handler = nullptr, ip_handler = nullptr;
bool netif_initialized = false;
bool wifi_initialized = false;
bool wifi_started = false;
std::atomic<unsigned> disconnect_reason{0};

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
        disconnect_reason.store(event ? unsigned(event->reason) : 0U);
        xEventGroupSetBits(events, kDisconnected);
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(events, kConnected);
}
}
void stop() {
    if (wifi_started) { esp_wifi_disconnect(); esp_wifi_stop(); wifi_started = false; }
    if (wifi_handler) { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler); wifi_handler = nullptr; }
    if (ip_handler) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler); ip_handler = nullptr; }
    if (wifi_initialized) { esp_wifi_deinit(); wifi_initialized = false; }
    if (netif) { esp_netif_destroy_default_wifi(netif); netif = nullptr; }
    if (events) { vEventGroupDelete(events); events = nullptr; }
}
Result connect(const network_settings::Credentials& credentials, std::atomic<bool>& cancelled, int64_t deadline_us) {
    stop();
    esp_log_level_set("wifi", ESP_LOG_NONE);
    events = xEventGroupCreate();
    if (!events) return Result::error;
    esp_err_t err = ESP_OK;
    if (!netif_initialized) {
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { stop(); return Result::error; }
        netif_initialized = true;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { stop(); return Result::error; }
    netif = esp_netif_create_default_wifi_sta();
    if (!netif) { stop(); return Result::error; }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) { stop(); return Result::error; }
    wifi_initialized = true;
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr, &wifi_handler) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr, &ip_handler) != ESP_OK) {
        stop(); return Result::error;
    }
    wifi_config_t config = {};
    memcpy(config.sta.ssid, credentials.ssid, std::min(strlen(credentials.ssid), sizeof(config.sta.ssid)));
    memcpy(config.sta.password, credentials.password, std::min(strlen(credentials.password), sizeof(config.sta.password)));
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &config);
    network_settings::wipe(&config, sizeof(config));
    if (err != ESP_OK || esp_wifi_start() != ESP_OK) { stop(); return Result::error; }
    wifi_started = true;
    if (esp_wifi_connect() != ESP_OK) { stop(); return Result::error; }
    for (;;) {
        if (cancelled.load()) { stop(); return Result::cancelled; }
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) { stop(); return Result::timeout; }
        const auto bits = xEventGroupWaitBits(events, kConnected | kDisconnected, pdFALSE, pdFALSE,
                                             pdMS_TO_TICKS(std::min<int64_t>((remaining_us + 999) / 1000, 100)));
        if (bits & kConnected) return Result::connected;
        if (bits & kDisconnected) {
            printf("NETWORK wifi_disconnected reason=%u\n", disconnect_reason.load());
            stop();
            return Result::disconnected;
        }
    }
}
}
