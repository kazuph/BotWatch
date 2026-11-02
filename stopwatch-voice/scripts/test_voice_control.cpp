#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "voice_control.h"
#include "ir_codes.h"
#include "ir_home.h"

int main() {
    using namespace voice_catalog;
    using namespace voice_control;
    const char *vocab[] = {
#include "vocabulary.inc"
    };
    const Group groups[] = {group(k_idle), group(k_tv), group(k_aircon), group(k_light), group(k_print_words), group(k_print_confirm)};
    constexpr size_t vocab_count = sizeof(vocab) / sizeof(*vocab);
    bool kinds[static_cast<int>(Kind::cancel) + 1] = {};
    for (const auto &g : groups) {
        const size_t bytes = home_match::Matcher::bytes(g.phrases, g.count, vocab, vocab_count);
        assert(bytes && bytes < 50584);
        std::vector<std::max_align_t> storage((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        home_match::Matcher matcher; assert(matcher.init(storage.data(), g.phrases, g.count, vocab, vocab_count));
        for (size_t i = 0; i < g.count; ++i) {
            const auto &p = g.phrases[i]; assert(home_match::Matcher::token_count(p.text, vocab, vocab_count));
            kinds[p.kind] = true;
            home_match::Candidate best{p.text, 0, 1, p.kind, p.value, p.negative, p.long_form};
            home_match::Candidate second{"other", 0, 1, 0, 0, false, false};
            home_match::Result m{&best, &second, -.1f, -.9f};
            assert(decide(m, "こえ").accepted == !p.negative);
            m.score = -1.2f; assert(!decide(m, "こえ").accepted);
            m.score = -.1f; assert(!decide(m, "").accepted);
        }
    }
    for (bool present : kinds) assert(present);
    for (int n = 1; n <= 10; ++n) for (int sign : {-1, 1}) {
        bool found = false; for (const auto &p : k_tv) if (p.kind == int(Kind::tv_volume) && p.value == n * sign) found = true;
        assert(found);
    }
    for (int n = 1; n <= 9; ++n) { bool found = false; for (const auto &p : k_tv) if (p.kind == int(Kind::tv_channel) && p.value == n) found = true; assert(found); }
    for (int n = 16; n <= 31; ++n) { bool found = false; for (const auto &p : k_aircon) if (p.kind == int(Kind::temperature) && p.value == n) found = true; assert(found); }
    for (const auto &p : k_aircon) if (p.kind == int(Kind::temperature)) assert(p.value >= 16 && p.value <= 31);
    for (const auto &p : k_tv) if (p.kind == int(Kind::tv_volume)) assert(std::abs(p.value) <= 10);
    home_match::Candidate wake{"てれび", 0, 3, int8_t(Kind::wake_tv), 0, false, false};
    home_match::Candidate second{"えあこん", 0, 3, int8_t(Kind::wake_aircon), 0, false, false};
    home_match::Result m{&wake, &second, -.021f, -.957f}; assert(decide(m, "てれび").accepted);
    m.score = -.038f; m.score2 = -.793f; assert(decide(m, "えれび").accepted);
    m.score = -.588f; m.score2 = -.628f; assert(!decide(m, "かせんご").accepted);
    m.score = -.1f; m.score2 = -.2f; assert(!decide(m, "てれび").accepted);
    home_match::Candidate volume{"おんりょーごあげて", 0, 3, int8_t(Kind::tv_volume), 5, false, false};
    m = {&volume, &second, -.745f, -1.2f}; assert(!decide(m, "りょーをごあげて").accepted);
    m.score = -1.138f; assert(!decide(m, "おんりょーごあげて").accepted);
    m.score = -.1f; assert(!decide(m, "").accepted);
    State state; state.arm(Target::tv, 1000, 16000); assert(state.target(81000) == Target::tv); assert(state.target(81001) == Target::none);
    state.arm(Target::aircon, 0, 16000); state.clear(); assert(state.target(1) == Target::none);
    home_match::Candidate negative{"なくて", 0, 1, int8_t(Kind::negative), 0, true, false};
    m = {&negative, &second, -.718f, -1.5f};
    assert(route(m, "わらって", Target::none) == Route::fallback);
    m.score = -.1f;
    assert(route(m, "なくて", Target::none) == Route::suppress);
    assert(!std::strcmp(caption(Display::idle), "話しかけてね"));
    m.score = -.718f;
    assert(route(m, "わらって", Target::tv) == Route::suppress);
    assert(route(m, "", Target::tv) == Route::suppress);
    assert(route(m, "", Target::none) == Route::fallback);
    assert(!std::strcmp(caption(Display::prompt), "どうぞ"));
    assert(caption(Display::shown) == nullptr && caption(Display::fallback) == nullptr);
    state.arm(Target::tv, 1000, 16000);
    assert(!state.expire(81000)); assert(state.expire(81001)); assert(!state.expire(81002));
    home_match::Candidate long_volume{"おんりょーをごあげて", 0, 1, int8_t(Kind::tv_volume), 5, false, true};
    m = {&long_volume, &second, -.027f, -.030f};
    assert(!decide(m, "おんりょーをごあげて").accepted);
    assert(decide(m, "おんりょーをごあげて", Target::tv).accepted);
    m.score = -1.1f; assert(!decide(m, "おんりょーをごあげて", Target::tv).accepted);
    state.arm(Target::tv, 1000, 16000);
    assert(route(m, "おんりょーをごあげて", Target::tv) == Route::suppress);
    assert(state.active(Target::tv, 80000));
    state.arm(Target::tv, 80000, 16000);
    assert(state.active(Target::tv, 160000)); assert(!state.active(Target::tv, 160001));
    assert(minimum_voiced_hops(false, 15, 3) == 15);
    assert(minimum_voiced_hops(true, 15, 3) == 3);
    state.arm(Target::tv, 0, 16000); state.command_begin(Target::tv); state.command_sent(Target::tv, false, 100, 16000);
    assert(state.active(Target::tv, 80000)); state.command_sent(Target::tv, true, 1000, 16000);
    assert(state.active(Target::tv, 81000)); state.clear(); assert(!state.active(Target::tv, 1));
    state.arm(Target::aircon, 0, 16000); state.command_begin(Target::aircon); assert(!state.active(Target::aircon, 1));
    assert(!show_koeact(true, false, false)); assert(show_koeact(true, false, true)); assert(show_koeact(true, true, false));
    using K = home::Command::Kind; ir_home::Learned slot;
    assert(ir_home::learned_slot({K::tv_band, 0, 0}, slot) && slot == ir_home::Learned::terrestrial);
    assert(ir_home::learned_slot({K::tv_band, 0, 1}, slot) && slot == ir_home::Learned::satellite);
    assert(ir_home::learned_slot({K::power, 2, 1}, slot) && slot == ir_home::Learned::light_on);
    assert(ir_home::learned_slot({K::power, 2, 0}, slot) && slot == ir_home::Learned::light_off);
    assert(ir_home::learned_slot({K::light_mode, 2, 0}, slot) && slot == ir_home::Learned::light_orange);
    assert(ir_home::learned_slot({K::light_mode, 2, 1}, slot) && slot == ir_home::Learned::light_night);
    assert(ir_home::learned_slot({K::light_mode, 2, 2}, slot) && slot == ir_home::Learned::light_white);
    assert(!ir_home::learned_slot({K::power, 1, 1}, slot));
    static_assert(ir_codes::tv_code(ir_codes::tv_band_key(0)) == 0x20DFDB0C);
    static_assert(ir_codes::tv_code(ir_codes::tv_band_key(1)) == 0x20DFF00F);
    std::puts("PASS vocabulary, all operation types, allowed numbers, thresholds, negatives, wake timeout, IR codes");
}
