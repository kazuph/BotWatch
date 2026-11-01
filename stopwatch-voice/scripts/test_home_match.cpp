#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "home_match.h"

int main() {
    const char *full_vocab[] = {
#include "vocabulary.inc"
    };
    const home_match::Phrase full_phrases[] = {
#include "home_phrases.inc"
    };
    const size_t full_count = sizeof(full_phrases) / sizeof(full_phrases[0]);
    const size_t vocab_count = sizeof(full_vocab) / sizeof(full_vocab[0]);
    const size_t full_bytes = home_match::Matcher::bytes(full_phrases, full_count, full_vocab, vocab_count);
    assert(full_bytes > 0 && full_bytes < 61340);
    for (const char *required : {"てれびをちでじにして", "てれびをちじょーはにして", "てれびをびーえすにして", "てれびをしーえすにして", "てれびをけさないで"}) {
        bool found = false;
        for (const auto &phrase : full_phrases) if (!std::strcmp(phrase.text, required)) found = true;
        assert(found);
    }
    const char *vocab[] = {"<blank>", "<unk>", "あ", "い"};
    const home_match::Phrase phrases[] = {{"あい", false}, {"ああ", true}, {"い", false}};
    const size_t bytes = home_match::Matcher::bytes(phrases, 3, vocab, 4);
    std::vector<std::max_align_t> storage((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    home_match::Matcher matcher;
    assert(matcher.init(storage.data(), phrases, 3, vocab, 4));
    const float rows[][4] = {{std::log(.1f),std::log(.1f),std::log(.7f),std::log(.1f)},
                             {std::log(.7f),std::log(.1f),std::log(.1f),std::log(.1f)},
                             {std::log(.1f),std::log(.1f),std::log(.1f),std::log(.7f)}};
    for (const auto &row : rows) matcher.feed(row, 4, 1.0f);
    auto result = matcher.result();
    assert(std::strcmp(result.best->text, "あい") == 0);
    assert(std::fabs(result.score - 3 * std::log(.7f) / 2) < 1e-5f);
    assert(std::strcmp(result.second->text, "ああ") == 0 && result.second->negative);
    assert(std::fabs(result.score2 - (2 * std::log(.7f) + std::log(.1f)) / 2) < 1e-5f);
    matcher.reset();
    matcher.feed(rows[0], 4, 1.0f); matcher.feed(rows[1], 4, 1.0f); matcher.feed(rows[0], 4, 1.0f);
    result = matcher.result();
    assert(std::strcmp(result.best->text, "ああ") == 0 && result.best->negative);
    assert(std::fabs(result.score - 3 * std::log(.7f) / 2) < 1e-5f);
    const home_match::Phrase bad[] = {{"未登録", false}};
    assert(home_match::Matcher::bytes(bad, 1, vocab, 4) == 0);
    puts("PASS Viterbi scores, ranking, repeated token, negative, vocabulary");
}
