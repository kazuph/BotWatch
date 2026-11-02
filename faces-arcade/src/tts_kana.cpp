// Plays kana voices rendered ahead of time by tools/gen_kana.c with sanoTTS-jp,
// the same engine and weights as stopwatch-voice. Synthesis on this ESP32 takes about
// 4 seconds per kana, so the clips ship in flash and play with no wait.
// Each kana is spoken twice with a short pause between.
#include "common.h"
#include "esp_heap_caps.h"
#include "kana_voice.h"

extern const uint8_t voice_start[] asm("_binary_kana_voice_bin_start");

namespace {
int16_t* playBuf = nullptr;  // flash data has no alignment guarantee; playback uses this copy
const size_t kMaxSamples = 22050 * 2;
const uint32_t kPauseMs  = 350;
size_t clipSamples = 0;
bool secondPending = false;
uint32_t secondAt  = 0;

bool playClip() {
    const bool ok = M5.Speaker.playRaw(playBuf, clipSamples, KANA_VOICE_RATE, false, 1, 0, true);
    Serial.printf("TTS play samples=%u ok=%d\n", unsigned(clipSamples), ok);
    return ok;
}
}  // namespace

bool ttsBegin() {
    if (!playBuf) playBuf = static_cast<int16_t*>(heap_caps_malloc(kMaxSamples * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return playBuf != nullptr;
}

void ttsCancel() { secondPending = false; }

void ttsTick() {
    if (secondPending && int32_t(millis() - secondAt) >= 0) {
        secondPending = false;
        playClip();
    }
}

bool ttsSay(const char* kana) {
    if (!playBuf) return false;
    for (const auto& v : kKanaVoice) {
        if (strcmp(v.kana, kana)) continue;
        if (v.samples > kMaxSamples) return false;
        soundStop();
        memcpy(playBuf, voice_start + v.offset * 2, v.samples * 2);
        clipSamples = v.samples;
        Serial.printf("TTS say=%s\n", kana);
        if (!playClip()) return false;
        secondAt      = millis() + v.samples * 1000 / KANA_VOICE_RATE + kPauseMs;
        secondPending = true;
        return true;
    }
    Serial.printf("TTS missing kana=%s\n", kana);
    return false;
}
