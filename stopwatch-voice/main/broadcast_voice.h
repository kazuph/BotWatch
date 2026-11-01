#pragma once
#include <cstdint>
#include <cstring>

namespace broadcast_voice {
constexpr float kWakeScoreThreshold = -0.2f;
constexpr float kWakeMarginThreshold = 0.3f;
constexpr float kCommandScoreThreshold = -0.4f;
constexpr int kWaitSeconds = 5;
enum class Decision { none, wake, terrestrial, satellite };
struct Result { Decision decision; const char *reason; };

class State {
public:
    bool waiting(int64_t onset) {
        if (armed_ && onset > deadline_) armed_ = false;
        return armed_;
    }
    void arm(int64_t listening_start, int sample_rate) {
        deadline_ = listening_start + int64_t(kWaitSeconds) * sample_rate; armed_ = true;
    }
    Result wake(const char *word, float score, float margin, const char *kana) const {
        if (!kana || !*kana) return {Decision::none, "empty_kana"};
        if (!word || std::strcmp(word, "てれび") != 0) return {Decision::none, "not_tv"};
        if (!(score >= kWakeScoreThreshold)) return {Decision::none, "score_below"};
        if (!(margin >= kWakeMarginThreshold)) return {Decision::none, "margin_below"};
        return {Decision::wake, "accepted"};
    }
    Result command(const char *word, float score, const char *kana, int64_t onset) {
        if (!waiting(onset)) return {Decision::none, "expired"};
        armed_ = false;
        if (!kana || !*kana) return {Decision::none, "empty_kana"};
        if (!word) return {Decision::none, "no_match"};
        if (!(score >= kCommandScoreThreshold)) return {Decision::none, "score_below"};
        if (!std::strcmp(word, "ちじょーは") || !std::strcmp(word, "ちでじ")) return {Decision::terrestrial, "accepted"};
        if (!std::strcmp(word, "びーえす") || !std::strcmp(word, "しーえす")) return {Decision::satellite, "accepted"};
        return {Decision::none, "not_command"};
    }
private:
    bool armed_ = false;
    int64_t deadline_ = 0;
};
}
