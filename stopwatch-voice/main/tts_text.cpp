#include "tts_text.h"
#include "tts_phrases.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>
#include <initializer_list>

namespace tts {
namespace {
struct Builder {
    char *out; size_t capacity, used = 0;
    float gain = std::numeric_limits<float>::max();
    bool append(const char *key) {
        for (const auto &p : kTtsPhrases) {
            if (std::strcmp(p.caption, key)) { continue; }
            const size_t n = std::strlen(p.kana);
            if (used + n >= capacity) { return false; }
            std::memcpy(out + used, p.kana, n + 1); used += n;
            gain=std::min(gain,p.gain);
            return true;
        }
        return false;
    }
    bool number(uint64_t value) {
        if (value < 10) { char key[] = {char('0' + value), 0}; return append(key); }
        struct Place { uint64_t value; const char *word; };
        constexpr Place places[] = {{100000000, "億"}, {10000, "万"}, {1000, "千"}, {100, "百"}, {10, "十"}};
        for (const auto &p : places) {
            if (value < p.value) { continue; }
            const auto digit = value / p.value;
            const char *irregular = p.value == 100 ? (digit == 3 ? "三百" : digit == 6 ? "六百" : digit == 8 ? "八百" : nullptr)
                                  : p.value == 1000 ? (digit == 3 ? "三千" : digit == 8 ? "八千" : nullptr) : nullptr;
            if (irregular) { if (!append(irregular)) { return false; } }
            else {
                if ((digit != 1 || p.value >= 10000) && !number(digit)) { return false; }
                if (!append(p.word)) { return false; }
            }
            return value % p.value == 0 || number(value % p.value);
        }
        return false;
    }
};
}
size_t kana_capacity() {
    size_t longest = 0, digit = 0, place = 0;
    for (const auto &p : kTtsPhrases) {
        const size_t n = std::strlen(p.kana);
        longest = std::max(longest, n);
        if (std::strlen(p.caption) == 1 && p.caption[0] >= '0' && p.caption[0] <= '9') { digit = std::max(digit, n); }
        for (const char *key : {"十", "百", "千", "万", "億"}) {
            if (!std::strcmp(key, p.caption)) { place = std::max(place, n); }
        }
    }
    // Existing captions have at most one int32 field, a prefix and a suffix.
    // Each decimal digit contributes at most a digit and a place-name piece.
    constexpr size_t digits = std::numeric_limits<uint32_t>::digits10 + 1;
    return 3 * longest + digits * (digit + place) + 1; // includes the optional minus piece
}

bool intermediate(const char *caption, char *output, size_t capacity, float *gain) {
    if (!caption || std::strlen(caption) >= kCaptionBytes || !output || !capacity) { return false; }
    output[0] = 0;
    Builder b{output, capacity};
    while (*caption) {
        if ((*caption >= '0' && *caption <= '9') || *caption == '-') {
            if (*caption == '-') { if (!b.append("マイナス")) { return false; } ++caption; }
            if (*caption < '0' || *caption > '9') { return false; }
            uint64_t n = 0;
            while (*caption >= '0' && *caption <= '9') {
                n = n * 10 + (*caption++ - '0');
                if (n > uint64_t(INT32_MAX) + 1) { return false; }
            }
            if (!b.number(n)) { return false; }
            continue;
        }
        const TtsPhrase *best = nullptr;
        size_t length = 0;
        for (const auto &p : kTtsPhrases) {
            const size_t n = std::strlen(p.caption);
            if (n > length && !std::strncmp(caption, p.caption, n)) { best = &p; length = n; }
        }
        if (!best || !b.append(best->caption)) { return false; }
        caption += length;
    }
    if(gain) *gain=b.gain;
    return b.used != 0;
}
}
