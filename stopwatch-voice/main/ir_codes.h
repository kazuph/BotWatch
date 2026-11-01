#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ir_codes {
enum class LightKey { power, orange, night, white };
constexpr uint32_t light_code(LightKey key)
{
    constexpr uint32_t codes[] = {0x0176F00F, 0x017642BD, 0x0176D02F, 0x0176827D};
    return codes[static_cast<unsigned>(key)];
}
enum class TvKey { power, volume_up, volume_down, channel_1, channel_2, channel_3,
                   channel_4, channel_5, channel_6, channel_7, channel_8, channel_9,
                   channel_up, channel_down, candidate_tv, candidate_tv_radio, candidate_flashback, candidate_livetv,
                   candidate_japan_display, candidate_prlist, candidate_3digit_input,
                   candidate_up, candidate_down, candidate_left, candidate_right, candidate_ok, candidate_back,
                   candidate_terrestrial_captured };
constexpr uint32_t tv_code(TvKey key)
{
    constexpr uint32_t codes[] = {0x20DF10EF, 0x20DF40BF, 0x20DFC03F, 0x20DF8877,
        0x20DF48B7, 0x20DFC837, 0x20DF28D7, 0x20DFA857, 0x20DF6897, 0x20DFE817, 0x20DF18E7,
        0x20DF9867, 0x20DF00FF, 0x20DF807F, 0x20DFF00F, 0x20DF0FF0, 0x20DF58A7, 0x20DF7986,
        0x20DF35CA, 0x20DFCA35, 0x20DFC43B,
        0x20DF02FD, 0x20DF827D, 0x20DFE01F, 0x20DF609F, 0x20DF22DD, 0x20DF14EB, 0x20DFDB0C};
    return codes[static_cast<unsigned>(key)];
}

constexpr TvKey tv_band_key(int band)
{
    return band == 0 ? TvKey::candidate_terrestrial_captured : TvKey::candidate_tv;
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
inline std::array<uint8_t, 61> hitachi_frame(bool on, int mode, int temperature)
{
    std::array<uint8_t, 61> frame = {
        0x01,0x10,0x00,0x40,0xBF,0xFF,0x00,0xCC,0x33,0x92,0x6D,0x44,0xBB,0x6C,0x93,
        0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x53,0xAC,0xF1,0x0E,0x00,
        0xFF,0x00,0xFF,0x80,0x7F,0x03,0xFC,0x01,0xFE,0xC8,0x37,0x00,0xFF,0x00,0xFF,
        0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF
    };
    frame[11] = 0x13; frame[12] = uint8_t(~frame[11]);
    frame[13] = uint8_t((frame[13] & 0x03) | (temperature << 2)); frame[14] = uint8_t(~frame[13]);
    constexpr uint8_t modes[] = {3, 6, 5};
    frame[25] = uint8_t((frame[25] & 0xF0) | modes[mode]); frame[26] = uint8_t(~frame[25]);
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
