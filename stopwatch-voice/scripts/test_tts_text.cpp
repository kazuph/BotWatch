#include "tts_text.h"
#include "tts_phrases.h"
extern "C" {
#include "g2p.h"
}
#include <cassert>
#include <cstdio>
#include <vector>
#include <string>
#include <climits>
int main() {
    std::vector<char> kana(tts::kana_capacity());
    std::vector<int32_t> ids(saan_g2p_capacity(kana.size()));
    int tested = 0;
    auto check = [&](const std::string &s) {
        assert(tts::intermediate(s.c_str(), kana.data(), kana.size()));
        int32_t count;
        assert(saan_g2p(kana.data(), std::char_traits<char>::length(kana.data()), ids.data(), ids.size(), &count, nullptr) == SAAN_G2P_OK);
        assert(count > 3); ++tested;
    };
    for (const auto &phrase : kTtsPhrases) { check(phrase.caption); }
    for (int n = 0; n <= 100; ++n) {
        for (const char *s : {"おとのおおきさ ", "あかるさ ", "おとを"}) {
            check(std::string(s) + std::to_string(n) + (std::string(s)=="おとを" ? "あげたよ" : ""));
        }
    }
    for (int n : {INT_MIN, -1, 0, 16, 28, 31, 100, 300, 600, 800, 3000, 8000, INT_MAX}) {
        check(std::to_string(n) + "℃にしたよ");
        check("みぎを" + std::to_string(n) + "ど見たよ");
    }
    check("テレビをつけたよ"); check("にっこり"); check("ラベルをいんさつするよ");
    assert(!tts::intermediate("未対応の文", kana.data(), kana.size()));
    assert(!tts::intermediate("にっこり", kana.data(), 2));
    printf("TTS_TEXT PASS cases=%d unsupported_and_overflow=2 kana_capacity=%zu\n", tested, kana.size());
}
