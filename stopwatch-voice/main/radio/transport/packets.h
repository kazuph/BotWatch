/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../model/protocol.h"
#include "g711.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <esp_now.h>

namespace transceiver {

enum class PacketType : uint8_t {
    Discover,
    Call,
    Answer,
    Confirm,
    Hangup,
    TalkStart,
    TalkStop,
    ModeRequest,
    ModeSet,
    Audio,
};

constexpr uint32_t packetMagic                 = 0x5254354D;  // "M5TR" in little-endian memory.
constexpr uint8_t packetVersion                = 4;
constexpr std::size_t spectrumBandCount        = 20;
constexpr uint32_t captureSampleRate           = 16000;
constexpr uint32_t playbackSampleRate          = 44100;
constexpr uint32_t duplexSampleRate            = captureSampleRate;
constexpr uint16_t captureDurationMs           = g711::frameDurationMs;
constexpr std::size_t sourceSamplesPerPacket   = captureSampleRate * captureDurationMs / 1000;
constexpr std::size_t playbackSamplesPerPacket = playbackSampleRate * captureDurationMs / 1000;
constexpr std::size_t duplexSamplesPerPacket   = duplexSampleRate * captureDurationMs / 1000;
constexpr std::size_t audioSamplesPerPacket    = g711::samplesPerFrame;
// atomic14/esp32-walkie-talkie buffers 300 ms before playback and allocates
// three times that amount so radio jitter cannot overwrite unplayed audio.
constexpr uint16_t playbackPrebufferMs        = 300;
constexpr std::size_t playbackPrebufferFrames = playbackPrebufferMs / captureDurationMs;
constexpr std::size_t transportBufferFrames   = playbackPrebufferFrames * 3;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic    = packetMagic;
    uint8_t version   = packetVersion;
    PacketType type   = PacketType::Call;
    uint16_t reserved = 0;
    uint32_t session  = 0;
    uint32_t sequence = 0;
};

struct ControlPacket {
    PacketHeader header;
    Role role                         = Role::None;
    ConversationMode conversationMode = ConversationMode::Ptt;
};

struct AudioPacket {
    PacketHeader header;
    Role talker                                        = Role::None;
    uint8_t sampleCount                                = audioSamplesPerPacket;
    std::array<uint8_t, spectrumBandCount> spectrum    = {};
    std::array<uint8_t, audioSamplesPerPacket> samples = {};
};
#pragma pack(pop)

static_assert(sizeof(AudioPacket) < ESP_NOW_MAX_DATA_LEN);
static_assert(sourceSamplesPerPacket * 1000 == captureSampleRate * captureDurationMs);
static_assert(playbackSamplesPerPacket * 1000 == playbackSampleRate * captureDurationMs);
static_assert(duplexSamplesPerPacket * 1000 == duplexSampleRate * captureDurationMs);
static_assert(audioSamplesPerPacket * 1000 == g711::sampleRate * captureDurationMs);
static_assert(playbackPrebufferMs % captureDurationMs == 0);

inline bool validHeader(const PacketHeader& header)
{
    return header.magic == packetMagic && header.version == packetVersion;
}

inline bool validConversationMode(ConversationMode mode)
{
    return mode == ConversationMode::Ptt;
}

}  // namespace transceiver
