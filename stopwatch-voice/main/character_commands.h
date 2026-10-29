#pragma once
#include <string>
#include <initializer_list>
namespace characters {
enum class Command { none, jump, play, back, black, field, room, beach, gather };
inline Command parse(const std::string &kana)
{
    auto has=[&](std::initializer_list<const char *> words) {
        for(const char *word:words) if(kana.find(word)!=std::string::npos) return true;
        return false;
    };
    auto ends=[&](std::initializer_list<const char *> words) {
        for(const char *word:words) {
            const size_t size=std::char_traits<char>::length(word);
            if(kana.size()>=size && kana.compare(kana.size()-size,size,word)==0) return true;
        }
        return false;
    };
    if(has({"ないで","なくて","しません"})) return Command::none;
    if(has({"みんな"}) && ends({"あつまって","ーつまって","あつまってね","しゅーご","しゅーごー","しゅーごう","しゅうごう"})) return Command::gather;
    if(has({"はいけい","はいけー","はいけえ"}) && ends({"にして","にしてね","にかえて","にかえてね"})) {
        if(has({"くろ","ほし"})) return Command::black;
        if(has({"あきち","あきぢ"})) return Command::field;
        if(has({"いえ","へや"})) return Command::room;
        if(has({"すなはま","つなはま","うみ"})) return Command::beach;
    }
    if(ends({"じゃんぷして","ちゃんぷして","じゃんぷしてね","ちゃんぷしてね","とんで","とんでね","とびはねて","とびはねてね"})) return Command::jump;
    if(ends({"あそんで","あそんでね","あそぼう","あそぼー"})) return Command::play;
    if(has({"うしろ"}) && ends({"むいて","むいてね","むけて","みて","まわって"})) return Command::back;
    return Command::none;
}
}
