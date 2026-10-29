#pragma once
#include <cstddef>
namespace tts {
// The existing home/action captions use char[96]; storage is reserved at boot.
constexpr size_t kCaptionBytes = 96;
size_t kana_capacity();
bool intermediate(const char *caption, char *output, size_t capacity, float *gain = nullptr);
}
