#pragma once
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>

namespace talk {
struct Reply { const char *word,*caption; };
constexpr Reply kReplies[]={
    {"おはよう", "おはよう！"},
    {"こんにちは", "こんにちは！"},
    {"こんばんは", "こんばんは！"},
    {"おやすみ", "おやすみなさい"},
    {"ありがとう", "どういたしまして"},
    {"げんき", "げんきだよ！"},
    {"ただいま", "おかえりなさい"},
    {"いってきます", "いってらっしゃい"},
    {"つかれた", "おつかれさま"},
    {"おなかすいた", "なにかたべよう"},
    {"すごいね", "えへへ、ありがとう"},
    {"かわいい", "てれちゃうな"},
    {"なまえは", "ボットウォッチだよ"},
    {"ばいばい", "またね"},
    {"またね", "またね"},
    // Minimal surface forms measured in the device CTC replay.
    {"おはよー", "おはよう！"},
    {"こんにちわ", "こんにちは！"},
    {"こんばんわ", "こんばんは！"},
    {"ありがとー", "どういたしまして"},
    {"ただいわ", "おかえりなさい"},
    {"なまえわ", "ボットウォッチだよ"},
    {"わたね", "またね"},
};
inline size_t characters(std::string_view text)
{
    size_t count=0;
    for(unsigned char c:text) if((c&0xc0)!=0x80) ++count;
    return count;
}
inline const char *parse(const std::string &kana)
{
    // TALK-BRIEF: only a leading greeting with at most three following characters.
    constexpr size_t kSuffixCharacters=3;
    for(const char *negative:{"ない","ません","なくて"})
        if(kana.find(negative)!=std::string::npos) return nullptr;
    for(const auto &reply:kReplies)
        if(kana.compare(0,std::strlen(reply.word),reply.word)==0
           && characters(kana)<=characters(reply.word)+kSuffixCharacters) return reply.caption;
    return nullptr;
}
}
