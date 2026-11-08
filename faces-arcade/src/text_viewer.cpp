#include "text_viewer.h"
#include "common.h"
#include "ui_icons.h"
#include <algorithm>
#include <string>
#include <vector>

namespace text_viewer {
namespace {
std::vector<std::string> lines;
size_t top = 0;
constexpr int kRowHeight = 34;
constexpr int kTop = 51;
constexpr int kRows = 4;
constexpr int kTextWidth = 272;
network_job::Kind shownKind = network_job::Kind::ocr;

void draw() {
    auto& d = uiFrame;
    const uint16_t accent = shownKind == network_job::Kind::ocr ? pal::kAccent : rgb(155, 123, 255);
    d.fillScreen(rgb(7, 9, 18));
    d.fillSmoothRoundRect(6, 6, 140, 34, 17, accent);
    d.fillSmoothRoundRect(7, 7, 138, 32, 16, rgb(14, 18, 38));
    if (shownKind == network_job::Kind::ocr) ui_icon::kana(d, 26, 23, accent);
    else ui_icon::bubble(d, 26, 23, accent);
    text(d, shownKind == network_job::Kind::ocr ? "文字よみ" : "せつめい", 42, 10, pal::kText, F24);
    d.fillSmoothRoundRect(246, 6, 68, 34, 17, accent);
    char count[24];
    snprintf(count, sizeof(count), "%u行", unsigned(lines.size()));
    if (d.textWidth(count, F24) > 60) strlcpy(count, "…", sizeof(count));
    text(d, count, 280, 10, rgb(20, 24, 52), F24, top_center);
    d.fillSmoothRoundRect(6, 46, 300, 152, 12, rgb(42, 48, 96));
    d.fillSmoothRoundRect(8, 48, 296, 148, 10, rgb(20, 24, 52));
    for (size_t i = 0; i < kRows && top + i < lines.size(); ++i)
        text(d, lines[top + i].c_str(), 18, kTop + i * kRowHeight, pal::kText, F24);
    d.fillSmoothRoundRect(310, 48, 6, 148, 3, rgb(38, 44, 77));
    const int scrollHeight = lines.size() <= kRows ? 148 : max(6, int(148 * kRows / lines.size()));
    const int scrollY = lines.size() <= kRows ? 48 : 48 + int((148 - scrollHeight) * top / (lines.size() - kRows));
    d.fillSmoothRoundRect(310, scrollY, 6, scrollHeight, 3, pal::kDim);
    darkenRoundRect(0, 202, 320, 38, 0);
    ui_icon::arrowsUpDown(d, 10, 212, pal::kText);
    ui_icon::buttonA(d, 80, 221);
    text(d, "カメラ", 98, 207, pal::kText, F24);
    ui_icon::buttonB(d, 186, 221);
    text(d, "送り直す", 204, 207, pal::kText, F24);
    d.pushSprite(0, 0);
}
}
void show(const std::string& value, network_job::Kind kind) {
    shownKind = kind;
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
        if (!line.empty() && uiFrame.textWidth(candidate.c_str(), F24) > kTextWidth) {
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
