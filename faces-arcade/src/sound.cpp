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
    Timbre timbre;
    float volume;
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

void renderBell(int16_t* out, size_t n, const int* notes);
void renderBuzz(int16_t* out, size_t n, const int* notes);

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
int countVoices(const int* notes) {
    int v = 0;
    for (int i = 0; i < 3; ++i) v += notes[i] >= 0;
    return v ? v : 1;
}

// Two-operator FM bell (modulator at 3.5x the carrier, the classic tubular-bell ratio).
// The modulation index falls quickly, so the strike is bright and the tail turns into a clean tone.
void renderBell(int16_t* out, size_t n, const int* notes) {
    const int voices = countVoices(notes);
    const float gain = 15000.0f / voices;  // two overlapping bell notes stay under the 16-bit limit
    const float radToPhase = 4294967296.0f / (2.0f * PI);
    const float ampDecay = expf(-1.0f / (kRate * 0.45f));
    const float indexDecay = expf(-1.0f / (kRate * 0.08f));
    const size_t attack = kRate * 2 / 1000, release = kRate * 30 / 1000;
    for (int v = 0, k = 0; v < 3; ++v) {
        if (notes[v] < 0) continue;
        const float hz = midiHz(notes[v]);
        const uint32_t stepC = uint32_t(hz / kRate * 4294967296.0f);
        const uint32_t stepM = uint32_t(hz * 3.5f / kRate * 4294967296.0f);
        uint32_t pc = 0, pm = 0;
        float amp = 1.0f, index = 2.2f;
        for (size_t i = 0; i < n; ++i) {
            const float mod = index * sineTable[pm >> (32 - kTableBits)];
            float s = sineTable[uint32_t(pc + int32_t(mod * radToPhase)) >> (32 - kTableBits)] * amp;
            if (i < attack) s *= float(i) / attack;
            if (n - i < release) s *= float(n - i) / release;
            const int32_t mixed = (k ? out[i] : 0) + int32_t(s * gain);
            out[i] = int16_t(mixed);
            pc += stepC; pm += stepM;
            amp *= ampDecay; index *= indexDecay;
        }
        ++k;
    }
}

// Band-limited square wave (odd harmonics 1, 3, 5, 7) with a flat sustain: a buzzer.
// Two notes a semitone apart beat against each other, which adds the "rough" alarm quality.
void renderBuzz(int16_t* out, size_t n, const int* notes) {
    const int voices = countVoices(notes);
    const float w[4] = {1.0f, 1.0f / 3, 1.0f / 5, 1.0f / 7};
    const float gain = 30000.0f / (voices * (w[0] + w[1] + w[2] + w[3]));  // the semitone pair partly cancels, so it needs more gain
    const size_t attack = kRate * 4 / 1000, release = kRate * 25 / 1000;
    for (size_t i = 0; i < n; ++i) out[i] = 0;
    for (int v = 0; v < 3; ++v) {
        if (notes[v] < 0) continue;
        const uint32_t step = uint32_t(midiHz(notes[v]) / kRate * 4294967296.0f);
        uint32_t p = 0;
        for (size_t i = 0; i < n; ++i) {
            float s = 0;
            for (int h = 0; h < 4; ++h) s += w[h] * sineTable[uint32_t(p * uint32_t(2 * h + 1)) >> (32 - kTableBits)];
            if (i < attack) s *= float(i) / attack;
            if (n - i < release) s *= float(n - i) / release;
            out[i] = int16_t(out[i] + int32_t(s * gain));
            p += step;
        }
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
    M5.Speaker.setVolume(soundSpeakerVolume());
    eventCount = 0;
}

uint8_t soundSpeakerVolume() {
    static const uint8_t kLevels[4] = {0, 60, 120, 200};  // 200 was the only volume before
    return test_mute ? 0 : kLevels[sound_level < 0 ? 0 : sound_level > 3 ? 3 : sound_level];
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
    events[eventCount++] = {lgfx::millis() + delayMs, durationMs, {note1, note2, note3}, Timbre::piano, 1.0f};
}

void soundPlayVolume(float volume, uint32_t delayMs, uint32_t durationMs, int note1, int note2, int note3) {
    if (eventCount >= kMaxEvents) return;
    events[eventCount++] = {lgfx::millis() + delayMs, durationMs, {note1, note2, note3}, Timbre::piano, volume};
}

void soundPlayTimbre(Timbre timbre, uint32_t delayMs, uint32_t durationMs, int note1, int note2, int note3) {
    if (eventCount >= kMaxEvents) return;
    events[eventCount++] = {lgfx::millis() + delayMs, durationMs, {note1, note2, note3}, timbre, 1.0f};
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
            if (e.timbre == Timbre::bell) renderBell(slotBuf[slot], n, e.notes);
            else if (e.timbre == Timbre::buzz) renderBuzz(slotBuf[slot], n, e.notes);
            else render(slotBuf[slot], n, e.notes);
            if (e.volume < 1.0f)
                for (size_t k = 0; k < n; ++k) slotBuf[slot][k] = int16_t(slotBuf[slot][k] * e.volume);
            slotStarted[slot] = now;
            M5.Speaker.playRaw(slotBuf[slot], n, kRate, false, 1, slot + 1, true);
        } else {
            for (int ch = 0; ch < 3; ++ch)
                if (e.notes[ch] >= 0)
                    M5.Speaker.tone(midiHz(e.notes[ch]), e.duration, ch + 1, true, fallbackWave, sizeof(fallbackWave));
        }
    }
}
