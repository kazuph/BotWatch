// Non-blocking note sequencer on the Core2 speaker.
#include "common.h"

namespace {
struct Event {
    uint32_t at;
    uint32_t duration;
    int notes[3];
};
const int kMaxEvents = 8;
Event events[kMaxEvents];
int eventCount = 0;
uint8_t sineWave[64];
bool waveReady = false;

float midiHz(int midi) { return 440.0f * powf(2.0f, (midi - 69) / 12.0f); }
}  // namespace

void soundBegin() {
    if (!waveReady) {
        for (int i = 0; i < 64; ++i) sineWave[i] = 128 + int(100.0f * sinf(i * 2.0f * PI / 64.0f));
        waveReady = true;
    }
    M5.Mic.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(test_mute ? 0 : 200);
    eventCount = 0;
}

void soundEnd() {
    soundStop();
    M5.Speaker.end();
}

void soundStop() {
    ttsCancel();
    eventCount = 0;
    M5.Speaker.stop();
}

void soundPlay(uint32_t delayMs, uint32_t durationMs, int note1, int note2, int note3) {
    if (eventCount >= kMaxEvents) return;
    events[eventCount++] = {lgfx::millis() + delayMs, durationMs, {note1, note2, note3}};
}

void soundTick() {
    ttsTick();
    const uint32_t now = lgfx::millis();
    for (int i = 0; i < eventCount;) {
        if (int32_t(now - events[i].at) < 0) {
            ++i;
            continue;
        }
        for (int ch = 0; ch < 3; ++ch) {
            if (events[i].notes[ch] < 0) continue;
            M5.Speaker.tone(midiHz(events[i].notes[ch]), events[i].duration, ch, true, sineWave, sizeof(sineWave));
        }
        events[i] = events[--eventCount];
    }
}
