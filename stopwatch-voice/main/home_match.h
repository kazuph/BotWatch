#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace home_match {
struct Phrase { const char *text; bool negative; };
struct Candidate { const char *text; uint16_t offset, length; bool negative; float best; uint16_t start, end; };
struct Result { const Candidate *best; const Candidate *second; float score, score2; uint16_t start, end; };

// The caller supplies PSRAM for candidates, token IDs and Viterbi states.
class Matcher {
public:
    static size_t token_count(const char *text, const char *const *vocab, size_t vocab_size) {
        size_t n = 0;
        while (*text) {
            size_t id = token(text, vocab, vocab_size);
            if (!id) return 0;
            text += std::strlen(vocab[id]); ++n;
        }
        return n;
    }
    static size_t bytes(const Phrase *phrases, size_t count, const char *const *vocab, size_t vocab_size) {
        size_t tokens = 0;
        for (size_t i = 0; i < count; ++i) { size_t n = token_count(phrases[i].text, vocab, vocab_size); if (!n) return 0; tokens += n; }
        return count * sizeof(Candidate) + tokens * sizeof(uint16_t) + (tokens * 2 + count) * (sizeof(float) + sizeof(uint16_t)) + alignof(float);
    }
    static size_t state_bytes(const Phrase *phrases, size_t count, const char *const *vocab, size_t vocab_size) {
        size_t tokens = 0;
        for (size_t i = 0; i < count; ++i) tokens += token_count(phrases[i].text, vocab, vocab_size);
        return (tokens * 2 + count) * (sizeof(float) + sizeof(uint16_t)) + count * (sizeof(float) + 2 * sizeof(uint16_t));
    }
    bool init(void *memory, const Phrase *phrases, size_t count, const char *const *vocab, size_t vocab_size) {
        if (!memory || !bytes(phrases, count, vocab, vocab_size)) return false;
        count_ = count; candidates_ = static_cast<Candidate *>(memory);
        size_t total = 0;
        for (size_t i = 0; i < count; ++i) total += token_count(phrases[i].text, vocab, vocab_size);
        ids_ = reinterpret_cast<uint16_t *>(candidates_ + count);
        auto end = reinterpret_cast<uintptr_t>(ids_ + total);
        states_ = reinterpret_cast<float *>((end + alignof(float) - 1) & ~(uintptr_t(alignof(float)) - 1));
        starts_ = reinterpret_cast<uint16_t *>(states_ + total * 2 + count);
        size_t offset = 0;
        for (size_t i = 0; i < count; ++i) {
            const char *p = phrases[i].text; size_t length = 0;
            while (*p) { const size_t id = token(p, vocab, vocab_size); ids_[offset + length++] = id; p += std::strlen(vocab[id]); }
            candidates_[i] = {phrases[i].text, static_cast<uint16_t>(offset), static_cast<uint16_t>(length), phrases[i].negative, 0, 0, 0}; offset += length;
        }
        reset(); return true;
    }
    void reset() {
        size_t state = 0; frame_ = 0;
        for (size_t i = 0; i < count_; ++i) {
            const size_t n = 2 * candidates_[i].length + 1;
            std::fill(states_ + state, states_ + state + n, -std::numeric_limits<float>::infinity());
            std::fill(starts_ + state, starts_ + state + n, 0); states_[state] = 0; state += n;
            candidates_[i].best = -std::numeric_limits<float>::infinity(); candidates_[i].start = candidates_[i].end = 0;
        }
    }
    template <typename T> void feed(const T *row, size_t vocab_size, float scale) {
        const T maximum = *std::max_element(row, row + vocab_size);
        float sum = 0; for (size_t i = 0; i < vocab_size; ++i) sum += std::exp((float(row[i]) - float(maximum)) * scale);
        const float normalizer = float(maximum) * scale + std::log(sum);
        size_t offset = 0;
        for (size_t c = 0; c < count_; ++c) {
            Candidate &candidate = candidates_[c]; const size_t n = 2 * candidate.length + 1;
            float *s = states_ + offset;
            uint16_t *starts = starts_ + offset;
            for (size_t j = n; j-- > 1;) {
                const size_t id = j % 2 ? ids_[candidate.offset + j / 2] : 0;
                float best = s[j]; uint16_t start = starts[j];
                if (s[j - 1] > best) { best = s[j - 1]; start = j == 1 ? frame_ : starts[j - 1]; }
                if (j > 1 && j % 2 && ids_[candidate.offset + j / 2] != ids_[candidate.offset + j / 2 - 1] && s[j - 2] > best) { best = s[j - 2]; start = starts[j - 2]; }
                s[j] = best + float(row[id]) * scale - normalizer;
                starts[j] = start;
            }
            const size_t last = s[n - 1] > s[n - 2] ? n - 1 : n - 2;
            if (s[last] > candidate.best) { candidate.best = s[last]; candidate.start = starts[last]; candidate.end = frame_ + 1; }
            offset += n;
        }
        ++frame_;
    }
    Result result() const {
        Result r{nullptr, nullptr, -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 0, 0};
        for (size_t i = 0; i < count_; ++i) {
            const Candidate &c = candidates_[i];
            const float score = c.end > c.start ? c.best / (c.end - c.start) : -std::numeric_limits<float>::infinity();
            if (score > r.score) { r.second = r.best; r.score2 = r.score; r.best = &c; r.score = score; r.start = c.start; r.end = c.end; }
            else if (score > r.score2) { r.second = &c; r.score2 = score; }
        }
        return r;
    }
private:
    static size_t token(const char *p, const char *const *vocab, size_t n) {
        size_t best = 0, len = 0;
        for (size_t i = 2; i < n; ++i) { const size_t size = std::strlen(vocab[i]); if (size > len && std::strncmp(p, vocab[i], size) == 0) { best = i; len = size; } }
        return best;
    }
    Candidate *candidates_ = nullptr; uint16_t *ids_ = nullptr; float *states_ = nullptr; uint16_t *starts_ = nullptr; size_t count_ = 0; uint16_t frame_ = 0;
};
}
