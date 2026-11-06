#include "text_viewer.h"
#include "common.h"
#include <algorithm>
#include <string>
#include <vector>

namespace text_viewer {
namespace {
std::vector<std::string> lines;
size_t top = 0;
constexpr int kRowHeight = 24;
constexpr int kTop = 32;
constexpr int kRows = (240 - kTop - kRowHeight) / kRowHeight;

void draw() {
    auto& d = M5.Display;
    d.fillScreen(pal::kBg);
    text(d, "上下:スクロール", 160, 2, pal::kAccent, F24, top_center);
    text(d, "B:再送 A:もどる", 160, 216, pal::kDim, F24, top_center);
    for (size_t i = 0; i < kRows && top + i < lines.size(); ++i)
        text(d, lines[top + i].c_str(), 0, kTop + i * kRowHeight, pal::kText, F24);
}
}
void show(const std::string& value) {
    lines.clear();
    top = 0;
    std::string line;
    for (size_t i = 0; i < value.size();) {
        const unsigned char c = value[i];
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
            ++i;
            continue;
        }
        if (c == '\r') { ++i; continue; }
        size_t count = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        count = std::min(count, value.size() - i);
        const std::string character = value.substr(i, count);
        const std::string candidate = line + character;
        if (!line.empty() && M5.Display.textWidth(candidate.c_str(), F24) > 320) {
            lines.push_back(line);
            line.clear();
        }
        line += character;
        i += count;
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    draw();
}
void tick() {
    const size_t old = top;
    if ((pad_just & kUp) && top > 0) --top;
    if ((pad_just & kDown) && top + kRows < lines.size()) ++top;
    if (old != top) draw();
}
void clear() { lines.clear(); top = 0; }
}
