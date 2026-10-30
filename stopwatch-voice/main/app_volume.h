/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>

namespace transceiver::volume {

// The device settings screen defines speaker volume as 0..100 in steps of 5.
// These presets use the endpoints plus the quarter and midpoint of that range.
constexpr std::array<int, 4> presets = {0, 25, 50, 100};

constexpr int nextPreset(int current)
{
    for (const int preset : presets) {
        if (preset > current) {
            return preset;
        }
    }
    return presets.front();
}

}  // namespace transceiver::volume
