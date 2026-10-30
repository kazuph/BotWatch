/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace transceiver::g711 {

constexpr uint32_t sampleRate         = 8000;
constexpr uint16_t frameDurationMs    = 20;
constexpr std::size_t samplesPerFrame = sampleRate * frameDurationMs / 1000;

inline uint8_t encodeMuLaw(int16_t sample)
{
    constexpr int32_t bias = 0x84;
    constexpr int32_t clip = 32635;

    int32_t magnitude = sample;
    uint8_t sign      = 0;
    if (magnitude < 0) {
        sign      = 0x80;
        magnitude = -magnitude;
    }
    magnitude = std::min(magnitude, clip) + bias;

    uint8_t exponent = 7;
    for (int32_t mask = 0x4000; exponent > 0 && (magnitude & mask) == 0; mask >>= 1) {
        --exponent;
    }
    const uint8_t mantissa = static_cast<uint8_t>((magnitude >> (exponent + 3)) & 0x0F);
    return static_cast<uint8_t>(~(sign | static_cast<uint8_t>(exponent << 4) | mantissa));
}

inline int16_t decodeMuLaw(uint8_t encoded)
{
    constexpr int32_t bias = 0x84;

    const uint8_t value = static_cast<uint8_t>(~encoded);
    int32_t magnitude   = (static_cast<int32_t>(value & 0x0F) << 3) + bias;
    magnitude <<= (value & 0x70) >> 4;
    return static_cast<int16_t>((value & 0x80) != 0 ? bias - magnitude : magnitude - bias);
}

template <std::size_t SourceRate, std::size_t SourceSamples>
std::array<uint8_t, samplesPerFrame> encodeFrame(const std::array<int16_t, SourceSamples>& source)
{
    static_assert(SourceSamples * 1000 == SourceRate * frameDurationMs);

    std::array<uint8_t, samplesPerFrame> encoded = {};
    for (std::size_t output = 0; output < encoded.size(); ++output) {
        const std::size_t begin = output * SourceRate / sampleRate;
        const std::size_t end   = (output + 1) * SourceRate / sampleRate;
        int32_t sum             = 0;
        for (std::size_t input = begin; input < end; ++input) {
            sum += source[input];
        }
        encoded[output] = encodeMuLaw(static_cast<int16_t>(sum / static_cast<int32_t>(end - begin)));
    }
    return encoded;
}

template <std::size_t DestinationRate, std::size_t DestinationSamples>
void decodeFrameInto(const std::array<uint8_t, samplesPerFrame>& encoded, int16_t* decoded)
{
    static_assert(DestinationSamples * 1000 == DestinationRate * frameDurationMs);

    for (std::size_t output = 0; output < DestinationSamples; ++output) {
        const std::size_t scaled    = output * sampleRate;
        const std::size_t index     = scaled / DestinationRate;
        const std::size_t remainder = scaled % DestinationRate;
        const std::size_t next      = std::min(index + 1, encoded.size() - 1);
        const int32_t current       = decodeMuLaw(encoded[index]);
        const int32_t following     = decodeMuLaw(encoded[next]);
        decoded[output] = static_cast<int16_t>((current * static_cast<int32_t>(DestinationRate - remainder) +
                                                following * static_cast<int32_t>(remainder)) /
                                               static_cast<int32_t>(DestinationRate));
    }
}

}  // namespace transceiver::g711
