// Shared input, palette, text helpers, sound and scores for FACES ARCADE.
// All on-screen text uses the F24/F32/F40 Japanese fonts: 24 px is the minimum size.
#pragma once
#include <M5Unified.h>
#include <M5Faces.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include "arcade_led.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "esp_random.h"
using std::min;
using std::max;
constexpr float PI = 3.14159265358979323846f;
inline long arcadeRandom(long upper) { return upper > 0 ? esp_random() % upper : 0; }
inline long arcadeRandom(long lower, long upper) { return lower + arcadeRandom(upper - lower); }

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

#define F24 (&fonts::lgfxJapanGothicP_24)
#define F32 (&fonts::lgfxJapanGothicP_32)
#define F40 (&fonts::lgfxJapanGothicP_40)

namespace pal {
constexpr uint16_t kBg      = rgb(14, 18, 38);
constexpr uint16_t kPanel   = rgb(28, 34, 66);
constexpr uint16_t kLine    = rgb(70, 82, 140);
constexpr uint16_t kText    = rgb(238, 240, 250);
constexpr uint16_t kDim     = rgb(170, 178, 214);
constexpr uint16_t kAccent  = rgb(255, 208, 70);
constexpr uint16_t kCyan    = rgb(70, 220, 240);
constexpr uint16_t kRed     = rgb(240, 70, 80);
constexpr uint16_t kGreen   = rgb(90, 220, 110);
}  // namespace pal

enum : uint8_t {
    kUp = GAMEPAD3_BTN_UP, kDown = GAMEPAD3_BTN_DOWN, kLeft = GAMEPAD3_BTN_LEFT, kRight = GAMEPAD3_BTN_RIGHT,
    kA = GAMEPAD3_BTN_A, kB = GAMEPAD3_BTN_B, kSelect = GAMEPAD3_BTN_SELECT, kStart = GAMEPAD3_BTN_START,
};

extern uint8_t pad_now;    // buttons held
extern uint8_t pad_just;   // buttons newly pressed this read
extern bool pad_exit;      // SELECT pressed: leave the current app
extern bool request_menu;  // set by an app to return to the menu
extern bool tap_just;      // the screen was tapped this read
extern int tap_x, tap_y;
extern int64_t input_time_us;
struct TenTiming { int64_t serial, pad, sound, draw, transfer, other, wait, led, state, rest, ticks, entry, copy; };
struct TenDetail { int64_t led, state, rest, ticks, entry, copy; };
extern TenDetail ten_detail;
extern int ten_serial_bytes, ten_serial_pad, ten_max_serial_bytes, ten_max_serial_pad;
extern int64_t ten_clean_gap;
extern TenTiming ten_timing_max;
extern int64_t ten_draw_us, ten_transfer_us;
bool tenTimingActive();
extern int test_tone_hz;   // self test: replaces microphone input with a sine wave when non-zero
extern bool test_mute;     // self test: keeps the speaker silent
extern bool test_mic_left; // self test: PDM channel used by the microphone
extern M5Canvas uiFrame;  // Shared 320 x 240 UI frame allocated once in PSRAM.
extern M5Canvas uiGlyph;  // Small sprite for antialiased enlarged text.
void darkenRoundRect(int x, int y, int w, int h, int radius);
void darkenRoundRect(M5Canvas& target, int x, int y, int w, int h, int radius);
void uiGradientCard(int x, int y, int w, int h, int radius, uint16_t top, uint16_t bottom);
void uiRadialBackground(uint16_t center, uint16_t edge);

int bestScore(const char* key);
bool submitScore(const char* key, int score);  // true when it is a new best
bool tenBest(const char* key, int32_t* value);
bool submitTenBest(const char* key, int32_t value);

void text(lgfx::LovyanGFX& d, const char* s, int x, int y, uint16_t color,
          const lgfx::IFont* font = F24, textdatum_t datum = top_left, float scale = 1.0f);

// Centered dialog used for pause and game over.
void dialog(const char* title, uint16_t titleColor, const char* line1, const char* line2 = nullptr,
            const char* line3 = nullptr);

// Speaker. soundBegin() must run before notes; the microphone and speaker share one I2S port.
void soundBegin();
void soundEnd();
void soundStop();
// Schedules up to 3 simultaneous MIDI notes `delayMs` from now. Call soundTick() every loop.
void soundPlay(uint32_t delayMs, uint32_t durationMs, int note1, int note2 = -1, int note3 = -1);
void soundTick();

// sanoTTS-jp: speaks hiragana. ttsSay blocks while a clip is synthesized the first time.
bool ttsBegin();
bool ttsSay(const char* kana);
void ttsTick();
void ttsCancel();

enum class GameCategory { learn, listen, play, view };
enum class GameFooter { score, sound, photo, tenBest };
struct Game {
    const char* name;
    const char* scoreKey;  // nullptr: no score
    void (*enter)();
    void (*tick)();
    void (*icon)(int x, int y);
    void (*leave)();  // may be nullptr
    GameCategory category;
    GameFooter footer;
};
extern const Game kAbsolutePitchQuiz;
extern const Game kNoteQuiz;
extern const Game kChordQuiz;
extern const Game kKanaQuiz;
extern const Game kSpectrum;
extern const Game kBlockDrop;
extern const Game kSnake;
extern const Game kFlappy;
extern const Game kTenSeconds;
#if CONFIG_IDF_TARGET_ESP32S3
extern const Game kCamera;
extern const Game kPhotoViewer;
#endif
