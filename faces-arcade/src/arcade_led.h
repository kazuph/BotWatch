#pragma once
#include <cstdint>
#include "esp_err.h"
namespace arcade_led {
enum class Color { off, red, green, white };
esp_err_t begin();
esp_err_t show(Color color);
esp_err_t green_level(uint8_t level);
esp_err_t countdown(uint32_t elapsed_ms);
esp_err_t clear();
}
