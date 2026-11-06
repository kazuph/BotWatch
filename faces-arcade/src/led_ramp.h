#pragma once
#include <cstdint>

namespace led_ramp {
constexpr uint8_t maximum = 64;
// Even when the first step follows a long idle, 63 further steps take at least one second.
constexpr int64_t step_us = (1000000 + maximum - 2) / (maximum - 1);
constexpr uint8_t countdown_target(uint32_t elapsed_ms) {
    return elapsed_ms >= 3000 ? 0 : static_cast<uint8_t>(48 * (3000 - elapsed_ms) / 3000);
}
struct Channel {
    uint8_t value = 0;
    int64_t last_change = 0;
    void advance(uint8_t target, int64_t now) {
        if (target > maximum) target = maximum;
        if (value == target || now - last_change < step_us) return;
        value += value < target ? 1 : -1;
        last_change = now;
    }
};
}
