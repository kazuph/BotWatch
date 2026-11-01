// Check that prior appliance operations remain represented by the staged voice catalog.
#include <cstdio>
#include <string>

#include "home.h"
#include "voice_catalog.h"

int main()
{
    using K = home::Command::Kind;
    struct Case { const char *kana; K kind; int device, value; };
    const Case cases[] = {
        {"てれびをつけて", K::power, 0, 1}, {"てれびをけして", K::power, 0, 0}, {"てれびをおんにして", K::power, 0, 1},
        {"てれびおふ", K::power, 0, 0}, {"えあこんをつけて", K::power, 1, 1}, {"えあこんおふ", K::power, 1, 0},
        {"でんきをつけて", K::power, 2, 1}, {"でんきをけして", K::power, 2, 0}, {"あかりをけして", K::power, 2, 0},
        {"あかりおん", K::power, 2, 1}, {"えあこんをれいぼーにして", K::aircon_mode, 1, 0},
        {"だんぼーにして", K::aircon_mode, 1, 1}, {"どらいにして", K::aircon_mode, 1, 2},
        {"おんどをにじゅーはちどにして", K::temperature, 1, 28}, {"おんどおにじゅーごどにして", K::temperature, 1, 25},
        {"えあこんをじゅーはちどにして", K::temperature, 1, 18}, {"おんどをさんじゅーどにして", K::temperature, 1, 30},
        {"てれびのおんりょーをごあげて", K::tv_volume, 0, 5}, {"おんりょーをさんさげて", K::tv_volume, 0, -3},
        {"おんりょーをじゅーあげて", K::tv_volume, 0, 10}, {"てれびのおんりょーをあげて", K::tv_volume, 0, 1},
        {"おんりょーをあげて", K::none, 0, 0},  // KoeAct's adjust_volume keeps this
        {"てれびをちじょーはにして", K::none, 0, 0}, {"びーえすにして", K::none, 0, 0},
        {"ちでじにして", K::none, 0, 0}, {"てれびをちでじにかえて", K::none, 0, 0},
        {"ちだじにして", K::none, 0, 0}, {"ちでつにして", K::none, 0, 0},
        {"ちだずにして", K::none, 0, 0}, {"ひじょーはんにして", K::none, 0, 0},
        {"ひじょーはにして", K::none, 0, 0},
        {"てれびをつだじにして", K::none, 0, 0}, {"それびをちょとじにして", K::none, 0, 0},
        {"くれびをじょじょーはにして", K::none, 0, 0}, {"てれびをちじょーはにかえて", K::none, 0, 0},
        {"てれびをちでじにきりかえて", K::none, 0, 0}, {"てれびをちじょーはにきりかえて", K::none, 0, 0},
        {"それびをびーえすにして", K::none, 0, 0}, {"くれびをしーえすにして", K::none, 0, 0},
        {"てれびをびーえすじにして", K::none, 0, 0},
        {"てれびをはちじにして", K::none, 0, 0}, {"てれびをさんじにして", K::none, 0, 0},
        {"てれびを8じにして", K::none, 0, 0}, {"てれびをつだじにしないで", K::none, 0, 0},
        {"てれびをつだじになくて", K::none, 0, 0},
        {"あちわいてとーわてれびをつだじゅましてごずの…", K::none, 0, 0},
        {"てれびをちれじゅにしてかなたのかまさまみぎくれるんとちをげ", K::none, 0, 0},
        {"をちじょーはにして", K::none, 0, 0}, {"をちじょーはんにかえて", K::none, 0, 0},
        {"もーせのてれびをつじょーはんにして", K::none, 0, 0},
        {"てれびをちょじょーはんにしてて", K::none, 0, 0},
        {"ちじょーはにして", K::none, 0, 0}, {"てれびをちじょーはにしないで", K::none, 0, 0},
        {"をちじょーはですにして", K::none, 0, 0}, {"あをちじょーはにして", K::none, 0, 0},
        {"てれびをつだじにしてあと", K::none, 0, 0},
        {"てれびをつだじにかえてあと", K::none, 0, 0},
        {"てれびをつだじにきりかえてあと", K::none, 0, 0},
        {"てれびをつだじゅにして", K::none, 0, 0},
        {"てれびをはちじゅにして", K::none, 0, 0},
        {"てれびをじゅにして", K::none, 0, 0},
        {"ちだじ", K::none, 0, 0}, {"ちでつ", K::none, 0, 0}, {"ちだず", K::none, 0, 0},
        {"ひじょーはん", K::none, 0, 0}, {"ひじょーは", K::none, 0, 0},
        {"ちでじ", K::none, 0, 0}, {"ちじょーは", K::none, 0, 0},
        {"ちだじにしないで", K::none, 0, 0}, {"ひじょーはにしないで", K::none, 0, 0},
        {"ちだじはみないで", K::none, 0, 0},
        {"びーえす", K::none, 0, 0}, {"しーえす", K::none, 0, 0},
        {"ちゃんねるいちにして", K::tv_channel, 0, 1}, {"ちゃんねるはちにして", K::tv_channel, 0, 8},
        {"ちゃんねるきゅーにして", K::tv_channel, 0, 9},
        {"ちゃんねるあげて", K::tv_channel_step, 0, 1}, {"ちゃんねるさげて", K::tv_channel_step, 0, -1},
        {"ちゃんねるをあげないで", K::none, 0, 0},
        {"おれんじにして", K::light_mode, 2, 0}, {"ほあんとうにして", K::light_mode, 2, 1},
        {"しろにして", K::light_mode, 2, 2},
        {"てれびをしーえすにして", K::none, 0, 0}, {"てれびをけさないで", K::none, 0, 0},
        {"りえすにして", K::none, 0, 0}, {"てれびをしんえーすにして", K::none, 0, 0},  // seen on the board
        {"えあこんのれーぼーにして", K::aircon_mode, 1, 0}, {"おんどにじゅーにどにして", K::temperature, 1, 22},
        {"きょうはいいてんきです", K::none, 0, 0},
        {"らいとをけして", K::none, 0, 0},  // KoeAct's set_led
        {"わらって", K::none, 0, 0}, {"かおをみぎによんじゅーごどむけて", K::none, 0, 0},
    };
    int failed = 0, checked = 0;
    for (const Case &c : cases) {
        if (c.kind == K::none) continue;
        using V = voice_catalog::Kind;
        const V kind = c.kind == K::power ? V::power : c.kind == K::aircon_mode ? V::aircon_mode :
            c.kind == K::temperature ? V::temperature : c.kind == K::tv_volume ? V::tv_volume :
            c.kind == K::tv_channel ? V::tv_channel : c.kind == K::tv_channel_step ? V::tv_channel_step : V::light_mode;
        const auto group = c.device == 0 ? voice_catalog::group(voice_catalog::k_tv) :
                           c.device == 1 ? voice_catalog::group(voice_catalog::k_aircon) : voice_catalog::group(voice_catalog::k_light);
        bool found = false;
        for (size_t i = 0; i < group.count; ++i)
            if (group.phrases[i].kind == int(kind) && group.phrases[i].value == c.value) found = true;
        ++checked;
        if (!found) {
            ++failed;
            printf("FAIL %s kind=%d device=%d value=%d\n", c.kana, int(c.kind), c.device, c.value);
        }
    }
    printf("%s prior operation mappings %d/%d\n", failed ? "FAIL" : "PASS", checked - failed, checked);
    return failed != 0;
}
