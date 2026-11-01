#pragma once
#include <cstdint>
#include <cstring>

namespace broadcast_voice {
constexpr float kScoreThreshold = -2.0f;
constexpr int kWaitSeconds = 5;
enum class Decision { none, wake, terrestrial, satellite };

class State {
public:
    bool waiting(int64_t onset) {
        if (armed_ && onset > deadline_) armed_ = false;
        return armed_;
    }
    void arm(int64_t listening_start, int sample_rate) {
        deadline_ = listening_start + int64_t(kWaitSeconds) * sample_rate; armed_ = true;
    }
    Decision wake(const char *word, float score) const {
        return word && std::strcmp(word, "てれび") == 0 && score >= kScoreThreshold ? Decision::wake : Decision::none;
    }
    Decision command(const char *word, float score, int64_t onset) {
        if (!waiting(onset)) return Decision::none;
        armed_ = false;
        if (!word || !(score >= kScoreThreshold)) return Decision::none;
        if (!std::strcmp(word, "ちじょーは") || !std::strcmp(word, "ちでじ")) return Decision::terrestrial;
        if (!std::strcmp(word, "びーえす") || !std::strcmp(word, "しーえす")) return Decision::satellite;
        return Decision::none;
    }
private:
    bool armed_ = false;
    int64_t deadline_ = 0;
};
}
