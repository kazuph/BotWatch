// FACES ARCADE: menu for M5Stack Core2 + Faces Gamepad3.
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/select.h>
#include <unistd.h>
#include <algorithm>
#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif
#include "common.h"
#include "ui_icons.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "photo_storage.h"
#include "network_serial.h"
#include "network_settings.h"
#include "network_job.h"
#include "ruby_service.h"
#endif

namespace {
M5Faces_Gamepad3 gamepad;
nvs_handle_t prefs = 0;
uint8_t pad_injected   = 0;  // set over serial for self tests
bool tap_injected      = false;
int injected_x = 0, injected_y = 0;

const Game* const kGames[] = {&kNoteQuiz, &kChordQuiz, &kKanaQuiz, &kSpectrum, &kBlockDrop, &kSnake, &kFlappy, &kTenSeconds
#if CONFIG_IDF_TARGET_ESP32S3
    , &kCamera, &kRuby
#endif
};
const int kGameCount       = sizeof(kGames) / sizeof(kGames[0]);
int menuBest[kGameCount] = {};
int cursor                 = 0;
int slideFrom = 0, slideTo = 0, slideDirection = 0;
uint32_t slideAt = 0;
uint32_t slideFrames = 0, firstSlideFrameMs = 0;
uint32_t dotAt = 0;
uint16_t dotFrom = 0, dotTo = 0;
bool dotFading = false;
bool sliding = false, menuTouchActive = false, injectedTapThisRead = false;
int menuBattery = -1;
#if CONFIG_IDF_TARGET_ESP32S3
bool menuPhotoReady = false;
unsigned menuPhotoCount = 0;
bool menuLink = false, linkFading = false;
int linkFrom = 0, linkTo = 0;
uint32_t linkAt = 0;
#endif
int touchStartX = 0, touchStartY = 0;
enum MenuAction { NONE, PREVIOUS, NEXT, WRAP_PREVIOUS, WRAP_NEXT, OPEN };
MenuAction pendingAction = NONE;

void logMemory(const char* phase) {
    printf("MEM phase=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u\n", phase,
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
const Game* running        = nullptr;

void padRead() {
    M5.update();
    gamepad.update();
    uint8_t now = pad_injected;
    // The pad is active-low. Raw 0x00 means no valid sample yet, not "all eight pressed".
    if (gamepad.getState().raw != 0x00) now |= static_cast<uint8_t>(~gamepad.getState().raw);
    pad_just = now & ~pad_now;
    pad_now  = now;

    pad_exit = pad_just & kSelect;

    tap_just = false;
    injectedTapThisRead = false;
    const auto touch = M5.Touch.getDetail();
    if (touch.wasPressed()) {
        tap_just = true;
        tap_x    = touch.x;
        tap_y    = touch.y;
    }
    if (tap_injected) {
        tap_injected = false;
        tap_just     = true;
        injectedTapThisRead = true;
        tap_x        = injected_x;
        tap_y        = injected_y;
    }
    input_time_us = esp_timer_get_time();
    if (tap_just) printf("GAMES tap x=%d y=%d\n", tap_x, tap_y);
}

uint16_t categoryColor(int index) {
    if (kGames[index]->category == GameCategory::learn) return rgb(79, 209, 255);
    if (kGames[index]->category == GameCategory::listen) return rgb(91, 227, 138);
    if (kGames[index]->category == GameCategory::play) return rgb(255, 92, 138);
    return rgb(155, 123, 255);
}

uint16_t categoryDark(int index) {
    if (kGames[index]->category == GameCategory::learn) return rgb(31, 90, 120);
    if (kGames[index]->category == GameCategory::listen) return rgb(29, 106, 60);
    if (kGames[index]->category == GameCategory::play) return rgb(122, 31, 82);
    return rgb(59, 37, 128);
}

uint16_t blendColor(uint16_t from, uint16_t to, uint32_t elapsed) {
    elapsed = min(elapsed, uint32_t(300));
    const auto mix = [elapsed](int a, int b) { return (a * (300 - elapsed) + b * elapsed) / 300; };
    return (mix((from >> 11) & 31, (to >> 11) & 31) << 11) |
           (mix((from >> 5) & 63, (to >> 5) & 63) << 5) | mix(from & 31, to & 31);
}

#if CONFIG_IDF_TARGET_ESP32S3
int linkBlend(uint32_t now) {
    const int elapsed = min<uint32_t>(now - linkAt, 300);
    return linkFrom + (linkTo - linkFrom) * elapsed / 300;
}

void drawLink() {
    if (!linkFading) { ui_icon::link(uiFrame, 258, 17, menuLink); return; }
    uint16_t background[13 * 13], disconnected[13 * 13];
    for (int y = 0; y < 13; ++y)
        for (int x = 0; x < 13; ++x) background[y * 13 + x] = uiFrame.readPixel(252 + x, 11 + y);
    uiGlyph.fillSprite(TFT_BLACK);
    for (int y = 0; y < 13; ++y)
        for (int x = 0; x < 13; ++x) uiGlyph.drawPixel(x, y, background[y * 13 + x]);
    ui_icon::link(uiGlyph, 6, 6, false);
    for (int y = 0; y < 13; ++y)
        for (int x = 0; x < 13; ++x) disconnected[y * 13 + x] = uiGlyph.readPixel(x, y);
    for (int y = 0; y < 13; ++y)
        for (int x = 0; x < 13; ++x) uiGlyph.drawPixel(x, y, background[y * 13 + x]);
    ui_icon::link(uiGlyph, 6, 6, true);
    const int amount = linkBlend(lgfx::millis());
    for (int y = 0; y < 13; ++y)
        for (int x = 0; x < 13; ++x)
            uiFrame.drawPixel(252 + x, 11 + y,
                              blendColor(disconnected[y * 13 + x], uiGlyph.readPixel(x, y), amount));
}
#endif

const char* categoryName(int index) {
    if (kGames[index]->category == GameCategory::learn) return "まなぶ";
    if (kGames[index]->category == GameCategory::listen) return "きく";
    if (kGames[index]->category == GameCategory::play) return "あそぶ";
    return "みる";
}

void drawMenuCard(int index, int x) {
    if (index < 0 || index >= kGameCount || x >= 320 || x + 240 <= 0) return;
    auto& d = uiFrame;
    d.setClipRect(max(0, x), 38, min(320, x + 240) - max(0, x), 166);
    uiGradientCard(x, 38, 240, 166, 18, categoryColor(index), categoryDark(index));
    darkenRoundRect(x + 10, 46, 88, 30, 15);
    text(d, categoryName(index), x + 54, 49, pal::kText, F24, top_center);
    if (x + 200 > 0 && x + 140 < 320) {
        uiGlyph.fillSprite(TFT_BLACK);
        kGames[index]->icon(0, 0);
        uiGlyph.setPivot(20, 20);
        uiGlyph.pushRotateZoomWithAA(&d, x + 170, 68, 0, 1.5f, 1.5f, TFT_BLACK);
        uiGlyph.setPivot(96, 24);
    }
    char label[64];
    strlcpy(label, kGames[index]->name, sizeof(label));
    while (d.textWidth(label, F40) > 220 && strlen(label)) {
        size_t n = strlen(label);
        do { --n; } while (n && (static_cast<unsigned char>(label[n]) & 0xC0) == 0x80);
        label[n] = 0;
    }
    if (strcmp(label, kGames[index]->name) != 0) {
        while (d.textWidth(label, F40) + d.textWidth("…", F40) > 220 && strlen(label)) {
            size_t n = strlen(label);
            do { --n; } while (n && (static_cast<unsigned char>(label[n]) & 0xC0) == 0x80);
            label[n] = 0;
        }
        strlcat(label, "…", sizeof(label));
    }
    text(d, label, x + 120, 111, pal::kText, F40, top_center);
    darkenRoundRect(x + 32, 164, 176, 32, 16);
    if (kGames[index]->footer == GameFooter::tenBest) {
        char score[32];
        if (menuBest[index] < 0) strlcpy(score, "まだ", sizeof(score));
        else { const int64_t cs = (int64_t(menuBest[index]) + 5) / 10; snprintf(score, sizeof(score), "%lld.%02lld秒", (long long)(cs / 100), (long long)(cs % 100)); }
        text(d, "BEST", x + 46, 168, pal::kAccent, F24);
        text(d, score, x + 194, 168, pal::kText, F24, top_right);
    } else if (kGames[index]->footer == GameFooter::score) {
        char score[20];
        snprintf(score, sizeof(score), "%d", menuBest[index]);
        const int scoreWidth = 148 - d.textWidth("BEST", F24) - 8;
        if (d.textWidth(score, F24) > scoreWidth) {
            size_t n = strlen(score);
            while (n && d.textWidth(score, F24) + d.textWidth("…", F24) > scoreWidth) score[--n] = 0;
            strlcat(score, "…", sizeof(score));
        }
        text(d, "BEST", x + 46, 168, pal::kAccent, F24);
        text(d, score, x + 194, 168, pal::kText, F24, top_right);
    } else {
        if (kGames[index]->footer == GameFooter::sound) text(d, "音をきく", x + 120, 168, pal::kText, F24, top_center);
#if CONFIG_IDF_TARGET_ESP32S3
        else {
            char status[32];
            if (menuPhotoReady) snprintf(status, sizeof(status), "写真 %uまい", menuPhotoCount);
            else strlcpy(status, "SD なし", sizeof(status));
            text(d, status, x + 120, 168, pal::kText, F24, top_center);
        }
#endif
    }
    d.clearClipRect();
}

void drawMenu() {
    auto& d = uiFrame;
    uiRadialBackground(rgb(26, 31, 74), rgb(7, 9, 18));
    text(d, "FACES", 8, 3, pal::kText, F24);
    text(d, "ARCADE", 104, 3, rgb(255, 92, 138), F24);
#if CONFIG_IDF_TARGET_ESP32S3
    drawLink();
#else
    ui_icon::link(d, 258, 17, false);
#endif
    if (menuBattery >= 0) ui_icon::battery(d, 274, 9, menuBattery);
    const int elapsed = sliding ? min<uint32_t>(lgfx::millis() - slideAt, 150) : 0;
    const int offset = sliding ? 280 * elapsed / 150 : 0;
    if (sliding) {
        drawMenuCard(slideFrom, 40 - slideDirection * offset);
        drawMenuCard(slideTo, 40 + slideDirection * (280 - offset));
    } else {
        drawMenuCard(cursor - 1, -210);
        drawMenuCard(cursor + 1, 290);
        drawMenuCard(cursor, 40);
    }
    if (cursor > 0) text(d, "◀", 6, 111, pal::kText, F24);
    if (cursor + 1 < kGameCount) text(d, "▶", 314, 111, pal::kText, F24, top_right);
    const int start = 160 - ((kGameCount - 1) * 14) / 2;
    for (int i = 0; i < kGameCount; ++i)
        d.fillSmoothCircle(start + i * 14, 222, 5, rgb(58, 65, 128));
    const int dotX = sliding ? start + slideFrom * 14 + (slideTo - slideFrom) * 14 * elapsed / 150
                             : start + cursor * 14;
    d.fillSmoothRoundRect(dotX - 7, 217, 14, 10, 5, blendColor(dotFrom, dotTo, lgfx::millis() - dotAt));
    d.pushSprite(0, 0);
    if (sliding) {
        ++slideFrames;
        if (slideFrames == 1) firstSlideFrameMs = lgfx::millis() - slideAt;
    }
}

void menuEnter() {
    sliding = false;
    pendingAction = NONE;
    menuTouchActive = false;
    menuBattery = M5.Power.getBatteryLevel();
    for (int i = 0; i < kGameCount; ++i)
        if (kGames[i]->footer == GameFooter::tenBest) { int32_t value; menuBest[i] = tenBest("t10_hide3", &value) ? value : -1; }
        else menuBest[i] = kGames[i]->scoreKey ? bestScore(kGames[i]->scoreKey) : 0;
#if CONFIG_IDF_TARGET_ESP32S3
    char error[80];
    menuPhotoReady = photo_storage::count_photos(&menuPhotoCount, error, sizeof(error));
    menuLink = network_job::link_connected();
    linkFrom = linkTo = menuLink ? 300 : 0;
    linkAt = lgfx::millis();
    linkFading = false;
#endif
    dotFrom = dotTo = categoryColor(cursor);
    dotFading = false;
    drawMenu();
    puts("GAMES menu");
}

void leaveCurrentGame() {
    request_menu = false;
    if (running && running->leave) running->leave();
#if CONFIG_IDF_TARGET_ESP32S3
    arcade_led::clear();
#endif
    running = nullptr;
}

void openCursor() {
    running = kGames[cursor];
    const char* name = running->scoreKey ? running->scoreKey : "spectrum";
    if (running == &kTenSeconds) name = "ten_seconds";
#if CONFIG_IDF_TARGET_ESP32S3
    if (running == &kCamera) name = "camera";
#endif
    printf("GAMES play=%s\n", name);
    running->enter();
}

void menuAction(MenuAction action) {
    if (action == NONE) return;
    if (sliding) {
        if (pendingAction == NONE) pendingAction = action;
        return;
    }
    if (action == OPEN) { openCursor(); return; }
    const int direction = (action == PREVIOUS || action == WRAP_PREVIOUS) ? -1 : 1;
    int next = cursor + direction;
    if (action == WRAP_PREVIOUS || action == WRAP_NEXT) next = (next + kGameCount) % kGameCount;
    if (next < 0 || next >= kGameCount) return;
    slideFrom = cursor;
    slideTo = next;
    slideDirection = direction;
    slideAt = lgfx::millis();
    dotFrom = blendColor(dotFrom, dotTo, slideAt - dotAt);
    dotTo = categoryColor(next);
    dotAt = slideAt;
    dotFading = dotFrom != dotTo;
    slideFrames = 0;
    firstSlideFrameMs = 0;
    sliding = true;
}

void menuTick() {
#if CONFIG_IDF_TARGET_ESP32S3
    const bool currentLink = network_job::link_connected();
    if (currentLink != menuLink) {
        linkFrom = linkBlend(lgfx::millis());
        linkTo = currentLink ? 300 : 0;
        linkAt = lgfx::millis();
        linkFading = true;
        menuLink = currentLink;
    }
#endif
    MenuAction action = NONE;
    if (pad_just & kUp) action = WRAP_PREVIOUS;
    else if (pad_just & kDown) action = WRAP_NEXT;
    else if (pad_just & kLeft) action = PREVIOUS;
    else if (pad_just & kRight) action = NEXT;
    else if (pad_just & (kA | kStart)) action = OPEN;
    const auto touch = M5.Touch.getDetail();
    if (touch.wasPressed()) {
        menuTouchActive = true;
        touchStartX = touch.x;
        touchStartY = touch.y;
    }
    if (menuTouchActive && touch.wasReleased()) {
        menuTouchActive = false;
        const int dx = touch.x - touchStartX, dy = touch.y - touchStartY;
        if (dx * dx + dy * dy <= 12 * 12) {
#if CONFIG_IDF_TARGET_ESP32S3
            if (!sliding && kGames[cursor] == &kCamera && touch.x >= 68 && touch.x < 252 && touch.y >= 160 && touch.y < 200) {
                running = &kPhotoViewer; running->enter(); return;
            }
#endif
            if (touch.x >= 40 && touch.x < 280 && touch.y >= 38 && touch.y < 204) action = OPEN;
        } else if (abs(dx) > abs(dy)) action = dx < 0 ? NEXT : PREVIOUS;
    }
    if (injectedTapThisRead && tap_x >= 40 && tap_x < 280 && tap_y >= 38 && tap_y < 204) {
#if CONFIG_IDF_TARGET_ESP32S3
        if (!sliding && kGames[cursor] == &kCamera && tap_x >= 68 && tap_x < 252 && tap_y >= 160 && tap_y < 200) {
            running = &kPhotoViewer; running->enter(); return;
        }
#endif
        action = OPEN;
    }
    menuAction(action);
    if (sliding) {
        if (lgfx::millis() - slideAt >= 150) {
            const uint32_t duration = lgfx::millis() - slideAt;
            printf("MENU slide frames=%lu duration_ms=%lu fps=%.1f input_to_push_ms=%lu\n",
                   static_cast<unsigned long>(slideFrames), static_cast<unsigned long>(duration),
                   duration ? slideFrames * 1000.0 / duration : 0.0, static_cast<unsigned long>(firstSlideFrameMs));
            cursor = slideTo;
            sliding = false;
            drawMenu();
            const MenuAction queued = pendingAction;
            pendingAction = NONE;
            menuAction(queued);
        } else drawMenu();
    } else if (!running && (dotFading
#if CONFIG_IDF_TARGET_ESP32S3
                             || linkFading
#endif
    )) {
        drawMenu();
        if (lgfx::millis() - dotAt >= 300) dotFading = false;
#if CONFIG_IDF_TARGET_ESP32S3
        if (lgfx::millis() - linkAt >= 300) linkFading = false;
#endif
    }
}

// Self-test channel:
//   p<hex>  hold buttons   s  send the screen as RGB bytes
//   m<0|1>  mute speaker   f<hz>  use a sine wave instead of the microphone (0 = off)
void sendScreenshot() {
    static uint8_t row[320 * 3];
    flockfile(stdout);
    flockfile(stderr);
    puts("SHOT 320 240");
    for (int y = 0; y < 240; ++y) {
        M5.Display.readRectRGB(0, y, 320, 1, row);
        fwrite(row, 1, sizeof(row), stdout);
        vTaskDelay(1);
    }
    puts("\nENDSHOT");
    fflush(stdout);
    funlockfile(stderr);
    funlockfile(stdout);
}

void handleSerial() {
#if CONFIG_IDF_TARGET_ESP32S3
    static char line[1 + sizeof(network_settings::Credentials::ssid) + sizeof(network_settings::Credentials::password) + 1];
#else
    static char line[10];
#endif
    static size_t len = 0;
    static bool overflow = false;
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    timeval timeout = {0, 0};
    while (select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout) > 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;
        ++ten_serial_bytes;
        if (c == '\r') continue;
        if (c != '\n') {
            if (len + 1 < sizeof(line) && !overflow) line[len++] = c;
            else overflow = true;
            continue;
        }
        if (overflow) {
#if CONFIG_IDF_TARGET_ESP32S3
            network_settings::wipe(line, sizeof(line));
#else
            memset(line, 0, sizeof(line));
#endif
            len = 0;
            overflow = false;
            puts("入力が長すぎます");
            continue;
        }
        line[len] = 0;
        len = 0;
#if CONFIG_IDF_TARGET_ESP32S3
        network_serial_command(line);
#endif
        if (line[0] == 'p') { ++ten_serial_pad; pad_injected = strtoul(line + 1, nullptr, 16); }
        if (line[0] == 's') sendScreenshot();
#if CONFIG_IDF_TARGET_ESP32S3
        if (line[0] == 'j') {
            char error[80];
            if (!photo_storage::send_latest(error, sizeof(error))) printf("JPEG error: %s\n", error);
        }
        if (line[0] == 'J') {  // J<index>: sends one saved photo (0 = newest) for backup
            char error[80], name[16];
            unsigned total = 0;
            uint8_t* data = nullptr;
            size_t size = 0;
            if (!photo_storage::read_index(strtoul(line + 1, nullptr, 10), &total, name, sizeof(name),
                                           &data, &size, error, sizeof(error))) {
                printf("PHOTO error %u %s\n", total, error);
            } else {
                flockfile(stdout);
                flockfile(stderr);
                printf("PHOTO %s %u %u\n", name, total, unsigned(size));
                for (size_t sent = 0; sent < size;) {
                    const size_t count = fwrite(data + sent, 1, std::min<size_t>(4096, size - sent), stdout);
                    if (!count) break;
                    sent += count;
                }
                fflush(stdout);
                funlockfile(stderr);
                funlockfile(stdout);
            }
            free(data);
        }
#endif
        if (line[0] == 'f') test_tone_hz = atoi(line + 1);
        if (line[0] == 'l') test_mic_left = line[1] == '1';
        if (line[0] == 'x' && prefs) {  // clears only the ten-second records left by a self test
            for (const char* key : {"t10_show", "t10_hide3", "t10_blind"}) nvs_erase_key(prefs, key);
            nvs_commit(prefs);
            puts("GAMES ten_records=cleared");
        }
        if (line[0] == 'r') {  // clears scores left behind by a self test
            if (prefs) { nvs_erase_all(prefs); nvs_commit(prefs); }
            if (!running) menuEnter();
        }
        if (line[0] == 't' && sscanf(line + 1, "%d,%d", &injected_x, &injected_y) == 2) tap_injected = true;
        if (line[0] == 'm') {
            test_mute = line[1] == '1';
            M5.Speaker.setVolume(test_mute ? 0 : 200);
        }
#if CONFIG_IDF_TARGET_ESP32S3
        network_settings::wipe(line, sizeof(line));
#else
        memset(line, 0, sizeof(line));
#endif
    }
}
}  // namespace

uint8_t pad_now = 0, pad_just = 0;
M5Canvas uiFrame(&M5.Display);
M5Canvas uiGlyph(&uiFrame);
bool pad_exit     = false;
bool request_menu = false;
bool tap_just     = false;
int tap_x = 0, tap_y = 0;
int64_t input_time_us = 0;
TenTiming ten_timing_max = {};
int ten_serial_bytes = 0, ten_serial_pad = 0, ten_max_serial_bytes = 0, ten_max_serial_pad = 0;
int64_t ten_clean_gap = 0;
TenDetail ten_detail = {};
int64_t ten_draw_us = 0, ten_transfer_us = 0;
int test_tone_hz = 0;
bool test_mute   = false;
bool test_mic_left = false;

int bestScore(const char* key) {
    int32_t score = 0;
    if (prefs) nvs_get_i32(prefs, key, &score);
    return score;
}

bool submitScore(const char* key, int score) {
    if (score <= bestScore(key)) return false;
    return prefs && nvs_set_i32(prefs, key, score) == ESP_OK && nvs_commit(prefs) == ESP_OK;
}

bool tenBest(const char* key, int32_t* value) {
    return prefs && nvs_get_i32(prefs, key, value) == ESP_OK;
}

bool submitTenBest(const char* key, int32_t value) {
    int32_t old;
    const esp_err_t read = prefs ? nvs_get_i32(prefs, key, &old) : ESP_FAIL;
    if (read == ESP_OK && value >= old) return false;
    if (read != ESP_OK && read != ESP_ERR_NVS_NOT_FOUND) { puts("TEN score read failed"); return false; }
    if (nvs_set_i32(prefs, key, value) != ESP_OK || nvs_commit(prefs) != ESP_OK) { puts("TEN score save failed"); return false; }
    return true;
}

void text(lgfx::LovyanGFX& d, const char* s, int x, int y, uint16_t color, const lgfx::IFont* font, textdatum_t datum,
          float scale) {
    d.setFont(font);
    d.setTextSize(scale);
    d.setTextDatum(datum);
    d.setTextColor(color);
    d.drawString(s, x, y);
    d.setTextSize(1);
}

void uiRadialBackground(uint16_t center, uint16_t edge) {
    static uint32_t horizontal[320], vertical[240];
    static uint16_t palette[256];
    static uint16_t oldCenter, oldEdge;
    static bool geometryReady = false, paletteReady = false;
    if (!geometryReady) {
        for (int x = 0; x < 320; ++x) horizontal[x] = (x - 160) * (x - 160) * 9;
        for (int y = 0; y < 240; ++y) vertical[y] = (y - 120) * (y - 120) * 16;
        geometryReady = true;
    }
    if (!paletteReady || center != oldCenter || edge != oldEdge) {
        for (int i = 0; i < 256; ++i) {
            const float t = sqrtf(float(i) / 255.0f);
            const auto mix = [t](int a, int b) { return int(a + (b - a) * t + 0.5f); };
            palette[i] = (mix((center >> 11) & 31, (edge >> 11) & 31) << 11) |
                         (mix((center >> 5) & 63, (edge >> 5) & 63) << 5) |
                         mix(center & 31, edge & 31);
        }
        oldCenter = center;
        oldEdge = edge;
        paletteReady = true;
    }
    auto* pixels = static_cast<uint8_t*>(uiFrame.getBuffer());
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            const uint32_t index = min(uint32_t(255), (horizontal[x] + vertical[y]) * 255 / 460800);
            const uint16_t color = palette[index];
            const int offset = 2 * (y * 320 + x);
            pixels[offset] = color >> 8;
            pixels[offset + 1] = color & 0xFF;
        }
    }
}

void uiGradientCard(int x, int y, int w, int h, int radius, uint16_t top, uint16_t bottom) {
    const uint16_t edge = (((((top >> 11) & 31) + 31) / 2) << 11) |
                          (((((top >> 5) & 63) + 63) / 2) << 5) | (((top & 31) + 31) / 2);
    uiFrame.fillSmoothRoundRect(x, y, w, h, radius, edge);
    for (int row = 2; row < h - 2; ++row) {
        int inset = 2;
        const int distance = row < radius ? radius - row : row >= h - radius ? row - (h - radius - 1) : 0;
        if (distance) inset = max(2, radius - int(sqrtf(float(radius * radius - distance * distance))) + 1);
        const auto mix = [row, h](int a, int b) { return (a * (h - 1 - row) + b * row) / (h - 1); };
        const uint16_t color = (mix((top >> 11) & 31, (bottom >> 11) & 31) << 11) |
                               (mix((top >> 5) & 63, (bottom >> 5) & 63) << 5) |
                               mix(top & 31, bottom & 31);
        uiFrame.drawFastHLine(x + inset, y + row, w - 2 * inset, color);
    }
}


void darkenRoundRect(M5Canvas& target, int x, int y, int w, int h, int radius) {
    static uint8_t dimTable[32][64];
    static bool dimTableReady = false;
    if (!dimTableReady) {
        for (int opacity = 0; opacity < 32; ++opacity)
            for (int component = 0; component < 64; ++component)
                dimTable[opacity][component] = (component * (62 - opacity) + 31) / 62;
        dimTableReady = true;
    }
    struct MaskCache { int w, h, radius; uint8_t* data; bool attempted; };
    constexpr size_t kMaskCacheLimit = 3;
    static MaskCache masks[kMaskCacheLimit] = {{140, 34, 17, nullptr, false},
        {158, 34, 17, nullptr, false}, {308, 140, 16, nullptr, false}};
    static bool maskAllocationFailureLogged = false;
    uint8_t* cachedMask = nullptr;
    for (auto& entry : masks) {
        if (entry.w != w || entry.h != h || entry.radius != radius) continue;
        if (!entry.attempted) {
            entry.attempted = true;
            entry.data = static_cast<uint8_t*>(heap_caps_malloc(w * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!entry.data) {
                if (!maskAllocationFailureLogged) {
                    puts("GAMES ui_mask=FAIL; drawing without cache");
                    maskAllocationFailureLogged = true;
                }
                break;
            }
            for (int ty = 0; ty < h; ty += 48) {
                for (int tx = 0; tx < w; tx += 192) {
                    uiGlyph.fillSprite(TFT_BLACK);
                    uiGlyph.fillSmoothRoundRect(-tx, -ty, w, h, radius, TFT_WHITE);
                    const auto* source = static_cast<const uint8_t*>(uiGlyph.getBuffer());
                    for (int row = 0; row < min(48, h - ty); ++row)
                        for (int col = 0; col < min(192, w - tx); ++col)
                            entry.data[(ty + row) * w + tx + col] = source[2 * (row * 192 + col)] >> 3;
                }
            }
        }
        cachedMask = entry.data;
        break;
    }
    auto* pixels = static_cast<uint8_t*>(target.getBuffer());
    for (int tileY = 0; tileY < h; tileY += 48) {
        for (int tileX = 0; tileX < w; tileX += 192) {
            const int left = max(0, x + tileX), right = min(320, min(x + w, x + tileX + 192));
            const int top = max(0, y + tileY), bottom = min(240, min(y + h, y + tileY + 48));
            if (left >= right || top >= bottom) continue;
            if (!radius) {
                for (int py = top; py < bottom; ++py) {
                    int px = left;
                    for (; px + 1 < right; px += 2) {
                        uint32_t pair;
                        auto* dest = pixels + 2 * (py * 320 + px);
                        memcpy(&pair, dest, sizeof(pair));
                        pair = ((pair & 0xdef6def6U) >> 1) | ((pair & 0x00010001U) << 15);
                        memcpy(dest, &pair, sizeof(pair));
                    }
                    if (px < right) {
                        auto* dest = pixels + 2 * (py * 320 + px);
                        uint16_t color = (uint16_t(dest[0]) << 8) | dest[1];
                        color = (color & 0xf7deU) >> 1;
                        dest[0] = color >> 8;
                        dest[1] = color & 0xff;
                    }
                }
                continue;
            }
            const uint8_t* mask = nullptr;
            if (radius && !cachedMask) {
                uiGlyph.fillSprite(TFT_BLACK);
                uiGlyph.fillSmoothRoundRect(-tileX, -tileY, w, h, radius, TFT_WHITE);
                mask = static_cast<const uint8_t*>(uiGlyph.getBuffer());
            }
            for (int py = top; py < bottom; ++py) {
                for (int px = left; px < right; ++px) {
                    const int offset = 2 * (py * 320 + px);
                    uint16_t color = (uint16_t(pixels[offset]) << 8) | pixels[offset + 1];
                    if (mask || cachedMask) {
                        const int maskOffset = 2 * ((py - y - tileY) * 192 + px - x - tileX);
                        const int opacity = cachedMask ? cachedMask[(py - y) * w + px - x] : mask[maskOffset] >> 3;
                        if (!opacity) continue;
                        const uint8_t* dim = dimTable[opacity];
                        color = (dim[(color >> 11) & 31] << 11) | (dim[(color >> 5) & 63] << 5) | dim[color & 31];
                    } else {
                        color = (color & 0xF7DE) >> 1;
                    }
                    pixels[offset] = color >> 8;
                    pixels[offset + 1] = color & 0xFF;
                }
            }
        }
    }
}

void darkenRoundRect(int x, int y, int w, int h, int radius) {
    darkenRoundRect(uiFrame, x, y, w, h, radius);
}

void dialog(const char* title, uint16_t titleColor, const char* line1, const char* line2, const char* line3) {
    (void)titleColor;
    (void)line3;
    auto& d = uiFrame;
    darkenRoundRect(0, 0, 320, 240, 0);
    if (strcmp(title, "一時停止") == 0) {
        d.fillSmoothRoundRect(30, 34, 260, 172, 20, pal::kAccent);
        d.fillSmoothRoundRect(32, 36, 256, 168, 18, rgb(20, 24, 52));
        ui_icon::pause(d, 160, 76, 40, pal::kAccent);
        text(d, title, 160, 103, pal::kText, F32, top_center);
        text(d, "START つづける", 160, 146, pal::kText, F24, top_center);
        text(d, "SELECT メニュー", 160, 176, pal::kDim, F24, top_center);
    } else {
        uiGradientCard(24, 16, 272, 208, 20, rgb(255, 92, 138), rgb(122, 31, 82));
        text(d, title, 160, 24, pal::kText, F32, top_center);
        if (line1) {
            const char* score = line1;
            while (*score && (*score < '0' || *score > '9')) ++score;
            char digits[16] = {};
            size_t n = 0;
            while (score[n] >= '0' && score[n] <= '9' && n + 1 < sizeof(digits)) {
                digits[n] = score[n];
                ++n;
            }
            if (uiGlyph.textWidth(digits, F40) > 160) {
                while (n && uiGlyph.textWidth(digits, F40) + uiGlyph.textWidth("…", F40) > 160) digits[--n] = 0;
                strlcat(digits, "…", sizeof(digits));
            }
            uiGlyph.fillSprite(TFT_BLACK);
            text(uiGlyph, digits, 96, 24, pal::kText, F40, middle_center);
            uiGlyph.pushRotateZoomWithAA(&d, 160, 94, 0, 1.4f, 1.4f, TFT_BLACK);
        }
        if (line2) {
            d.fillSmoothRoundRect(86, 130, 148, 32, 16, pal::kAccent);
            text(d, "NEW BEST!", 160, 134, rgb(58, 21, 0), F24, top_center);
        }
        text(d, "A・START もう一回", 160, 166, pal::kText, F24, top_center);
        text(d, "SELECT メニュー", 160, 196, pal::kDim, F24, top_center);
    }
    d.pushSprite(0, 0);
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
#if CONFIG_IDF_TARGET_ESP32S3
    usb_serial_jtag_driver_config_t serial = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&serial));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
#else
    constexpr uart_port_t console = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    ESP_ERROR_CHECK(uart_driver_install(console, 2048, 2048, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_set_baudrate(console, 460800));
    uart_vfs_dev_use_driver(console);
    uart_vfs_dev_port_set_tx_line_endings(console, ESP_LINE_ENDINGS_LF);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("GAMES reset_reason=%d\n", int(esp_reset_reason()));
    M5.Display.setBrightness(180);
    M5.Display.fillScreen(pal::kBg);
    uiFrame.setColorDepth(16);
    uiFrame.setPsram(true);
    void* uiBuffer = uiFrame.createSprite(320, 240);
    if (!uiBuffer || !esp_ptr_external_ram(uiBuffer)) {
        uiFrame.deleteSprite();
        text(M5.Display, "画面メモリ不足", 160, 100, pal::kRed, F32, top_center);
        puts("GAMES ui_frame=FAIL");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    uiGlyph.setColorDepth(16);
    uiGlyph.setPsram(true);
    void* glyphBuffer = uiGlyph.createSprite(192, 48);
    if (!glyphBuffer || !esp_ptr_external_ram(glyphBuffer)) {
        uiGlyph.deleteSprite();
        text(M5.Display, "画面メモリ不足", 160, 100, pal::kRed, F32, top_center);
        puts("GAMES ui_glyph=FAIL");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    uiGlyph.setPivot(96, 24);
    puts("GAMES ui_frame=OK");
#if CONFIG_IDF_TARGET_ESP32S3
    if (M5.getBoard() == m5::board_t::board_M5StackCoreS3 ||
        M5.getBoard() == m5::board_t::board_M5StackCoreS3SE) {
        esp_err_t led_result = arcade_led::begin();
        if (led_result != ESP_OK) printf("LED init=%s\n", esp_err_to_name(led_result));
    }
#endif
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_OK) nvs = nvs_open("arcade", NVS_READWRITE, &prefs);
    if (nvs != ESP_OK) printf("GAMES nvs=%s\n", esp_err_to_name(nvs));
    if (gamepad.begin(&M5.In_I2C, 0x08, 100000) != M5FACES_OK) {
        uiFrame.fillScreen(pal::kBg);
        text(uiFrame, "ゲームパッドなし", 160, 100, pal::kRed, F32, top_center);
        uiFrame.pushSprite(0, 0);
        puts("GAMES gamepad=FAIL");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    puts("GAMES gamepad=OK");
    // The Core2 IMU (0x68) and PDM mic share the small board in the stock bottom module.
    printf("GAMES imu_0x68=%d imu_enabled=%d board=%d\n", M5.In_I2C.scanID(0x68), M5.Imu.isEnabled(), int(M5.getBoard()));
    logMemory("startup");
#if CONFIG_IDF_TARGET_ESP32S3
    if (!rubyServiceBegin()) puts("Ruby の待受を準備できません");
#endif
    menuEnter();
}

void loop() {
    static int64_t previousInput = 0, previousEnd = 0, previousSound = 0, previousTick = 0, largestGap = 0;
    static int previousSerialBytes = 0, previousSerialPad = 0;
    const int64_t loopStart = esp_timer_get_time();
    ten_serial_bytes = ten_serial_pad = 0;
    handleSerial();
    const int64_t serialEnd = esp_timer_get_time();
    padRead();
    const int64_t padEnd = esp_timer_get_time();
    if (running == &kTenSeconds && tenTimingActive() && previousInput) {
        const int64_t gap = input_time_us - previousInput;
        if (!previousSerialBytes && !ten_serial_bytes) ten_clean_gap = max(ten_clean_gap, gap);
        if (gap > largestGap) {
            ten_max_serial_bytes = previousSerialBytes + ten_serial_bytes;
            ten_max_serial_pad = previousSerialPad + ten_serial_pad;
            largestGap = gap;
            ten_timing_max = {serialEnd - loopStart, input_time_us - serialEnd, previousSound,
                              ten_draw_us, ten_transfer_us,
                              previousTick - ten_draw_us - ten_transfer_us, loopStart - previousEnd,
                              ten_detail.led, ten_detail.state, ten_detail.rest, ten_detail.ticks, ten_detail.entry, ten_detail.copy};
        }
    } else { largestGap = 0; ten_clean_gap = 0; }
    previousSerialBytes = ten_serial_bytes; previousSerialPad = ten_serial_pad;
    previousInput = input_time_us;
    soundTick();
    const int64_t soundEndAt = esp_timer_get_time();
#if CONFIG_IDF_TARGET_ESP32S3
    if (running != &kRuby && rubyServiceProgramReady()) {
        leaveCurrentGame();
        sliding = false; pendingAction = NONE;
        cursor = kGameCount - 1;
        openCursor();
    } else
#endif
    if (!running) {
        menuTick();
    } else if (pad_exit || request_menu) {
        leaveCurrentGame();
        logMemory("returned_to_menu");
        menuEnter();
    } else {
        running->tick();
    }
    previousSound = soundEndAt - padEnd;
    previousTick = esp_timer_get_time() - soundEndAt;
    previousEnd = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(4));
}

extern "C" void app_main() {
    setup();
    while (true) loop();
}
