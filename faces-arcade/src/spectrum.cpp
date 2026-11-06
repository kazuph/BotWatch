// Shows microphone input as frequency: loudest pitch in Hz, its note, and log-spaced bars.
#include "common.h"

namespace {
const int kN = 1024, kRate = 16000;
const int kBars = 32, kBarsTop = 96, kBarsH = 112;
const int kSlots = 3;
const float kMinHz = 80, kMaxHz = 8000;

int16_t* rec;           // kSlots * kN samples
int slot = 0;
float re[kN], im[kN], win[kN], smooth[kBars];
M5Canvas bars(&M5.Display);
int shownHz = -1;
float testPhase = 0;
uint32_t lastLog = 0;
int flatBuffers = 0;
bool noMicShown = false;
void drawAxis();

void fft() {
    for (int i = 1, j = 0; i < kN; ++i) {
        int bit = kN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= kN; len <<= 1) {
        const float ang = -2.0f * PI / len;
        const float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < kN; i += len) {
            float cr = 1, ci = 0;
            for (int j = 0; j < len / 2; ++j) {
                const int a = i + j, b = i + j + len / 2;
                const float xr = re[b] * cr - im[b] * ci;
                const float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                const float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

void drawReadout(float hz, bool loud) {
    auto& d = M5.Display;
    const int value = loud ? int(hz + 0.5f) : 0;
    if (value == shownHz) return;
    shownHz = value;
    d.fillRect(0, 0, 320, kBarsTop, pal::kBg);
    if (!loud) {
        text(d, "声や音を", 160, 4, pal::kDim, F40, top_center);
        text(d, "聞かせてね", 160, 50, pal::kDim, F40, top_center);
        return;
    }
    char b[24];
    snprintf(b, sizeof(b), "%d", value);
    text(d, b, 8, 2, pal::kAccent, F40, top_left, 2.0f);
    const int digitsW = d.textWidth(b, F40) * 2;
    text(d, "Hz", 8 + digitsW + 4, 44, pal::kDim, F32);

    static const char* const kNames[12] = {"ド", "ド#", "レ", "レ#", "ミ", "ファ", "ファ#", "ソ", "ソ#", "ラ", "ラ#", "シ"};
    const float midi  = 69 + 12 * log2f(hz / 440.0f);
    const int nearest = int(lroundf(midi));
    const int cents   = int(lroundf((midi - nearest) * 100));
    text(d, kNames[(nearest % 12 + 12) % 12], 312, 2, pal::kCyan, F40, top_right);
    snprintf(b, sizeof(b), "%+d", cents);
    text(d, b, 312, 56, abs(cents) <= 10 ? pal::kGreen : pal::kText, F32, top_right);
}

void analyze(const int16_t* samples) {
    float rms = 0;
    for (int i = 0; i < kN; ++i) {
        float s = samples[i];
        if (test_tone_hz) {
            s = 8000.0f * sinf(testPhase);
            testPhase += 2.0f * PI * test_tone_hz / kRate;
        }
        rms += s * s;
        re[i] = s * win[i];
        im[i] = 0;
    }
    rms = sqrtf(rms / kN);
    // A real microphone always varies a little. A flat line means nothing is on the data pin.
    int lo = 32767, hi = -32768;
    for (int i = 0; i < kN; ++i) {
        lo = min<int>(lo, samples[i]);
        hi = max<int>(hi, samples[i]);
    }
    const bool core2 = M5.getBoard() == m5::board_t::board_M5StackCore2;
    flatBuffers = (core2 && !test_tone_hz && hi - lo <= 1) ? flatBuffers + 1 : 0;
    if (flatBuffers >= 8) {
        if (!noMicShown) {
            noMicShown = true;
            M5.Display.fillScreen(pal::kBg);
            text(M5.Display, "マイクなし", 160, 30, pal::kRed, F40, top_center);
            text(M5.Display, "マイクは純正の底に", 160, 96, pal::kText, F24, top_center);
            text(M5.Display, "入っています", 160, 126, pal::kText, F24, top_center);
            text(M5.Display, "SELECT:もどる", 160, 186, pal::kDim, F24, top_center);
            printf("MIC flat min=%d max=%d\n", lo, hi);
        }
        return;
    }
    if (noMicShown) {
        noMicShown = false;
        shownHz    = -1;
        M5.Display.fillScreen(pal::kBg);
        drawAxis();
    }
    fft();
    // Peak search above the room-rumble range.
    const int firstBin = int(kMinHz * kN / kRate);
    int peak           = firstBin;
    float peakMag      = 0;
    static float mag[kN / 2];
    for (int i = 0; i < kN / 2; ++i) {
        mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]);
        if (i >= firstBin && mag[i] > peakMag) {
            peakMag = mag[i];
            peak    = i;
        }
    }
    float binPos = peak;
    if (peak > 0 && peak < kN / 2 - 1) {
        const float l = mag[peak - 1], c = mag[peak], r = mag[peak + 1];
        const float denom = l - 2 * c + r;
        if (denom != 0) binPos += 0.5f * (l - r) / denom;
    }
    const float hz  = binPos * kRate / kN;
    const bool loud = peakMag / kN > 60.0f;
    drawReadout(hz, loud);

    bars.fillSprite(pal::kBg);
    for (int b = 0; b < kBars; ++b) {
        const float f0 = kMinHz * powf(kMaxHz / kMinHz, float(b) / kBars);
        const float f1 = kMinHz * powf(kMaxHz / kMinHz, float(b + 1) / kBars);
        const int i0 = max(1, int(f0 * kN / kRate));
        const int i1 = min(kN / 2 - 1, max(i0, int(f1 * kN / kRate)));
        float m = 0;
        for (int i = i0; i <= i1; ++i) m = max(m, mag[i]);
        // 0 to 1 over a 60 dB range.
        float level = (20.0f * log10f(m / kN + 1.0f)) / 60.0f;
        level       = std::clamp(level, 0.0f, 1.0f);
        smooth[b]   = max(level, smooth[b] * 0.82f);
        const int h = int(smooth[b] * kBarsH);
        const uint16_t c = smooth[b] > 0.75f ? pal::kRed : smooth[b] > 0.5f ? pal::kAccent : pal::kCyan;
        bars.fillRect(b * 10, kBarsH - h, 9, h, c);
    }
    bars.pushSprite(0, kBarsTop);

    if (lgfx::millis() - lastLog > 500) {
        lastLog = lgfx::millis();
        int lo = 32767, hi = -32768;
        for (int i = 0; i < kN; ++i) {
            lo = min<int>(lo, samples[i]);
            hi = max<int>(hi, samples[i]);
        }
        const auto c = M5.Mic.config();
        printf("MIC rms=%.1f min=%d max=%d peakHz=%.1f peakMag=%.1f loud=%d in=%d ws=%d port=%d adc=%d mag=%d os=%d left=%d\n", rms,
                      lo, hi, hz, peakMag / kN, loud, c.pin_data_in, c.pin_ws, int(c.i2s_port), c.use_adc, c.magnification,
                      c.over_sampling, c.left_channel);
    }
}

void drawAxis() {
    auto& d = M5.Display;
    d.fillRect(0, kBarsTop + kBarsH, 320, 240 - kBarsTop - kBarsH, pal::kPanel);
    const auto label = [&](const char* s, float hz, textdatum_t datum) {
        const int x = int(320 * log10f(hz / kMinHz) / log10f(kMaxHz / kMinHz));
        text(d, s, x, kBarsTop + kBarsH + 4, pal::kText, F24, datum);
    };
    label("100", 100, top_left);
    label("1k", 1000, top_center);
    label("8k", kMaxHz, top_right);
}

void enter() {
    if (!rec) {
        rec = static_cast<int16_t*>(heap_caps_malloc(sizeof(int16_t) * kN * kSlots, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        for (int i = 0; i < kN; ++i) win[i] = 0.5f - 0.5f * cosf(2 * PI * i / (kN - 1));
        bars.setColorDepth(16);
        bars.createSprite(320, kBarsH);
    }
    memset(smooth, 0, sizeof(smooth));
    if (rec) memset(rec, 0, sizeof(int16_t) * kN * kSlots);
    flatBuffers = 0;
    noMicShown  = false;
    shownHz = -1;
    M5.Speaker.end();
    auto cfg            = M5.Mic.config();
    cfg.sample_rate     = kRate;
    cfg.noise_filter_level = 0;
    if (M5.getBoard() == m5::board_t::board_M5StackCore2) cfg.left_channel = test_mic_left;
    M5.Mic.config(cfg);
    const bool ok = rec && M5.Mic.begin();
    M5.Display.fillScreen(pal::kBg);
    printf("MIC begin=%d\n", ok);
    if (!ok) {
        text(M5.Display, "マイクが使えません", 160, 100, pal::kRed, F32, top_center);
        return;
    }
    drawAxis();
    slot = 0;
}

void tick() {
    if (!M5.Mic.isEnabled()) return;
    if (M5.Mic.isRecording() >= 2) return;
    if (!M5.Mic.record(&rec[slot * kN], kN, kRate)) return;
    // The two newer slots are still queued, so the next slot holds the oldest finished buffer.
    slot = (slot + 1) % kSlots;
    analyze(&rec[slot * kN]);
}

void leave() { M5.Mic.end(); }

void icon(int x, int y) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    static const int8_t hs[6] = {10, 24, 32, 18, 26, 8};
    for (int i = 0; i < 6; ++i) d.fillRect(x + 3 + i * 6, y + 37 - hs[i], 4, hs[i], i == 2 ? pal::kAccent : pal::kCyan);
}
}  // namespace

const Game kSpectrum = {"周波数メーター", nullptr, enter, tick, icon, leave};
