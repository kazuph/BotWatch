#pragma once
#include <stddef.h>
#include <stdint.h>

bool rubyBleStart();
void rubyBleStop();
void rubyBleNotify(const char* text);
void rubyBleLog(const void* data, size_t size);
// Takes ownership of the PSRAM buffer only on success.
bool rubySubmit(uint8_t* data, size_t size);
