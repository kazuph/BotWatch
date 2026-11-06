// Icons for the FACES ARCADE UI. Drawn by the parent (Claude) so the implementation agent does not
// hand-draw visual assets. Only antialiased primitives are used so edges stay clean on the 320x240 panel.
#include "ui_icons.h"
#include <cmath>

namespace ui_icon {
namespace {
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t kGreen = rgb(91, 227, 138);
constexpr uint16_t kAmber = rgb(255, 181, 71);
constexpr uint16_t kGrey  = rgb(142, 151, 199);
constexpr uint16_t kText  = rgb(233, 236, 255);
constexpr uint16_t kInk   = rgb(17, 17, 17);
constexpr uint16_t kPink  = rgb(255, 92, 138);
constexpr uint16_t kYellow = rgb(255, 208, 70);

void letter(lgfx::LovyanGFX& g, const char* s, int cx, int cy, uint16_t color) {
    g.setFont(&fonts::lgfxJapanGothicP_24);
    g.setTextSize(1);
    g.setTextDatum(middle_center);
    g.setTextColor(color);
    g.drawString(s, cx, cy + 1);
}
}  // namespace

void battery(lgfx::LovyanGFX& g, int x, int y, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g.fillSmoothRoundRect(x, y, 30, 16, 4, kText);
    g.fillSmoothRoundRect(x + 2, y + 2, 26, 12, 3, rgb(7, 9, 18));
    g.fillSmoothRoundRect(x + 30, y + 4, 3, 8, 1, kText);
    const int w = 22 * percent / 100;
    if (w > 0) g.fillSmoothRoundRect(x + 4, y + 4, w, 8, 2, percent > 20 ? kGreen : kAmber);
}

void link(lgfx::LovyanGFX& g, int cx, int cy, bool up) {
    g.fillSmoothCircle(cx, cy, 6, up ? kGreen : kGrey);
}

void camera(lgfx::LovyanGFX& g, int x, int y, int w, uint16_t body, uint16_t lens, uint16_t accent) {
    const int h = w * 5 / 6;
    const int top = h / 6;
    g.fillSmoothRoundRect(x + w * 3 / 10, y, w * 4 / 10, top + 4, 4, body);  // prism
    g.fillSmoothRoundRect(x, y + top, w, h - top, w / 7, body);
    const int cx = x + w / 2, cy = y + top + (h - top) / 2;
    const int r = (h - top) * 7 / 20;
    g.fillSmoothCircle(cx, cy, r, lens);
    g.fillSmoothCircle(cx, cy, r * 2 / 3, accent);
    g.fillSmoothCircle(cx - r / 4, cy - r / 4, r / 4 > 1 ? r / 4 : 1, body);
    g.fillSmoothCircle(x + w * 17 / 20, y + top + (h - top) / 5, w / 24 > 1 ? w / 24 : 1, kPink);
}

void face(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color) {
    g.fillSmoothCircle(cx, cy, 10, color);
    g.fillSmoothCircle(cx, cy, 8, rgb(0, 0, 0));
    g.fillSmoothCircle(cx - 3, cy - 2, 2, color);
    g.fillSmoothCircle(cx + 3, cy - 2, 2, color);
    g.drawWideLine(cx - 4, cy + 3, cx, cy + 5, 2.0f, color);
    g.drawWideLine(cx, cy + 5, cx + 4, cy + 3, 2.0f, color);
}

void question(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color) {
    g.fillSmoothCircle(cx, cy, 11, color);
    letter(g, "?", cx, cy, kText);
}

void kana(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color) {
    g.fillSmoothRoundRect(cx - 11, cy - 11, 22, 22, 5, color);
    letter(g, "あ", cx, cy, kInk);
}

void bubble(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color) {
    g.fillSmoothRoundRect(cx - 11, cy - 9, 22, 15, 5, color);
    g.fillTriangle(cx - 6, cy + 4, cx - 1, cy + 4, cx - 7, cy + 10, color);
    for (int i = -1; i <= 1; ++i) g.fillSmoothCircle(cx + i * 6, cy - 2, 2, kText);
}

void photo(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color) {
    g.fillSmoothRoundRect(cx - 11, cy - 9, 22, 18, 3, color);
    g.fillSmoothRoundRect(cx - 9, cy - 7, 18, 14, 2, rgb(0, 0, 0));
    g.fillSmoothCircle(cx + 4, cy - 2, 3, color);
    g.fillTriangle(cx - 9, cy + 7, cx - 3, cy - 1, cx + 3, cy + 7, color);
}

void check(lgfx::LovyanGFX& g, int cx, int cy, int r, uint16_t disc, uint16_t mark) {
    g.fillSmoothCircle(cx, cy, r, disc);
    const float s = r / 15.0f;
    const float w = 3.5f * s > 2.0f ? 3.5f * s : 2.0f;
    g.drawWideLine(cx - 7 * s, cy, cx - 2 * s, cy + 5 * s, w, mark);
    g.drawWideLine(cx - 2 * s, cy + 5 * s, cx + 7 * s, cy - 5 * s, w, mark);
}

void warning(lgfx::LovyanGFX& g, int cx, int top, int size, uint16_t fill, uint16_t mark) {
    const int half = size / 2 + size / 10;
    g.fillTriangle(cx, top, cx - half, top + size, cx + half, top + size, fill);
    // Soften the three corners.
    g.fillSmoothCircle(cx, top + 2, 2, fill);
    g.fillSmoothRoundRect(cx - size / 18 - 1, top + size * 3 / 10, size / 9 + 2, size * 2 / 5, 2, mark);
    g.fillSmoothCircle(cx, top + size * 4 / 5, size / 14 + 1, mark);
}

void pause(lgfx::LovyanGFX& g, int cx, int cy, int h, uint16_t color) {
    const int w = h * 3 / 10;
    const int gap = h * 4 / 10;
    g.fillSmoothRoundRect(cx - gap / 2 - w, cy - h / 2, w, h, w / 3, color);
    g.fillSmoothRoundRect(cx + gap / 2, cy - h / 2, w, h, w / 3, color);
}

void star(lgfx::LovyanGFX& g, int cx, int cy, int r, uint16_t color) {
    float px[10], py[10];
    for (int i = 0; i < 10; ++i) {
        const float a  = -M_PI / 2 + i * M_PI / 5;
        const float rr = (i % 2) ? r * 0.42f : r;
        px[i] = cx + rr * cosf(a);
        py[i] = cy + rr * sinf(a);
    }
    for (int i = 0; i < 10; ++i) {
        const int j = (i + 1) % 10;
        g.fillTriangle(cx, cy, px[i], py[i], px[j], py[j], color);
    }
}

void buttonA(lgfx::LovyanGFX& g, int cx, int cy) {
    g.fillSmoothCircle(cx, cy, 14, kPink);
    letter(g, "A", cx, cy, kText);
}

void buttonB(lgfx::LovyanGFX& g, int cx, int cy) {
    g.fillSmoothCircle(cx, cy, 14, kYellow);
    letter(g, "B", cx, cy, kInk);
}

void buttonStart(lgfx::LovyanGFX& g, int x, int y) {
    g.fillSmoothRoundRect(x, y, 32, 24, 12, kGrey);
    g.fillTriangle(x + 11, y + 5, x + 23, y + 12, x + 11, y + 19, kInk);
}

void arrowsUpDown(lgfx::LovyanGFX& g, int x, int y, uint16_t color) {
    g.fillTriangle(x, y + 16, x + 9, y, x + 18, y + 16, color);
    g.fillTriangle(x + 24, y, x + 42, y, x + 33, y + 16, color);
}

}  // namespace ui_icon
