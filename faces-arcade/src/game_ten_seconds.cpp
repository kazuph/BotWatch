#include "common.h"
#include "ui_icons.h"
#include "esp_timer.h"
#include <climits>

namespace {
constexpr const char* keys[] = {"t10_show", "t10_hide3", "t10_blind"};
constexpr const char* modes[] = {"ずっと見える", "3秒で消える", "最初から見えない"};
enum class State { select, running, result };
State state = State::select;
int difficulty = 1, grade = 0;
int64_t started = 0, stopped = 0, resultAt = 0, previousRead = 0, maxGap = 0;
bool newBest = false;
int64_t ledAt = 0;
int ledFrom = 0, ledTo = 0, ledCurrent = 0;
int tile = 0;
int64_t lastShown = -1;
M5Canvas slice(&M5.Display);

void ramp(int target) { ledFrom = ledCurrent; ledTo = target; ledAt = esp_timer_get_time(); }
void tickLed() {
    const int64_t elapsed = std::min<int64_t>(esp_timer_get_time() - ledAt, 1200000);
    ledCurrent = ledFrom + int((ledTo - ledFrom) * elapsed / 1200000);
#if CONFIG_IDF_TARGET_ESP32S3
    arcade_led::green_level(ledCurrent);
#endif
}
void full() { uiFrame.pushSprite(0, 0); }
void bestText(char* dst, size_t n) {
    int32_t value;
    if (!tenBest(keys[difficulty], &value)) { strlcpy(dst, "—", n); return; }
    const int64_t cs = (int64_t(value) + 5) / 10;
    snprintf(dst, n, "%lld.%02lld秒", (long long)(cs / 100), (long long)(cs % 100));
}
void selectScreen() {
    state = State::select; soundStop(); ramp(0);
    auto& d = uiFrame; d.fillScreen(pal::kBg);
    text(d, "10秒ぴったり", 160, 8, pal::kText, F32, top_center);
    for (int i = 0; i < 3; ++i) {
        const int y = 62 + 40 * i;
        if (i == difficulty) d.fillRoundRect(40, y, 240, 38, 12, pal::kAccent);
        text(d, modes[i], 160, y + 6, i == difficulty ? pal::kBg : pal::kText, F24, top_center);
    }
    char best[40], line[80]; bestText(best, sizeof(best));
    snprintf(line, sizeof(line), "BEST %s A スタート", best);
    text(d, line, 160, 200, pal::kAccent, F24, top_center); full();
}
void start() {
    soundStop(); ramp(0); state = State::running; started = input_time_us;
    previousRead = started; maxGap = 0; tile = 0; lastShown = -1;
    auto& d = uiFrame; d.fillScreen(pal::kBg);
    text(d, modes[difficulty], 12, 8, pal::kDim, F24);
    text(d, "10.00 で A を押す", 160, 150, pal::kDim, F24, top_center);
    text(d, "SELECT でメニュー", 160, 200, pal::kDim, F24, top_center);
}
void result() {
    stopped = input_time_us; state = State::result; resultAt = stopped;
    const int64_t elapsed = stopped - started, err = elapsed - 10000000;
    const int64_t absolute = err < 0 ? -err : err;
    grade = absolute < 5000 ? 0 : absolute <= 50000 ? 1 : absolute <= 200000 ? 2 : 3;
    const int64_t ms = (absolute + 500) / 1000;
    newBest = ms <= INT32_MAX && submitTenBest(keys[difficulty], int32_t(ms));
    const int64_t cs = (elapsed + 5000) / 10000, diff = (absolute + 5000) / 10000;
    auto& d = uiFrame; d.fillScreen(pal::kBg);
    const char* labels[] = {"ぴったり！", "すごい！", "おしい！", "ざんねん"};
    text(d, labels[grade], 160, 8, pal::kAccent, F32, top_center);
    char number[48], delta[56];
    snprintf(number, sizeof(number), "%lld.%02lld", (long long)(cs / 100), (long long)(cs % 100));
    snprintf(delta, sizeof(delta), "%s%lld.%02lld秒", diff == 0 ? "±" : err < 0 ? "−" : "+", (long long)(diff / 100), (long long)(diff % 100));
    text(d, number, 160, 54, pal::kText, F40, top_center, 1.5f);
    text(d, delta, 160, 134, pal::kCyan, F32, top_center);
    if (newBest) text(d, "NEW BEST", 160, 176, pal::kAccent, F24, top_center);
    text(d, "A もう一回 B 難しさ", 160, 208, pal::kDim, F24, top_center); full();
    const int levels[] = {64, 40, 16, 0}; ramp(levels[grade]);
    if (grade == 0) { soundPlay(0,120,72); soundPlay(120,120,76); soundPlay(240,120,79); soundPlay(360,400,84,79,76); }
    if (grade == 1) { soundPlay(0,150,76); soundPlay(150,300,79,83); }
    if (grade == 2) { soundPlay(0,200,69); soundPlay(200,300,72); }
    if (grade == 3) { soundPlay(0,300,60); soundPlay(300,400,55); }
    printf("TEN elapsed_us=%lld max_gap_us=%lld grade=%d best=%d\n", (long long)elapsed, (long long)maxGap, grade, newBest);
}
void enter() { soundBegin(); ledCurrent = 0; selectScreen(); }
void leave() { soundStop(); soundEnd(); }
void icon(int x, int y) { ui_icon::stopwatch(uiGlyph, x, y); }
void tick() {
    tickLed();
    if (state == State::select) {
        if (pad_just & kUp) difficulty = max(0, difficulty - 1);
        else if (pad_just & kDown) difficulty = min(2, difficulty + 1);
        else if (tap_just && tap_y >= 62 && tap_y < 182 && tap_x >= 40 && tap_x < 280) { difficulty = (tap_y - 62) / 40; start(); return; }
        else if (pad_just & kA) { start(); return; }
        else return;
        selectScreen(); return;
    }
    if (state == State::result) {
        if (pad_just & kB) { selectScreen(); return; }
        if (input_time_us - resultAt >= 300000 && ((pad_just & kA) || tap_just)) start();
        return;
    }
    maxGap = max(maxGap, input_time_us - previousRead); previousRead = input_time_us;
    if (pad_just & kB) { selectScreen(); return; }
    if ((pad_just & kA) || tap_just) { result(); return; }
    if (tile < 30) {
        auto* pixels = static_cast<uint16_t*>(uiFrame.getBuffer());
        slice.setBuffer(pixels + tile * 8 * 320, 320, 8, 16);
        slice.pushSprite(0, tile * 8); ++tile; return;
    }
    const int64_t elapsed = input_time_us - started;
    const bool hidden = difficulty == 2 || (difficulty == 1 && elapsed >= 3000000);
    const int64_t cs = hidden ? -2 : elapsed / 10000;
    if (cs == lastShown) return;
    lastShown = cs;
    uiGlyph.fillSprite(pal::kBg);
    char number[48];
    if (hidden) strlcpy(number, "?.??", sizeof(number));
    else snprintf(number, sizeof(number), "%lld.%02lld", (long long)(cs / 100), (long long)(cs % 100));
    text(uiGlyph, number, 96, 0, pal::kText, F40, top_center);
    uiGlyph.pushSprite(64, 78);
}
}
const Game kTenSeconds = {"10秒ぴったり", nullptr, enter, tick, icon, leave, GameCategory::play, GameFooter::tenBest};
