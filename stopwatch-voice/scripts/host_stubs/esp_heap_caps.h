// Host-test stand-in for ESP-IDF's esp_heap_caps.h (scripts/test_home_parse.cpp only).
#pragma once
#include <cstdlib>
inline void heap_caps_free(void *pointer) { std::free(pointer); }
