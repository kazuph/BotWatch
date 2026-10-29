#include "talk_commands.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    for(const auto &reply:talk::kReplies) {
        assert(std::strcmp(talk::parse(reply.word),reply.caption)==0);
        assert(talk::parse(std::string(reply.word)+"だよね"));
        assert(!talk::parse(std::string(reply.word)+"なんだけど"));
        assert(!talk::parse(std::string(reply.word)+"ない"));
        assert(!talk::parse("きょうは"+std::string(reply.word)));
    }
    for(const char *text:{"げんきがない","かわいくない","ありがとうじゃない","おはようと言わないで",
            "おなかすいたけどまだたべられない","てれびをつけて","えあこんをけして",
            "らべるをいんさつして","ちゃんぷして","みんなあつまって","はいけいをくろにして",
            "みぎをむいて","わらって","おんりょうをごじゅーにして"}) assert(!talk::parse(text));
    assert(talk::parse("おやすみなさい"));
    std::puts("PASS talk=14 synonym=1 prefix_suffix_boundary=3 negatives=1 conversation=1 existing_commands=1");
}
