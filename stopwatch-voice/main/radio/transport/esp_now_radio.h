/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "packets.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace transceiver {

class EspNowRadio {
public:
    static constexpr std::size_t maxPayloadSize = 250;

    struct ReceivedPacket {
        MacAddress sender                           = {};
        uint16_t length                             = 0;
        std::array<uint8_t, maxPayloadSize> payload = {};
    };

    bool start();
    void stop();
    bool addPeer(const MacAddress& peer);
    bool sendBroadcast(const void* data, std::size_t length);
    bool sendTo(const MacAddress& peer, const void* data, std::size_t length);
    bool sendToBlocking(const MacAddress& peer, const void* data, std::size_t length);
    bool receive(ReceivedPacket& packet);
    bool isStarted() const
    {
        return _started;
    }
    bool isSendReady() const
    {
        return _send_ready.load();
    }
    uint32_t sendFailureCount() const
    {
        return _send_failures.load();
    }
    uint32_t receiveDropCount() const
    {
        return _receive_drops.load();
    }

private:
    static void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length);
    static void onSend(const esp_now_send_info_t* info, esp_now_send_status_t status);
    bool send(const uint8_t* destination, const void* data, std::size_t length);

    static EspNowRadio* _instance;
    QueueHandle_t _receive_queue         = nullptr;
    SemaphoreHandle_t _send_complete     = nullptr;
    std::atomic<bool> _send_ready        = true;
    std::atomic<bool> _last_send_success = false;
    std::atomic<uint32_t> _send_failures = 0;
    std::atomic<uint32_t> _receive_drops = 0;
    bool _started                        = false;
};

}  // namespace transceiver
