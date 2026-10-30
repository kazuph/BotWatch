/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "esp_now_radio.h"
#include <algorithm>
#include <cstring>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_now.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_log.h>

namespace transceiver {
namespace {

constexpr uint8_t broadcastAddress[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr const char* tag                            = "EspNowRadio";

bool addPeerInternal(const uint8_t* address)
{
    if (esp_now_is_peer_exist(address)) {
        return true;
    }

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, address, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

}  // namespace

EspNowRadio* EspNowRadio::_instance = nullptr;

bool EspNowRadio::start()
{
    if (_started) {
        return true;
    }

    _send_ready.store(true);
    _last_send_success.store(false);
    _send_failures.store(0);
    _receive_drops.store(0);

    const esp_err_t netif_result = esp_netif_init();
    if (netif_result != ESP_OK && netif_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(tag, "esp_netif_init: %s", esp_err_to_name(netif_result));
        return false;
    }
    const esp_err_t event_result = esp_event_loop_create_default();
    if (event_result != ESP_OK && event_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(tag, "esp_event_loop_create_default: %s", esp_err_to_name(event_result));
        return false;
    }

    wifi_init_config_t config   = WIFI_INIT_CONFIG_DEFAULT();
    const esp_err_t init_result = esp_wifi_init(&config);
    if (init_result != ESP_OK && init_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(tag, "esp_wifi_init: %s", esp_err_to_name(init_result));
        return false;
    }

    esp_wifi_stop();
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK) {
        result = esp_wifi_start();
    }
    if (result == ESP_OK) {
        result = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (result == ESP_OK) {
        result = esp_now_init();
    }
    if (result != ESP_OK) {
        ESP_LOGE(tag, "radio start: %s", esp_err_to_name(result));
        stop();
        return false;
    }

    _receive_queue = xQueueCreate(transportBufferFrames, sizeof(ReceivedPacket));
    if (_send_complete == nullptr) {
        _send_complete = xSemaphoreCreateBinary();
    }
    if (_receive_queue == nullptr || _send_complete == nullptr) {
        if (_receive_queue != nullptr) {
            vQueueDelete(_receive_queue);
            _receive_queue = nullptr;
        }
        stop();
        return false;
    }

    _instance = this;
    if (esp_now_register_recv_cb(&EspNowRadio::onReceive) != ESP_OK ||
        esp_now_register_send_cb(&EspNowRadio::onSend) != ESP_OK || !addPeerInternal(broadcastAddress)) {
        stop();
        return false;
    }

    _started = true;
    ESP_LOGI(tag, "started open ESP-NOW transport");
    return true;
}

void EspNowRadio::stop()
{
    _started = false;
    _last_send_success.store(false);
    if (_send_complete != nullptr) {
        xSemaphoreGive(_send_complete);
    }
    if (_instance == this) {
        _instance = nullptr;
    }
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    esp_wifi_stop();
    if (_receive_queue != nullptr) {
        vQueueDelete(_receive_queue);
        _receive_queue = nullptr;
    }
    esp_wifi_deinit();
    if (_send_complete != nullptr) {vSemaphoreDelete(_send_complete);_send_complete=nullptr;}
    _send_ready.store(true);
}

bool EspNowRadio::addPeer(const MacAddress& peer)
{
    return _started && addPeerInternal(peer.data());
}

bool EspNowRadio::sendBroadcast(const void* data, std::size_t length)
{
    return send(broadcastAddress, data, length);
}

bool EspNowRadio::sendTo(const MacAddress& peer, const void* data, std::size_t length)
{
    return addPeer(peer) && send(peer.data(), data, length);
}

bool EspNowRadio::sendToBlocking(const MacAddress& peer, const void* data, std::size_t length)
{
    if (_send_complete == nullptr) {
        return false;
    }
    xSemaphoreTake(_send_complete, 0);
    if (!sendTo(peer, data, length)) {
        return false;
    }
    if (xSemaphoreTake(_send_complete, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    return _started && _last_send_success.load();
}

bool EspNowRadio::send(const uint8_t* destination, const void* data, std::size_t length)
{
    if (!_started || data == nullptr || length == 0 || length > ESP_NOW_MAX_DATA_LEN || !_send_ready.exchange(false)) {
        return false;
    }
    if (esp_now_send(destination, static_cast<const uint8_t*>(data), length) != ESP_OK) {
        _send_ready.store(true);
        return false;
    }
    return true;
}

bool EspNowRadio::receive(ReceivedPacket& packet)
{
    return _receive_queue != nullptr && xQueueReceive(_receive_queue, &packet, 0) == pdTRUE;
}

void EspNowRadio::onReceive(const esp_now_recv_info* info, const uint8_t* data, int length)
{
    if (_instance == nullptr || _instance->_receive_queue == nullptr || info == nullptr || info->src_addr == nullptr ||
        data == nullptr || length <= 0 || length > static_cast<int>(maxPayloadSize)) {
        return;
    }

    ReceivedPacket packet;
    std::copy_n(info->src_addr, packet.sender.size(), packet.sender.begin());
    packet.length = static_cast<uint16_t>(length);
    std::copy_n(data, packet.length, packet.payload.begin());
    if (xQueueSend(_instance->_receive_queue, &packet, 0) != pdTRUE) {
        _instance->_receive_drops.fetch_add(1);
    }
}

void EspNowRadio::onSend(const esp_now_send_info_t*, esp_now_send_status_t status)
{
    if (_instance != nullptr) {
        if (status != ESP_NOW_SEND_SUCCESS) {
            _instance->_send_failures.fetch_add(1);
        }
        _instance->_last_send_success.store(status == ESP_NOW_SEND_SUCCESS);
        _instance->_send_ready.store(true);
        if (_instance->_send_complete != nullptr) {
            xSemaphoreGive(_instance->_send_complete);
        }
    }
}

}  // namespace transceiver
