#pragma once
#include <cstddef>
#include <cstdint>
namespace radio_app {
size_t audio_memory_bytes();
bool start();
void stop();
bool update(bool pressed,int volume);
const char *status();
bool microphone_released(void *data,size_t samples);
void drop_next_talk_stop();
void key(bool pressed);
void inject(const int16_t *samples,size_t count);
void stats();
}
