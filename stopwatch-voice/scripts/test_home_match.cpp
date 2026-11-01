#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "home_match.h"
#include "voice_control.h"
#include "ir_codes.h"

int main() {
    static_assert(ir_codes::tv_code(ir_codes::tv_band_key(0)) == 0x20DFDB0C);
    static_assert(ir_codes::tv_code(ir_codes::tv_band_key(1)) == 0x20DFF00F);
    const char *full_vocab[] = {
#include "vocabulary.inc"
    };
    const home_match::Phrase full_phrases[] = {
        {"てれび", false}, {"ちじょーは", false}, {"ちでじ", false}, {"びーえす", false}, {"しーえす", false},
    };
    const size_t full_count = sizeof(full_phrases) / sizeof(full_phrases[0]);
    const size_t vocab_count = sizeof(full_vocab) / sizeof(full_vocab[0]);
    const size_t full_bytes = home_match::Matcher::bytes(full_phrases, full_count, full_vocab, vocab_count);
    assert(full_bytes > 0 && full_bytes < 61340);
    for (const char *required : {"てれび", "ちでじ", "ちじょーは", "びーえす", "しーえす"}) {
        bool found = false;
        for (const auto &phrase : full_phrases) if (!std::strcmp(phrase.text, required)) found = true;
        assert(found);
    }
    std::vector<std::max_align_t> full_storage((full_bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    home_match::Matcher word_matcher;
    assert(word_matcher.init(full_storage.data(), full_phrases, full_count, full_vocab, vocab_count));
    for (size_t target = 0; target < full_count; ++target) {
        word_matcher.set_begin(target == 0 ? 0 : 1);
        word_matcher.reset(); const char *p = full_phrases[target].text; size_t previous = 0;
        auto feed_token = [&](size_t id) {
            std::vector<float> logits(vocab_count, -8.0f); logits[id] = 5.0f;
            word_matcher.feed(logits.data(), vocab_count, 1.0f);
        };
        while (*p) {
            size_t best = 0, length = 0;
            for (size_t i = 2; i < vocab_count; ++i) {
                const size_t n = std::strlen(full_vocab[i]);
                if (n > length && std::strncmp(p, full_vocab[i], n) == 0) { best = i; length = n; }
            }
            assert(best); if (best == previous) feed_token(0);
            feed_token(best); previous = best; p += length;
        }
        const auto word_result = word_matcher.result(target == 0 ? 0 : 1);
        assert(word_result.best && !std::strcmp(word_result.best->text, full_phrases[target].text));
        assert(word_result.score >= voice_control::kWakeScore);
    }
    const auto tv = voice_catalog::group(voice_catalog::k_tv);
    const size_t tv_bytes = home_match::Matcher::bytes(tv.phrases, tv.count, full_vocab, vocab_count);
    std::vector<std::max_align_t> tv_storage((tv_bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    home_match::Matcher tv_matcher;
    assert(tv_matcher.init(tv_storage.data(), tv.phrases, tv.count, full_vocab, vocab_count));
    const char *short_word = "さん";
    while (*short_word) {
        size_t best = 0, length = 0;
        for (size_t i = 2; i < vocab_count; ++i) {
            const size_t n = std::strlen(full_vocab[i]);
            if (n > length && std::strncmp(short_word, full_vocab[i], n) == 0) { best = i; length = n; }
        }
        assert(best);
        std::vector<float> logits(vocab_count, -8.0f); logits[best] = 5.0f;
        tv_matcher.feed(logits.data(), vocab_count, 1.0f); short_word += length;
    }
    const auto short_result = tv_matcher.result();
    assert(short_result.best && !std::strcmp(short_result.best->text, "さん"));
    assert(voice_control::decide(short_result, "さん", voice_control::Target::tv).accepted);
    assert(short_result.best->kind == int(voice_catalog::Kind::tv_channel) && short_result.best->value == 3);
    const char *vocab[] = {"<blank>", "<unk>", "あ", "い"};
    const home_match::Phrase phrases[] = {{"あい", false}, {"ああ", true}};
    const size_t bytes = home_match::Matcher::bytes(phrases, 2, vocab, 4);
    std::vector<std::max_align_t> storage((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    home_match::Matcher matcher;
    assert(matcher.init(storage.data(), phrases, 2, vocab, 4));
    const float rows[][4] = {{std::log(.1f),std::log(.1f),std::log(.7f),std::log(.1f)},
                             {std::log(.7f),std::log(.1f),std::log(.1f),std::log(.1f)},
                             {std::log(.1f),std::log(.1f),std::log(.1f),std::log(.7f)}};
    for (const auto &row : rows) matcher.feed(row, 4, 1.0f);
    auto result = matcher.result();
    assert(std::strcmp(result.best->text, "あい") == 0);
    assert(std::fabs(result.score - std::log(.7f)) < 1e-5f);
    assert(std::strcmp(result.second->text, "ああ") == 0 && result.second->negative);
    assert(std::fabs(result.score2 - (2 * std::log(.7f) + std::log(.1f)) / 3) < 1e-5f);
    matcher.reset();
    matcher.feed(rows[0], 4, 1.0f); matcher.feed(rows[1], 4, 1.0f); matcher.feed(rows[0], 4, 1.0f);
    result = matcher.result();
    assert(std::strcmp(result.best->text, "ああ") == 0 && result.best->negative);
    assert(std::fabs(result.score - std::log(.7f)) < 1e-5f);
    const home_match::Phrase bad[] = {{"未登録", false}};
    assert(home_match::Matcher::bytes(bad, 1, vocab, 4) == 0);
    puts("PASS full CTC scores, ranking, threshold, timeout, vocabulary");
}
