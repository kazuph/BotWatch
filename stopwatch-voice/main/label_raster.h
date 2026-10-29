#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "esp_heap_caps.h"

namespace label_raster {
constexpr int kWidth = 96;  // Measured print head, docs/PROTOCOL.md.
constexpr int kStride = kWidth / 8;
constexpr int kLeadRows = 16;
constexpr int kTailRows = 100;
struct Label {
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> bits{nullptr, heap_caps_free};
    int rows = 0;
    size_t bytes() const { return size_t(rows) * kStride; }
    double paper_mm() const { return (rows + kTailRows) / 8.0; }
};
constexpr int kEndPadding = 3 * 8;  // Existing labelprinter.render pad_mm=3, 8 dots/mm.
// The framed label's start gap looked nearly closed on paper (human report), so it gets 1.5 mm more.
constexpr int kFramedStartPadding = kEndPadding + 12;
constexpr int kSpaceDots = 24;  // 3 mm between a preset's family and given names.
bool render(const std::string &text, Label &label, std::string &error, int start_padding = kEndPadding);
constexpr int kStyles = 6;
bool render_style(const std::string &text, int style, Label &label, std::string &error);
// frame: 0 plain, 1 framed, 2 inverted; the text is printed as given (kanji included).
bool render_variant(const std::string &text, int frame, Label &label, std::string &error);
const char *style_name(int style);
std::string all_characters();
}
