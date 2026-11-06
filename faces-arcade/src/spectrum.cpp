// Shows microphone input as frequency: loudest pitch in Hz, its note, and log-spaced bars.
#include "common.h"
#include "ui_icons.h"

namespace {
const int kN = 1024, kRate = 16000;
const int kBars = 32, kBarsH = 112;
const int kSlots = 3;
const float kMinHz = 80, kMaxHz = 8000;

int16_t* rec;           // kSlots * kN samples
int slot = 0;
float re[kN], im[kN], win[kN], smooth[kBars];
M5Canvas bars(&M5.Display);
bool showBands = false;
float needleCents = 0;
uint16_t badgeFrom, badgeTo;
uint32_t badgeAt;
float testPhase = 0;
uint32_t lastLog = 0;
int flatBuffers = 0;
bool noMicShown = false;


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

uint16_t badgeColor(uint16_t target, uint32_t now) {
    const uint32_t elapsed = min(now - badgeAt, uint32_t(300));
    const auto mix = [elapsed](int a, int b) { return (a * (300 - elapsed) + b * elapsed) / 300; };
    const uint16_t current = (mix((badgeFrom >> 11) & 31, (badgeTo >> 11) & 31) << 11) |
                             (mix((badgeFrom >> 5) & 63, (badgeTo >> 5) & 63) << 5) |
                             mix(badgeFrom & 31, badgeTo & 31);
    if (target != badgeTo) {
        badgeFrom = current;
        badgeTo = target;
        badgeAt = now;
    }
    return current;
}

void drawReadout(float hz, bool loud) {
    auto& d = uiFrame;
    if (showBands) d.fillScreen(rgb(7, 9, 18));
    else uiRadialBackground(rgb(15, 42, 34), rgb(7, 9, 18));
    d.fillSmoothRoundRect(6, 6, 134, 32, 16, rgb(30, 75, 53));
    text(d, showBands ? "周波数の帯" : "チューナー", 73, 10, pal::kGreen, F24, top_center);
    ui_icon::buttonB(d, 22, showBands ? 58 : 130);
    text(d, showBands ? "チューナー" : "帯", 42, showBands ? 46 : 118, pal::kText, F24);
    static const char* const names[12] = {"ド", "ド#", "レ", "レ#", "ミ", "ファ", "ファ#", "ソ", "ソ#", "ラ", "ラ#", "シ"};
    int nearest = 0, cents = 0;
    char line[32];
    if (loud) {
        const float midi = 69 + 12 * log2f(hz / 440.0f);
        nearest = int(lroundf(midi));
        cents = int(lroundf((midi - nearest) * 100));
        const char* name = names[(nearest % 12 + 12) % 12];
        if (showBands) snprintf(line, sizeof(line), "%s %dHz", name, int(hz + 0.5f));
        else snprintf(line, sizeof(line), "%dHz", int(hz + 0.5f));
        text(d, line, 314, 8, pal::kText, F24, top_right);
        if (!showBands) {
            snprintf(line, sizeof(line), "%+d", cents);
            d.fillSmoothRoundRect(246, 42, 68, 30, 15,
                                  badgeColor(abs(cents) <= 10 ? pal::kGreen : pal::kDim, lgfx::millis()));
            text(d, line, 280, 45, rgb(6, 33, 15), F24, top_center);
            const int noteWidth = int(uiGlyph.textWidth(name, F40) * 1.8f + 0.5f);
            const int noteCenter = min(152, 226 - noteWidth / 2);
            uiGlyph.fillSprite(TFT_BLACK);
            text(uiGlyph, name, 96, 24, pal::kText, F40, middle_center);
            uiGlyph.pushRotateZoomWithAA(&d, noteCenter, 88, 0, 1.8f, 1.8f, TFT_BLACK);
            snprintf(line, sizeof(line), "%d", nearest / 12 - 1);
            text(d, line, noteCenter + noteWidth / 2 + 2, 84, pal::kDim, F32);
        }
    } else if (!showBands) {
        text(d, "声や音を", 160, 46, pal::kDim, F32, top_center);
        text(d, "聞かせてね", 160, 82, pal::kDim, F32, top_center);
    }
    if (showBands) {
        for (int b = 0; b < kBars; ++b) {
            const int h = int(smooth[b] * 116);
            for (int y = 0; y < h; ++y) {
                const float f = float(y) / 116;
                d.drawFastHLine(10 + b * 304 / kBars, 199 - y, 7, rgb(31 + int(48 * f), 138 + int(71 * f), 106 + int(149 * f)));
            }
        }
        text(d, "80", 8, 207, pal::kDim, F24);
        text(d, "800", 160, 207, pal::kDim, F24, top_center);
        text(d, "8k", 314, 207, pal::kDim, F24, top_right);
    } else {
        needleCents += (cents - needleCents) * 0.2f;
        d.fillArc(160, 236, 108, 92, 180, 360, rgb(29, 42, 64));
        d.fillArc(160, 236, 108, 92, 252, 288, pal::kGreen);
        d.fillSmoothCircle(129, 141, 8, pal::kGreen);
        d.fillSmoothCircle(191, 141, 8, pal::kGreen);
        const float angle = (180 + (needleCents + 50) * 1.8f) * PI / 180;
        d.drawLine(160, 236, 160 + int(90 * cosf(angle)), 236 + int(90 * sinf(angle)), pal::kText);
        d.fillSmoothCircle(160, 236, 9, pal::kText);
        text(d, "-50", 6, 211, pal::kDim, F24);
        text(d, "+50", 314, 211, pal::kDim, F24, top_right);
    }
    d.pushSprite(0, 0);
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
            uiFrame.fillScreen(pal::kBg);
            text(uiFrame, "マイクなし", 160, 30, pal::kRed, F40, top_center);
            text(uiFrame, "マイクは純正の底に", 160, 96, pal::kText, F24, top_center);
            text(uiFrame, "入っています", 160, 126, pal::kText, F24, top_center);
            text(uiFrame, "SELECT:もどる", 160, 186, pal::kDim, F24, top_center);
            uiFrame.pushSprite(0, 0);
            printf("MIC flat min=%d max=%d\n", lo, hi);
        }
        return;
    }
    if (noMicShown) noMicShown = false;
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
#if CONFIG_IDF_TARGET_ESP32S3
    if (!loud) arcade_led::clear();
    else {
        const float midi = 69 + 12 * log2f(hz / 440.0f);
        const int cents = int(lroundf((midi - lroundf(midi)) * 100));
        arcade_led::green_level(static_cast<uint8_t>(64 * (50 - std::min(50, abs(cents))) / 50));
    }
#endif

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
    }
    drawReadout(hz, loud);

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
    showBands = false;
    needleCents = 0;
    badgeFrom = badgeTo = pal::kDim;
    badgeAt = lgfx::millis();
    M5.Speaker.end();
    auto cfg            = M5.Mic.config();
    cfg.sample_rate     = kRate;
    cfg.noise_filter_level = 0;
    if (M5.getBoard() == m5::board_t::board_M5StackCore2) cfg.left_channel = test_mic_left;
    M5.Mic.config(cfg);
    const bool ok = rec && M5.Mic.begin();
    uiFrame.fillScreen(pal::kBg);
    printf("MIC begin=%d\n", ok);
    if (!ok) {
        text(uiFrame, "マイクが使えません", 160, 100, pal::kRed, F32, top_center);
        uiFrame.pushSprite(0, 0);
        return;
    }
    drawReadout(0, false);
    slot = 0;
}

void tick() {
    if (pad_just & kB) showBands = !showBands;
    if (!M5.Mic.isEnabled()) return;
    if (M5.Mic.isRecording() >= 2) return;
    if (!M5.Mic.record(&rec[slot * kN], kN, kRate)) return;
    // The two newer slots are still queued, so the next slot holds the oldest finished buffer.
    slot = (slot + 1) % kSlots;
    analyze(&rec[slot * kN]);
}

void leave() { M5.Mic.end(); }

void icon(int x, int y) {
    auto& d = uiGlyph;
    d.fillRoundRect(x, y, 40, 40, 6, pal::kBg);
    static const int8_t hs[6] = {10, 24, 32, 18, 26, 8};
    for (int i = 0; i < 6; ++i) d.fillRect(x + 3 + i * 6, y + 37 - hs[i], 4, hs[i], i == 2 ? pal::kAccent : pal::kCyan);
}
}  // namespace

const Game kSpectrum = {"周波数メーター", nullptr, enter, tick, icon, leave};
