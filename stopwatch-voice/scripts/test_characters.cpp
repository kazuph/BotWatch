// Pure motion/command contracts: no device stand-ins or substituted dependencies.
// c++ -std=c++17 -I main scripts/test_characters.cpp -o /tmp/test_characters && /tmp/test_characters
#include "character_motion.h"
#include "character_commands.h"
#include "character_cloud.h"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace characters;
    assert(cloud_contains(-16,-20));
    assert(!cloud_contains(-31,-33));
    assert(!cloud_contains(-47,6));
    assert(cloud_color(-16,-20)==0x8fb8ff);
    assert(cloud_color(80,80)==0x2f6fe0);
    struct Case { const char *kana; Command command; };
    const Case commands[]={
        {"じゃんぷして",Command::jump},{"とんで",Command::jump},
        {"ちゃんぷして",Command::jump},{"ちゃんぷしないで",Command::none},
        {"あそんで",Command::play},{"あそぼう",Command::play},
        {"みんなあつまって",Command::gather},{"みんなしゅーごー",Command::gather},
        {"みんなーつまって",Command::gather},{"みんなしゅーご",Command::gather},
        {"みんなーつまらないで",Command::none},{"みんなしゅーごしないで",Command::none},
        {"みんなあつまらないで",Command::none},
        {"うしろをむいて",Command::back},
        {"はいけいをくろにして",Command::black},{"はいけいをあきちにして",Command::field},
        {"はいけーをいえのなかにして",Command::room},{"はいけいをすなはまにして",Command::beach},
        {"はいけーをつなはまにして",Command::beach},{"はいけーをつなはまにしないで",Command::none},
        {"じゃんぷしないで",Command::none},{"あそばなくていい",Command::none},
        {"はいけいをくろにしないで",Command::none},{"うしろをむかないで",Command::none},
        {"てれびをつけて",Command::none},{"らべるをいんさつして",Command::none},
        {"こんにちは",Command::none},{"はいけいをみたことがある",Command::none},
        {"じゃんぷがすき",Command::none},{"じゃんぷしません",Command::none},
        {"とんでもない",Command::none},{"あそんでいた",Command::none},
        {"みんなしゅーごーがすき",Command::none},{"うしろをむいた",Command::none},
        {"はいけいをくろにしてみたい",Command::none},{"じゃんぷしてね",Command::jump},
    };
    for(const auto &c:commands) assert(parse(c.kana)==c.command);
    const double initial=0,target=1,dt=.02;
    double repeated=initial;
    for(int i=0;i<50;++i) repeated=follow(repeated,target,dt);
    assert(std::abs(repeated-follow(initial,target,1))<1e-12);
    for(int i=0;i<6;++i) {
        const double offset=i*.08;
        const Pose home=pose(i,0,0,Move::rest,0,0);
        const Pose crouch=pose(i,0,offset+.1,Move::jump,0,0);
        const Pose air=pose(i,0,offset+.42,Move::jump,0,0);
        const Pose land=pose(i,0,offset+.8,Move::jump,0,0);
        assert(crouch.sy<home.sy && crouch.sx>home.sx);
        assert(air.y < home.y-60);
        assert(land.sy<home.sy && land.sx>home.sx);
        assert(std::abs(pose(i,0,offset+1,Move::jump,0,0).y-home.y)<1e-12);
        const double play_offset=i*.25;
        assert(pose(i,0,play_offset+2,Move::play,0,0).x < -200);
        const Pose entering=pose(i,0,play_offset+3,Move::play,0,0);
        assert(i%2 ? entering.x<0 : entering.x>466);
        assert(std::abs(pose(i,0,play_offset+4.5,Move::play,0,0).x-home.x)<1e-12);
        const Pose single_exit=pose(i,0,play_offset+1.39,Move::play,0,0,true);
        const double half_tile=kTileSide*kSingleScale/2;
        assert(i%2 ? single_exit.x-half_tile>466 : single_exit.x+half_tile<0);
        const Pose single_entry=pose(i,0,play_offset+3,Move::play,0,0,true);
        assert(i%2 ? single_entry.x+half_tile<0 : single_entry.x-half_tile>466);
        assert(pose(i,0,0,Move::rest,3,0).row==10);
        assert(pose(i,0,0,Move::rest,2,0).row==5);
        assert(pose(i,0,0,Move::rest,1,0).row==3);
        for(double t=0;t<6;t+=.01) {
            const Pose p=pose(i,t,t,Move::jump,0,t);
            assert(std::isfinite(p.x)&&std::isfinite(p.y)&&p.sx>0&&p.sy>0);
        }
    }
    printf("PASS commands=%zu spring_semigroup=1 motion_contracts=6\n",sizeof(commands)/sizeof(commands[0]));
}
