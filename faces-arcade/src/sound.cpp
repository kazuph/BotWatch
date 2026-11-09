// Non-blocking note sequencer on the speaker.
// Each scheduled event is rendered once into a 16-bit PCM buffer and played with playRaw:
// a smooth sine table (no aliasing from a short wavetable), a few soft harmonics for an
// electric-piano like tone, a short attack and release (no clicks), and headroom so a
// three-note chord never clips. Channel 0 is left to the kana voice (tts_kana.cpp).
#include "common.h"
#include "esp_heap_caps.h"

namespace {
struct Event {
    uint32_t at;
    uint32_t duration;
    int notes[3];
};
const int kMaxEvents = 8;
Event events[kMaxEvents];
int eventCount = 0;

const uint32_t kRate = 24000;          // Hz; the mixer resamples to the speaker rate
const uint32_t kMaxMs = 2000;          // longest note we render
const size_t kMaxSamples = kRate * kMaxMs / 1000;
const int kSlots = 6;                  // speaker channels 1..6
int16_t* slotBuf[kSlots] = {};
uint32_t slotStarted[kSlots] = {};
bool buffersReady = false;

const int kTableBits = 10;             // 1024-point sine table
float sineTable[1 << kTableBits];
// Harmonic weights: warm fundamental, a little 2nd and 3rd, like a soft electric piano.
const float kHarm[3] = {1.0f, 0.35f, 0.12f};

uint8_t fallbackWave[64];               // used only if the PSRAM buffers could not be allocated

float midiHz(int midi) { return 440.0f * powf(2.0f, (midi - 69) / 12.0f); }

int pickSlot() {
    int oldest = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (!M5.Speaker.isPlaying(i + 1)) return i;
        if (int32_t(slotStarted[i] - slotStarted[oldest]) < 0) oldest = i;
    }
    M5.Speaker.stop(oldest + 1);
    return oldest;
}

void render(int16_t* out, size_t n, const int* notes) {
    int voices = 0;
    uint32_t step[3][3] = {};
    for (int v = 0; v < 3; ++v) {
        if (notes[v] < 0) continue;
        const float hz = midiHz(notes[v]);
        for (int h = 0; h < 3; ++h)  // phase step in 32-bit fixed point
            step[voices][h] = uint32_t((hz * (h + 1) / kRate) * 4294967296.0f);
        ++voices;
    }
    float harmSum = 0;
    for (float w : kHarm) harmSum += w;
    const float gain = 24000.0f / (harmSum * (voices ? voices : 1));
    const size_t attack = kRate * 6 / 1000;     // 6 ms
    const size_t release = kRate * 40 / 1000;   // 40 ms fade at the end
    const float decay = expf(-1.0f / (kRate * 0.9f));  // gentle piano-like decay
    uint32_t phase[3][3] = {};
    float env = 1.0f;
    for (size_t i = 0; i < n; ++i) {
        float s = 0;
        for (int v = 0; v < voices; ++v)
            for (int h = 0; h < 3; ++h) {
                s += kHarm[h] * sineTable[phase[v][h] >> (32 - kTableBits)];
                phase[v][h] += step[v][h];
            }
        float shape = env;
        if (i < attack) shape *= float(i) / attack;
        if (n - i < release) shape *= float(n - i) / release;
        out[i] = int16_t(s * gain * shape);
        env *= decay;
    }
}
}  // namespace

void soundBegin() {
    if (!buffersReady) {
        for (int i = 0; i < (1 << kTableBits); ++i) sineTable[i] = sinf(i * 2.0f * PI / (1 << kTableBits));
        for (int i = 0; i < 64; ++i) fallbackWave[i] = 128 + int(100.0f * sinf(i * 2.0f * PI / 64.0f));
        bool ok = true;
        for (int i = 0; i < kSlots; ++i) {
            slotBuf[i] = static_cast<int16_t*>(heap_caps_malloc(kMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            ok &= slotBuf[i] != nullptr;
        }
        if (!ok) puts("SOUND pcm buffers unavailable, using simple tones");
        buffersReady = true;
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
        const Event e = events[i];
        events[i] = events[--eventCount];
        const int slot = pickSlot();
        if (slotBuf[slot]) {
            const size_t n = kRate * min(e.duration, kMaxMs) / 1000;
            render(slotBuf[slot], n, e.notes);
            slotStarted[slot] = now;
            M5.Speaker.playRaw(slotBuf[slot], n, kRate, false, 1, slot + 1, true);
        } else {
            for (int ch = 0; ch < 3; ++ch)
                if (e.notes[ch] >= 0)
                    M5.Speaker.tone(midiHz(e.notes[ch]), e.duration, ch + 1, true, fallbackWave, sizeof(fallbackWave));
        }
    }
}
