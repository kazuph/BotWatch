#pragma once
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
inline Result decide(const home_match::Result &match, const char *kana) {
    if (!kana || !*kana) return {false, "empty_kana"};
    if (!match.best) return {false, "no_match"};
    if (match.best->negative) return {false, "negative"};
    const auto kind = static_cast<voice_catalog::Kind>(match.best->kind);
    const bool wake = is_wake(kind);
    const float threshold = wake ? kWakeScore : match.best->long_form ? kLongScore : kShortScore;
    if (!(match.score >= threshold)) return {false, "score_below"};
    if (wake || match.best->long_form) {
        const float required = wake ? kWakeMargin : kLongMargin;
        if (!match.second || !(match.score - match.score2 >= required)) return {false, "margin_below"};
    }
    return {true, "accepted"};
}
class State {
public:
    Target target(int64_t onset) { if (target_ != Target::none && onset > deadline_) target_ = Target::none; return target_; }
    void arm(Target target, int64_t listening_start, int sample_rate) {
        target_ = target; deadline_ = listening_start + int64_t(kWaitSeconds) * sample_rate;
    }
    void clear() { target_ = Target::none; }
private:
    Target target_ = Target::none;
    int64_t deadline_ = 0;
};
}
