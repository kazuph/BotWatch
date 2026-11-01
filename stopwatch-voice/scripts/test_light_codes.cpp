#include <cassert>
#include <cstdio>
#include "ir_codes.h"
#include "voice_catalog.h"

int main() {
    constexpr uint32_t expected[] = {0x0176F00F, 0x017642BD, 0x0176D02F, 0x0176827D};
    for (unsigned n = 0; n < 4; ++n) {
        const uint32_t value = ir_codes::light_code(static_cast<ir_codes::LightKey>(n));
        assert(value == expected[n]);
        const auto pulses = ir_codes::nec(value);
        uint32_t decoded = 0;
        for (int i = 0; i < 32; ++i) decoded = (decoded << 1) | (pulses[3 + i * 2] == 1690);
        assert(decoded == expected[n]);
    }
    bool power_on = false, power_off = false, modes[3] = {};
    for (const auto &phrase : voice_catalog::k_light) {
        if (phrase.kind == int(voice_catalog::Kind::power)) { power_on |= phrase.value == 1; power_off |= phrase.value == 0; }
        if (phrase.kind == int(voice_catalog::Kind::light_mode) && phrase.value >= 0 && phrase.value < 3) modes[phrase.value] = true;
    }
    assert(power_on && power_off && modes[0] && modes[1] && modes[2]);
    std::puts("PASS 4 captured NEC codes and light voice actions");
}
