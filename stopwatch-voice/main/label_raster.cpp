#include "label_raster.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include <limits>

namespace label_raster {
namespace {
struct Glyph { uint32_t code; int width, height, advance, top, left; size_t offset; };
#include "print_font.inc"

const Glyph *find(uint32_t code)
{
    for (const auto &glyph : kPrintGlyphs) { if (glyph.code == code) { return &glyph; } }
    return nullptr;
}
}

bool render(const std::string &text, Label &label, std::string &error, int start_padding)
{
    if (text.empty()) { error = "empty_recognition"; return false; }
    std::vector<const Glyph *> glyphs;
    int cursor = 0, min_x = 0, max_x = 0;
    int min_y = std::numeric_limits<int>::max(), max_y = std::numeric_limits<int>::min();
    for (size_t offset = 0; offset < text.size();) {
        const uint8_t lead = text[offset++];
        uint32_t code = lead;
        const int count = lead < 0x80 ? 0 : lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
        if (count) { code &= (1u << (6 - count)) - 1; }
        for (int i = 0; i < count; ++i) {
            if (offset >= text.size() || (uint8_t(text[offset]) & 0xc0) != 0x80) { error = "unsupported_utf8"; return false; }
            code = (code << 6) | (uint8_t(text[offset++]) & 0x3f);
        }
        // A space (between a preset's family and given names) only advances.
        if (code == ' ') { glyphs.push_back(nullptr); cursor += kSpaceDots; continue; }
        const auto *glyph = find(code);
        if (!glyph) { error = "unsupported_glyph"; return false; }
        glyphs.push_back(glyph);
        min_x = std::min(min_x, cursor + glyph->left);
        max_x = std::max(max_x, cursor + glyph->left + glyph->width);
        min_y = std::min(min_y, -glyph->top);
        max_y = std::max(max_y, glyph->height - glyph->top);
        cursor += glyph->advance;
    }
    if (max_y - min_y > kWidth || kPrintAscent + kPrintDescent > kWidth) { error = "font_exceeds_head"; return false; }
    label.rows = kLeadRows + start_padding + max_x - min_x + kEndPadding;
    label.bits.reset(static_cast<uint8_t *>(heap_caps_calloc(label.bytes(), 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!label.bits) { error = "label_allocation"; return false; }
    const int baseline = (kWidth - (max_y - min_y)) / 2 - min_y;
    cursor = kLeadRows + start_padding - min_x;
    for (const auto *glyph : glyphs) {
        if (!glyph) { cursor += kSpaceDots; continue; }
        const int stride = (glyph->width + 7) / 8;
        for (int y = 0; y < glyph->height; ++y) {
            for (int x = 0; x < glyph->width; ++x) {
                if (!(kPrintBits[glyph->offset + y * stride + x / 8] & (0x80 >> (x % 8)))) { continue; }
                // Clockwise rotation: horizontal text runs along the tape.
                const int px = kWidth - 1 - (baseline - glyph->top + y);
                const int py = cursor + glyph->left + x;
                if (px < 0 || px >= kWidth || py < kLeadRows || py >= label.rows) { error = "glyph_outside_raster"; return false; }
                label.bits.get()[py * kStride + px / 8] |= 0x80 >> (px % 8);
            }
        }
        cursor += glyph->advance;
    }
    return true;
}

const char *style_name(int style)
{
    static const char *names[] = {"白ひら", "白カナ", "枠ひら", "枠カナ", "反転ひら", "反転カナ"};
    return style >= 0 && style < kStyles ? names[style] : "";
}

bool render_style(const std::string &text, int style, Label &label, std::string &error)
{
    if (style < 0 || style >= kStyles) { error = "unsupported_style"; return false; }
    std::string converted;
    for (size_t offset = 0; offset < text.size();) {
        const uint8_t lead = text[offset++];
        uint32_t code = lead;
        const int count = lead < 0x80 ? 0 : lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
        if (count) { code &= (1u << (6 - count)) - 1; }
        for (int i = 0; i < count; ++i) {
            if (offset >= text.size() || (uint8_t(text[offset]) & 0xc0) != 0x80) { error = "unsupported_utf8"; return false; }
            code = (code << 6) | (uint8_t(text[offset++]) & 0x3f);
        }
        if ((style & 1) && code >= 0x3041 && code <= 0x3096) { code += 0x60; }
        if (code < 0x80) { converted += char(code); }
        else if (code < 0x800) { converted += char(0xc0 | (code >> 6)); converted += char(0x80 | (code & 63)); }
        else { converted += char(0xe0 | (code >> 12)); converted += char(0x80 | ((code >> 6) & 63)); converted += char(0x80 | (code & 63)); }
    }
    return render_variant(converted, style / 2, label, error);
}

bool render_variant(const std::string &text, int frame, Label &label, std::string &error)
{
    if (frame < 0 || frame > 2) { error = "unsupported_frame"; return false; }
    if (!render(text, label, error, frame == 1 ? kFramedStartPadding : kEndPadding)) { return false; }
    if (frame == 2) {
        for (size_t i = kLeadRows * kStride; i < label.bytes(); ++i) { label.bits.get()[i] ^= 0xff; }
    } else if (frame == 1) {
        // Existing labelprinter.render uses a two-dot border.
        constexpr int border = 2;
        for (int y = kLeadRows; y < label.rows; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                if (x < border || x >= kWidth - border || y < kLeadRows + border || y >= label.rows - border) {
                    label.bits.get()[y * kStride + x / 8] |= 0x80 >> (x % 8);
                }
            }
        }
    }
    return true;
}

std::string all_characters()
{
    std::string text;
    for (const auto &glyph : kPrintGlyphs) {
        const uint32_t code = glyph.code;
        text += char(0xe0 | (code >> 12));
        text += char(0x80 | ((code >> 6) & 0x3f));
        text += char(0x80 | (code & 0x3f));
    }
    return text;
}
}
