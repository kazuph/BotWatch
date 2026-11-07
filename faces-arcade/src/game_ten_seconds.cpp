#include "common.h"
#include "ui_icons.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"
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
int64_t copy_us = 0, max_slice_us = 0;
M5Canvas slice(&M5.Display);
M5Canvas number(&M5.Display);  // running time, pushed straight to the panel
M5Canvas ready0(&M5.Display), ready1(&M5.Display), ready2(&M5.Display);
M5Canvas* const ready[] = {&ready0, &ready1, &ready2};

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
    if (!tenBest(keys[difficulty], &value)) { strlcpy(dst, "まだ", n); return; }
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
    previousRead = started; maxGap = 0; max_slice_us = 0; tile = 0; lastShown = -1;
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
    printf("TEN timing_us serial=%lld pad=%lld sound=%lld draw=%lld transfer=%lld other=%lld wait=%lld\n",
           (long long)ten_timing_max.serial, (long long)ten_timing_max.pad, (long long)ten_timing_max.sound,
           (long long)ten_timing_max.draw, (long long)ten_timing_max.transfer, (long long)ten_timing_max.other,
           (long long)ten_timing_max.wait);
    printf("TEN max_slice_us=%lld clean_gap_us=%lld serial_bytes=%d serial_pad=%d\n", (long long)max_slice_us, (long long)ten_clean_gap, ten_max_serial_bytes, ten_max_serial_pad);
    printf("TEN detail_us led=%lld state=%lld rest=%lld ticks=%lld entry=%lld copy=%lld\n",
           (long long)ten_timing_max.led, (long long)ten_timing_max.state,
           (long long)ten_timing_max.rest, (long long)ten_timing_max.ticks,
           (long long)ten_timing_max.entry, (long long)ten_timing_max.copy);
}
void enter() {
    soundBegin(); ledCurrent = 0;
    for (int i = 0; i < 3; ++i) {
        ready[i]->setColorDepth(16); ready[i]->setPsram(true);
        void* buffer = ready[i]->createSprite(320, 240);
        if (!buffer || !esp_ptr_external_ram(buffer)) {
        for (auto* frame : ready) frame->deleteSprite();
        uiFrame.fillScreen(pal::kBg);
        text(uiFrame, "画面メモリ不足", 160, 100, pal::kRed, F32, top_center);
        uiFrame.pushSprite(0, 0);
        puts("TEN frame=FAIL"); request_menu = true; return;
        }
        ready[i]->fillScreen(pal::kBg);
        text(*ready[i], modes[i], 12, 8, pal::kDim, F24);
        text(*ready[i], "10.00 で A を押す", 160, 150, pal::kDim, F24, top_center);
        text(*ready[i], "SELECT でメニュー", 160, 200, pal::kDim, F24, top_center);
    }
    number.setColorDepth(16); number.setPsram(true);
    if (!number.createSprite(260, 64)) { puts("TEN number=FAIL"); request_menu = true; return; }
    selectScreen();
}
void leave() { soundStop(); soundEnd(); for (auto* frame : ready) frame->deleteSprite(); number.deleteSprite(); }
void icon(int x, int y) { ui_icon::stopwatch(uiGlyph, x, y); }
void tick() {
    copy_us = 0;
    const int64_t entry = int(state);
    const int64_t began = esp_timer_get_time();
    const TickType_t tickBegan = xTaskGetTickCount();
    int64_t ledEnd = began, stateEnd = began;
    struct DetailCapture {
        int64_t began, &ledEnd, &stateEnd;
        TickType_t tickBegan;
        int64_t entry;
        ~DetailCapture() {
            const int64_t end = esp_timer_get_time();
            ten_detail = {ledEnd - began, stateEnd - ledEnd, end - stateEnd - ten_draw_us - ten_transfer_us,
                          int64_t(xTaskGetTickCount() - tickBegan), entry, copy_us};
        }
    } capture{began, ledEnd, stateEnd, tickBegan, entry};
    ten_draw_us = ten_transfer_us = 0;
    tickLed();
    ledEnd = stateEnd = esp_timer_get_time();
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
    stateEnd = esp_timer_get_time();
    maxGap = max(maxGap, input_time_us - previousRead); previousRead = input_time_us;
    if (pad_just & kB) { selectScreen(); return; }
    if ((pad_just & kA) || tap_just) { result(); return; }
    if (tile < 30) {
        auto* pixels = static_cast<uint16_t*>(ready[difficulty]->getBuffer());
        slice.setBuffer(pixels + tile * 8 * 320, 320, 8, 16);
        const int64_t before = esp_timer_get_time();
        slice.pushSprite(0, tile * 8); ten_transfer_us = esp_timer_get_time() - before; max_slice_us = max(max_slice_us, ten_transfer_us); ++tile; return;
    }
    const int64_t elapsed = input_time_us - started;
    const bool hidden = difficulty == 2 || (difficulty == 1 && elapsed >= 3000000);
    const int64_t cs = hidden ? -2 : elapsed / 10000;
    if (cs == lastShown) return;
    lastShown = cs;
    const int64_t before = esp_timer_get_time();
    number.fillSprite(pal::kBg);
    char digits[48];
    if (hidden) strlcpy(digits, "?.??", sizeof(digits));
    else snprintf(digits, sizeof(digits), "%lld.%02lld", (long long)(cs / 100), (long long)(cs % 100));
    text(number, digits, 130, 2, hidden ? pal::kDim : pal::kText, F40, top_center, 1.5f);
    ten_draw_us = esp_timer_get_time() - before;
    const int64_t pushAt = esp_timer_get_time();
    number.pushSprite(30, 66);
    ten_transfer_us = esp_timer_get_time() - pushAt;
}
}
bool tenTimingActive() { return state == State::running; }
const Game kTenSeconds = {"10びょう", nullptr, enter, tick, icon, leave, GameCategory::play, GameFooter::tenBest};
