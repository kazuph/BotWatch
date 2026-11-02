#include "talk_commands.h"
#include "voice_catalog.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    for(size_t i=0;i<sizeof(talk::kReplies)/sizeof(talk::kReplies[0]);++i) {
        const auto &reply=talk::kReplies[i]; bool found=false;
        for(const auto &phrase:voice_catalog::k_idle)
            if(!std::strcmp(phrase.text,reply.word) && phrase.kind==int(voice_catalog::Kind::talk) && phrase.value==int(i)) found=true;
        assert(found && reply.caption && *reply.caption);
    }
    for(const char *negative:{"げんきがない","かわいくない","ありがとうじゃない"}) {
        bool found=false;
        for(const auto &phrase:voice_catalog::k_idle) if(!std::strcmp(phrase.text,negative) && phrase.negative) found=true;
        assert(found);
    }
    std::puts("PASS talk replies and negative candidate mappings");
}
