// Host test of main/home.cpp's kana rules (the panel drawing is stubbed).
//   c++ -std=c++17 -I main -I scripts/host_stubs scripts/test_home_parse.cpp main/home.cpp -o test_home && ./test_home
#include <cstdio>
#include <string>

#include "home.h"
#include "voice_screen.h"

namespace voice_screen {
void home_panel(const Tile *, int, int, const std::string &, const std::string &) {}
}

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
        {"てれびをちじょーはにして", K::tv_band, 0, 0}, {"びーえすにして", K::tv_band, 0, 1},
        {"てれびをしーえすにして", K::tv_band, 0, 2}, {"てれびをけさないで", K::none, 0, 0},
        {"りえすにして", K::tv_band, 0, 1}, {"てれびをしんえーすにして", K::tv_band, 0, 2},  // seen on the board
        {"えあこんのれーぼーにして", K::aircon_mode, 1, 0}, {"おんどにじゅーにどにして", K::temperature, 1, 22},
        {"きょうはいいてんきです", K::none, 0, 0},
        {"らいとをけして", K::none, 0, 0},  // KoeAct's set_led
        {"わらって", K::none, 0, 0}, {"かおをみぎによんじゅーごどむけて", K::none, 0, 0},
    };
    int failed = 0;
    for (const Case &c : cases) {
        const home::Command got = home::parse(c.kana);
        const bool ok = got.kind == c.kind && (c.kind == K::none || (got.device == c.device && got.value == c.value));
        if (!ok) {
            ++failed;
            printf("FAIL %s kind=%d device=%d value=%d\n", c.kana, (int)got.kind, got.device, got.value);
        }
    }
    printf("%s %d/%d\n", failed ? "FAIL" : "PASS", (int)(sizeof(cases) / sizeof(cases[0])) - failed,
           (int)(sizeof(cases) / sizeof(cases[0])));
    return failed != 0;
}
