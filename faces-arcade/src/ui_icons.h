// Small UI icons drawn with antialiased shapes. Each function draws into `g` with its
// top-left corner at (x, y) unless noted. Sizes match the approved UI design (ui-design.html v2).
#pragma once
#include <M5GFX.h>

namespace ui_icon {

// 32x16 battery outline with a fill bar for `percent` (0..100). Green above 20%, amber below.
void battery(lgfx::LovyanGFX& g, int x, int y, int percent);

// 12x12 status dot: green when the tailnet link is up, grey otherwise.
void link(lgfx::LovyanGFX& g, int cx, int cy, bool up);

// Camera body; `w` is the body width (height is w * 5 / 6). Lens uses `accent`.
void camera(lgfx::LovyanGFX& g, int x, int y, int w, uint16_t body, uint16_t lens, uint16_t accent);

// 22x22 mode badges for the camera name tag, centred at (cx, cy).
void face(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color);      // outlined smiley
void question(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color);  // filled circle with "?"
void kana(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color);      // rounded square with "あ"
void bubble(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color);    // speech bubble with three dots
void photo(lgfx::LovyanGFX& g, int cx, int cy, uint16_t color);     // picture frame with a sun

// Status marks.
void check(lgfx::LovyanGFX& g, int cx, int cy, int r, uint16_t disc, uint16_t mark);  // disc with a tick
void warning(lgfx::LovyanGFX& g, int cx, int top, int size, uint16_t fill, uint16_t mark);  // triangle with "!"
void pause(lgfx::LovyanGFX& g, int cx, int cy, int h, uint16_t color);  // two rounded bars, centred
void star(lgfx::LovyanGFX& g, int cx, int cy, int r, uint16_t color);   // five-point star

// Gamepad button marks for hint bars. Letters use the 24 px font.
void buttonA(lgfx::LovyanGFX& g, int cx, int cy);
void buttonB(lgfx::LovyanGFX& g, int cx, int cy);
void buttonStart(lgfx::LovyanGFX& g, int x, int y);  // 32x24 pill with a play triangle
void arrowsUpDown(lgfx::LovyanGFX& g, int x, int y, uint16_t color);  // 44x16 pair of triangles

}  // namespace ui_icon
