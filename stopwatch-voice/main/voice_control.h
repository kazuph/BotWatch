#pragma once
#include <cstring>
#include "voice_catalog.h"

namespace voice_control {
constexpr float kWakeScore = -0.2f;
constexpr float kWakeMargin = 0.3f;
constexpr float kShortScore = -0.4f;
constexpr float kLongScore = -1.0f;
constexpr float kLongMargin = 0.3f;
constexpr int kWaitSeconds = 5;
enum class Target { none, tv, aircon, light };
struct Result { bool accepted; const char *reason; };

inline bool is_wake(voice_catalog::Kind kind) {
    return kind == voice_catalog::Kind::wake_tv || kind == voice_catalog::Kind::wake_aircon || kind == voice_catalog::Kind::wake_light;
}
inline Result decide(const home_match::Result &match, const char *kana, Target target = Target::none) {
    if (!kana || !*kana) return {false, "empty_kana"};
    if (!match.best) return {false, "no_match"};
    const auto kind = static_cast<voice_catalog::Kind>(match.best->kind);
    const bool wake = is_wake(kind);
    const float threshold = wake ? kWakeScore : match.best->long_form ? kLongScore : kShortScore;
    if (!(match.score >= threshold)) return {false, "score_below"};
    if (match.best->negative) return {false, "negative"};
    if (wake || (target == Target::none && match.best->long_form)) {
        const float required = wake ? kWakeMargin : kLongMargin;
        if (!match.second || !(match.score - match.score2 >= required)) return {false, "margin_below"};
    }
    return {true, "accepted"};
}
enum class Display { fallback, shown, idle, prompt };
inline const char *caption(Display display) {
    return display == Display::idle ? "話しかけてね" : display == Display::prompt ? "どうぞ" : nullptr;
}
enum class Route { fallback, suppress, matched };
inline bool show_koeact(bool decoded, bool accepted, bool by_button) { return by_button || !decoded || accepted; }
inline Route route(const home_match::Result &match, const char *kana, Target target) {
    const Result result = decide(match, kana, target);
    if (result.accepted) return Route::matched;
    if (std::strcmp(result.reason, "negative") == 0 || target != Target::none) return Route::suppress;
    return Route::fallback;
}
class State {
public:
    bool active(Target target, int64_t now) const { return target_ == target && now <= deadline_; }
    bool expire(int64_t now) { if (target_ == Target::none || now <= deadline_) return false; target_ = Target::none; return true; }
    Target target(int64_t onset) { expire(onset); return target_; }
    void arm(Target target, int64_t listening_start, int sample_rate) {
        target_ = target; deadline_ = listening_start + int64_t(kWaitSeconds) * sample_rate;
    }
    void clear() { target_ = Target::none; }
    void command_begin(Target target) { if (target != Target::none && target != Target::tv) clear(); }
    void command_sent(Target target, bool sent, int64_t now, int rate) { if (target == Target::tv && sent) arm(target, now, rate); }
private:
    Target target_ = Target::none;
    int64_t deadline_ = 0;
};
inline int minimum_voiced_hops(bool tv_waiting, int ordinary, int onset) { return tv_waiting ? onset : ordinary; }
}
