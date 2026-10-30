#pragma once
#include <cstdint>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
namespace apps {
bool reserve_audio_memory();
void tick();
bool microphone_released(void *data, size_t samples);
// Voice modes are 0/1; 2 requests the light radio boot.
int run(StreamBufferHandle_t serial, const std::function<bool(uint8_t *, int)> &read,
        const std::function<void()> &capture,bool radio_only=false);
}
