#include "../src/led_ramp.h"
#include <cassert>
#include <cstdint>

int main() {
    assert(led_ramp::countdown_target(0) == 48);
    assert(led_ramp::countdown_target(1500) == 24);
    assert(led_ramp::countdown_target(3000) == 0);
    assert(led_ramp::countdown_target(UINT32_MAX) == 0);
    for (uint32_t t = 1; t <= 3000; ++t)
        assert(led_ramp::countdown_target(t) <= led_ramp::countdown_target(t - 1));
    led_ramp::Channel c;
    int64_t previous_change = 0;
    for (int64_t t = 0; t <= 5000000; t += 1000) {
        const uint8_t target = (t / 47000) % 2 ? 255 : 0;
        const uint8_t before = c.value;
        c.advance(target, t);
        assert(c.value <= 64);
        if (c.value != before) {
            assert((c.value == before + 1) || (c.value + 1 == before));
            assert(t - previous_change >= led_ramp::step_us);
            previous_change = t;
        }
    }
    c = {};
    c.advance(255, 10000000);
    assert(c.value == 1); // A long scheduling delay cannot cause a jump.
    for (int i = 1; i < 64; ++i) c.advance(255, 10000000 + i * led_ramp::step_us);
    assert(c.value == 64);
    c.advance(0, 10000000 + 63 * led_ramp::step_us);
    assert(c.value == 64);
    for (int i = 64; i < 128; ++i) c.advance(0, 10000000 + i * led_ramp::step_us);
    assert(c.value == 0);
}
