#include <cstdio>
#include "ir_codes.h"
#include "IRrecv.h"
#include "IRsend_test.h"
#include "IRutils.h"

int main()
{
    constexpr uint32_t expected[] = {0x20DF10EF, 0x20DF40BF, 0x20DFC03F, 0x20DF8877,
        0x20DF48B7, 0x20DFC837, 0x20DF28D7, 0x20DFA857, 0x20DF6897, 0x20DFE817, 0x20DF18E7};
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
