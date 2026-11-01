#include <cstdio>
#include "ir_codes.h"
#include "IRrecv.h"
#include "IRsend_test.h"
#include "IRutils.h"

int main()
{
    constexpr uint32_t expected[] = {0x20DF10EF, 0x20DF40BF, 0x20DFC03F, 0x20DF8877,
        0x20DF48B7, 0x20DFC837, 0x20DF28D7, 0x20DFA857, 0x20DF6897, 0x20DFE817, 0x20DF18E7,
        0x20DF9867, 0x20DF00FF, 0x20DF807F, 0x20DFF00F, 0x20DF0FF0, 0x20DF58A7, 0x20DF7986,
        0x20DF35CA, 0x20DFCA35, 0x20DFC43B,
        0x20DF02FD, 0x20DF827D, 0x20DFE01F, 0x20DF609F, 0x20DF22DD, 0x20DF14EB};
    int failed = 0;
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const auto key = static_cast<ir_codes::TvKey>(i);
        const auto timings = ir_codes::nec(ir_codes::tv_code(key));
        IRsendTest sender(4); IRrecv receiver(4); sender.begin(); sender.reset();
        for (size_t j = 0; j < timings.size(); ++j) {
            if (j % 2 == 0) sender.mark(timings[j]); else sender.space(timings[j]);
        }
        sender.makeDecodeResult();
        const bool ok = ir_codes::tv_code(key) == expected[i] && receiver.decode(&sender.capture) &&
                        sender.capture.decode_type == NEC && sender.capture.bits == 32 && sender.capture.value == expected[i];
        if (!ok) { ++failed; std::printf("FAIL key=%u\n", i); }
    }
    const auto captured = ir_codes::nec(ir_codes::tv_code(ir_codes::TvKey::candidate_terrestrial_captured));
    uint32_t bits = 0;
    bool raw_ok = captured.size() == 67 && captured[0] == 9000 && captured[1] == 4500 && captured[66] == 560;
    for (unsigned i = 0; i < 32; ++i) {
        const auto mark = captured[2 + i * 2], space = captured[3 + i * 2];
        raw_ok &= mark == 560 && (space == 560 || space == 1690);
        bits = (bits << 1) | (space == 1690);
    }
    raw_ok &= bits == 0x20DFDB0C;
    raw_ok &= ir_codes::tv_code(ir_codes::tv_band_key(0)) == 0x20DFDB0C;
    raw_ok &= ir_codes::tv_code(ir_codes::tv_band_key(1)) == 0x20DFF00F;
    raw_ok &= ir_codes::tv_code(ir_codes::tv_band_key(2)) == 0x20DFF00F;
    if (!raw_ok) { ++failed; std::printf("FAIL captured terrestrial pulses=%zu bits=%08X\n", captured.size(), bits); }
    else std::printf("PASS captured terrestrial pulses=%zu bits=%08X\n", captured.size(), bits);
    for (bool on : {false, true}) {
        auto frame = ir_codes::hitachi_power(on);
        auto timings = ir_codes::hitachi_timings(frame);
        IRsendTest sender(4); sender.begin(); sender.reset();
        sender.sendHitachiAc424(frame.data(), frame.size(), 0);
        bool ok = frame[11] == 0x13 && frame[12] == 0xEC && frame[27] == (on ? 0xF1 : 0xE1) &&
                  frame[28] == (on ? 0x0E : 0x1E);
        for (size_t i = 0; i < timings.size(); ++i) ok &= sender.output[i] == timings[i];
        if (!ok) { ++failed; std::printf("FAIL Hitachi power=%d\n", on); }
    }
    std::printf("%s LG NEC codes=%u\n", failed ? "FAIL" : "PASS", unsigned(sizeof(expected) / sizeof(expected[0])));
    return failed ? 1 : 0;
}
