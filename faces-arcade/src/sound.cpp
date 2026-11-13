// Non-blocking note sequencer on the speaker.
// Each scheduled event is rendered once into a 16-bit PCM buffer and played with playRaw:
// a smooth sine table (no aliasing from a short wavetable), a few soft harmonics for an
// electric-piano like tone, a short attack and release (no clicks), and headroom so a
// three-note chord never clips. Channel 0 is left to the kana voice (tts_kana.cpp).
#include "common.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

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
// Quiz answer effects, rendered once into PSRAM by the first soundBegin().
// "Pin-poon": two vibraphone-like bars falling a major third (E6, then C6 170 ms later). Each bar has
// partials 1, 2, 4 and 10 with their own decays and a 5.5 Hz motor tremolo; a small Schroeder
// reverb (four combs, two allpasses) adds the room tail that the old dry FM bell lacked.
// "Bu-buu": a short and a long buzz of a band-limited saw/square mix at 233 Hz with a detuned twin.
// The tiny speaker cannot play 233 Hz, so the fundamental is high-passed away and the ear rebuilds
// the low pitch from the harmonics; a soft clip brings its loudness level with the chime.
const size_t kCorrectSamples = kRate * 1000 / 1000;  // ends before a quiz asks the next question at 1 s
const size_t kWrongSamples = kRate * 850 / 1000;
int16_t* correctPcm = nullptr;
int16_t* wrongPcm = nullptr;
std::atomic<bool> effectsReady{false};  // set by the render task once both buffers are complete

float sineCycles(float cycles) { return sineTable[int((cycles - floorf(cycles)) * (1 << kTableBits)) & ((1 << kTableBits) - 1)]; }

void addBar(float* out, size_t n, size_t start, float hz) {
    const float ratio[4] = {1, 2, 4, 10}, level[4] = {1.0f, 0.25f, 0.35f, 0.12f}, decay[4] = {0.4f, 0.35f, 0.18f, 0.05f};
    float amp[4], fall[4], phase[4] = {}, step[4];
    for (int p = 0; p < 4; ++p) {
        amp[p] = hz * ratio[p] < kRate * 0.45f ? level[p] : 0;  // drop partials that would alias
        fall[p] = expf(-1.0f / (kRate * decay[p]));
        step[p] = hz * ratio[p] / kRate;
    }
    const size_t attack = kRate * 2 / 1000;
    for (size_t i = start; i < n; ++i) {
        const size_t k = i - start;
        float s = 0;
        for (int p = 0; p < 4; ++p) {
            s += amp[p] * sineCycles(phase[p]);
            phase[p] += step[p];
            if (phase[p] >= 1) phase[p] -= 1;
            amp[p] *= fall[p];
        }
        s *= 1 + 0.10f * sineCycles(5.5f * k / kRate);
        if (k < attack) s *= float(k) / attack;
        out[i] += s;
    }
}

void addReverb(float* x, size_t n, float mix) {
    const int combDelay[4] = {797, 887, 1009, 1097};
    const float combGain[4] = {0.80f, 0.78f, 0.76f, 0.74f};
    const int allDelay[2] = {223, 79};
    float* wet = static_cast<float*>(heap_caps_calloc(n, sizeof(float), MALLOC_CAP_SPIRAM));
    float* line = static_cast<float*>(heap_caps_malloc(1097 * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!wet || !line) {
        puts("SOUND reverb memory unavailable; the chime stays dry");
        free(wet); free(line);
        return;
    }
    for (int c = 0; c < 4; ++c) {
        memset(line, 0, combDelay[c] * sizeof(float));
        for (size_t i = 0, k = 0; i < n; ++i) {
            const float v = line[k];
            wet[i] += v / 4;
            line[k] = x[i] + v * combGain[c];
            if (++k == size_t(combDelay[c])) k = 0;
        }
    }
    for (int a = 0; a < 2; ++a) {
        memset(line, 0, allDelay[a] * sizeof(float));
        for (size_t i = 0, k = 0; i < n; ++i) {
            const float b = line[k], v = wet[i] + b * 0.6f;
            wet[i] = b - v * 0.6f;
            line[k] = v;
            if (++k == size_t(allDelay[a])) k = 0;
        }
    }
    for (size_t i = 0; i < n; ++i) x[i] += mix * wet[i];
    free(wet);
    free(line);
}

void addBuzz(float* out, size_t start, size_t length, float hz) {
    // One period of the band-limited saw/square mix is summed once into a table, then two detuned
    // oscillators read it; summing ~46 harmonics per sample instead took about 3 s on the S3.
    constexpr int kPeriod = 1024;
    static float period[kPeriod];
    static float periodHz = 0;
    if (periodHz != hz) {
        for (int i = 0; i < kPeriod; ++i) {
            float s = 0;
            for (int h = 1; hz * 1.012f * h < kRate * 0.45f; ++h)
                s += sineTable[(i * h) & (kPeriod - 1)] / h * (h % 2 ? 1.0f : 0.55f);
            period[i] = s;
        }
        periodHz = hz;
    }
    const float twin[2] = {1.0f, 1.012f}, level[2] = {1.0f, 0.8f};
    float phase[2] = {}, peak = 0;
    for (size_t k = 0; k < length; ++k) {
        float s = 0;
        for (int o = 0; o < 2; ++o) {
            s += level[o] * period[int(phase[o] * kPeriod) & (kPeriod - 1)];
            phase[o] += hz * twin[o] / kRate;
            if (phase[o] >= 1) phase[o] -= 1;
        }
        out[start + k] = s;
        peak = max(peak, fabsf(s));
    }
    const size_t attack = kRate * 4 / 1000, release = kRate * 30 / 1000;
    for (size_t k = 0; k < length; ++k) {
        float s = tanhf(2.88f * out[start + k] / peak);
        if (k < attack) s *= float(k) / attack;
        if (length - k < release) s *= float(length - k) / release;
        out[start + k] = s;
    }
}

void highPass(float* x, size_t n, float hz) {  // two one-pole stages, 12 dB per octave
    const float a = 1.0f / (1.0f + 2.0f * PI * hz / kRate);
    for (int stage = 0; stage < 2; ++stage) {
        float px = 0, py = 0;
        for (size_t i = 0; i < n; ++i) {
            py = a * (py + x[i] - px);
            px = x[i];
            x[i] = py;
        }
    }
}

void storePcm(const float* x, size_t n, int16_t* out, float peakLevel) {
    float peak = 0;
    for (size_t i = 0; i < n; ++i) peak = max(peak, fabsf(x[i]));
    const float gain = peak > 0 ? peakLevel * 32767.0f / peak : 0;
    for (size_t i = 0; i < n; ++i) out[i] = int16_t(x[i] * gain);
}

void renderEffects() {
    float* work = static_cast<float*>(heap_caps_calloc(kCorrectSamples, sizeof(float), MALLOC_CAP_SPIRAM));
    correctPcm = static_cast<int16_t*>(heap_caps_malloc(kCorrectSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    wrongPcm = static_cast<int16_t*>(heap_caps_malloc(kWrongSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!work || !correctPcm || !wrongPcm) {
        free(work); free(correctPcm); free(wrongPcm);
        correctPcm = wrongPcm = nullptr;
        puts("SOUND effects unavailable");
        return;
    }
    const int64_t started = esp_timer_get_time();
    addBar(work, kCorrectSamples, 0, 1318.5f);                  // E6 "pin"
    addBar(work, kCorrectSamples, kRate * 170 / 1000, 1046.5f);  // C6 "poon"
    addReverb(work, kCorrectSamples, 0.22f);
    const size_t fade = kRate * 250 / 1000;
    for (size_t i = kCorrectSamples - fade; i < kCorrectSamples; ++i) work[i] *= float(kCorrectSamples - i) / fade;
    storePcm(work, kCorrectSamples, correctPcm, 0.85f);

    memset(work, 0, kWrongSamples * sizeof(float));
    addBuzz(work, 0, kRate * 160 / 1000, 233.0f);                         // "bu"
    addBuzz(work, kRate * 230 / 1000, kRate * 580 / 1000, 233.0f);        // "buu"
    highPass(work, kWrongSamples, 500.0f);
    float peak = 0;
    for (size_t i = 0; i < kWrongSamples; ++i) peak = max(peak, fabsf(work[i]));
    for (size_t i = 0; i < kWrongSamples; ++i) work[i] = tanhf(2.2f * work[i] / peak);
    storePcm(work, kWrongSamples, wrongPcm, 0.85f);
    free(work);
    printf("SOUND effects rendered in %lld ms\n", (long long)((esp_timer_get_time() - started) / 1000));
    effectsReady = true;
}

// Rendering takes about 0.6 s, so it runs beside the game loop; answers given before it
// finishes use the older synthesized bell and buzzer.
void renderEffectsTask(void*) {
    renderEffects();
    printf("SOUND effects task stack_left=%u\n", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    vTaskDelete(nullptr);
}

void playEffect(const int16_t* pcm, size_t n) {
    const int slot = pickSlot();
    slotStarted[slot] = lgfx::millis();
    M5.Speaker.playRaw(pcm, n, kRate, false, 1, slot + 1, true);
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
        if (xTaskCreate(renderEffectsTask, "sound_fx", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr) != pdPASS)
            puts("SOUND effects task unavailable");
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

void soundCorrect() {
    if (!effectsReady) {  // still rendering, or PSRAM was short: the older FM bell keeps the answer audible
        soundPlayTimbre(Timbre::bell, 0, 500, 88);
        soundPlayTimbre(Timbre::bell, 160, 800, 84, 79);
        return;
    }
    playEffect(correctPcm, kCorrectSamples);
}

void soundWrong() {
    if (!effectsReady) {
        soundPlayTimbre(Timbre::buzz, 0, 170, 61, 62);
        soundPlayTimbre(Timbre::buzz, 250, 380, 62, 63);
        return;
    }
    playEffect(wrongPcm, kWrongSamples);
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
