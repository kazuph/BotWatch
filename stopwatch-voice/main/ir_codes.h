#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ir_codes {
enum class TvKey { power, volume_up, volume_down, channel_1, channel_2, channel_3,
                   channel_4, channel_5, channel_6, channel_7, channel_8 };
constexpr uint32_t tv_code(TvKey key)
{
    constexpr uint32_t codes[] = {0x20DF10EF, 0x20DF40BF, 0x20DFC03F, 0x20DF8877,
        0x20DF48B7, 0x20DFC837, 0x20DF28D7, 0x20DFA857, 0x20DF6897, 0x20DFE817, 0x20DF18E7};
    return codes[static_cast<unsigned>(key)];
}

// Alternating carrier mark / space, following StackChan's LG NEC timings.
inline std::array<uint32_t, 67> nec(uint32_t code)
{
    std::array<uint32_t, 67> out{};
    out[0] = 9000; out[1] = 4500;
    for (int bit = 0; bit < 32; ++bit) {
        out[2 + bit * 2] = 560;
        out[3 + bit * 2] = (code & (1UL << (31 - bit))) ? 1690 : 560;
    }
    out[66] = 560;
    return out;
}

// 61-byte frame observed from the real remote at 27 C; keep all unknown fields intact.
inline std::array<uint8_t, 61> hitachi_power(bool on)
{
    std::array<uint8_t, 61> frame = {
        0x01,0x10,0x00,0x40,0xBF,0xFF,0x00,0xCC,0x33,0x92,0x6D,0x44,0xBB,0x6C,0x93,
        0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x53,0xAC,0xF1,0x0E,0x00,
        0xFF,0x00,0xFF,0x80,0x7F,0x03,0xFC,0x01,0xFE,0xC8,0x37,0x00,0xFF,0x00,0xFF,
        0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF
    };
    frame[11] = 0x13; frame[12] = uint8_t(~frame[11]);
    frame[27] = on ? uint8_t(frame[27] | 0x10) : uint8_t(frame[27] & ~0x10);
    frame[28] = uint8_t(~frame[27]);
    return frame;
}

inline std::array<uint32_t, 981> hitachi_timings(const std::array<uint8_t, 61> &frame)
{
    std::array<uint32_t, 981> out{};
    out[0] = 29784; out[1] = 49290; out[2] = 3416; out[3] = 1604;
    for (std::size_t byte = 0; byte < frame.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::size_t at = 4 + (byte * 8 + bit) * 2;
            out[at] = 463; out[at + 1] = (frame[byte] & (1U << bit)) ? 1208 : 372;
        }
    }
    out[980] = 463;
    return out;
}
}
