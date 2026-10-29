#include "voice_screen.h"
#include "characters.h"
#include "freertos/task.h"

#include <M5Unified.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <limits>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace voice_screen {
namespace {
// Native sizes and positions follow the human-approved mock.html.
constexpr int kScale = 1;
constexpr int kFontPixels = 32;
#include "ui_fonts.inc"
#include "ui_icons.inc"
const uint8_t *noto48_start = ui32_start;
const uint8_t *noto16_start = ui20_start;
M5Canvas canvas(&M5.Display);
M5Canvas hints(&canvas);
struct CircleSpan { uint16_t left,right; };
CircleSpan *circle_spans=nullptr;
struct Token { size_t begin, end; int width, paragraph; bool newline; };
struct Line { int begin, end, next, width, capacity, x, y; };
struct View { std::vector<Line> lines; };
std::vector<Token> tokens;
std::vector<View> views;
std::string source;
int first = 0;
int rows = 0;
int safe_side = 0;
bool actions = false, error_actions = false;
bool animated = false, characters_ready = false;
bool character_background_invalid=true;
Avatar character_avatar;
characters::View character_view;
characters::TouchGesture character_gesture;
constexpr char kCharacterLeft[]="おして話す",kCharacterRight[]="もどる";
constexpr int kArcGlyphSide=40,kArcGlyphCount=(sizeof(kCharacterLeft)+sizeof(kCharacterRight)-2)/3;
struct ArcGlyph { int x,y,w,h;uint16_t color;uint8_t *alpha; };
ArcGlyph arc_glyphs[kArcGlyphCount];
uint8_t *arc_alpha=nullptr;
struct ArcSpan { uint16_t x,y,length,color; };
ArcSpan *arc_spans=nullptr;
size_t arc_span_count=0;
bool prepare_arc_cache();
uint64_t timed_frames=0,render_time_us=0,transfer_time_us=0,other_time_us=0;
uint64_t background_time_us=0,character_time_us=0,arc_time_us=0,ui_clip_time_us=0;
int64_t last_present_us=0;
std::string character_caption, character_kana;
constexpr int kCaptionBaseline=412,kDetailBaseline=448;
constexpr int kCaptionStep=kDetailBaseline-kCaptionBaseline;
struct CaptionRow { int x,y,width,height;float scale; };
CaptionRow character_text[2]{};
StaticTask_t animation_task_storage;
StackType_t animation_stack[4096 / sizeof(StackType_t)];
void draw_avatar();
int region_top = 0, region_height = 0;
const uint8_t *preview_bits = nullptr;
int preview_rows = 0;
double paper_length = 0;
const char *left_role = "", *right_role = "";
StaticSemaphore_t screen_mutex_storage;
SemaphoreHandle_t screen_mutex = nullptr;
struct Lock {
    Lock() { if (screen_mutex) { xSemaphoreTakeRecursive(screen_mutex, portMAX_DELAY); } }
    ~Lock() { if (screen_mutex) { xSemaphoreGiveRecursive(screen_mutex); } }
};
const label_raster::Label *style_labels = nullptr;
const char *const *style_names = nullptr;
const char *list_heading = "";
int style_count = 0, selected_style = 0, list_offset = 0;
std::vector<std::string> result_lines;
void draw_previews();

void paint_preview(const uint8_t *bits, int height, int top, int available_height);

// The sticker is 15mm (120 dots) high and the head prints only a 96-dot band
// in its middle, so a real label shows white paper, then the printed band (and
// the frame, for framed styles), then the text. The raster already begins with
// the 16 white lead rows; the same margin is shown after the text.
constexpr int kPaperDots = 120, kBandMargin = (kPaperDots - 96) / 2, kTrailDots = 16;
int paper_rows(int rows) { return rows + kTrailDots; }

// Draw the sticker at (x, y) with the print raster rotated back to horizontal,
// shrunk by `scale`. A screen pixel takes the ink colour when any source dot in
// its block is ink, so thin strokes and the 2-dot frame survive the reduction.
// Ink is black on a white label and white on an inverted (black) label.
void draw_label(const uint8_t *bits, int rows, double scale, int x, int paper_y)
{
    const int paper_width = std::max(1, int(paper_rows(rows) * scale));
    const int paper_height = std::max(1, int(kPaperDots * scale));
    canvas.fillRoundRect(x, paper_y, paper_width, paper_height, std::max(2, int(6 * scale)), TFT_WHITE);
    canvas.drawRoundRect(x, paper_y, paper_width, paper_height, std::max(2, int(6 * scale)), canvas.color888(0xC9, 0xC0, 0xA8));
    const int y = paper_y + std::lround(kBandMargin * scale);
    const int width = std::max(1, int(std::ceil(rows * scale)));
    const int height = std::max(1, int(std::ceil(96 * scale)));
    auto dot = [&](int row, int col) { return (bits[row * 12 + col / 8] & (0x80 >> (col % 8))) != 0; };
    const bool inverted = dot(rows / 2, 6) && dot(rows / 2, 89);  // inside the 2-dot frame, outside the glyphs
    for (int py = 0; py < height; ++py) {
        const int col_end = 95 - std::min(95, int(py / scale));
        const int col_begin = 95 - std::min(95, std::max(int(py / scale), int(std::ceil((py + 1) / scale)) - 1));
        for (int px = 0; px < width; ++px) {
            const int row_begin = std::min(rows - 1, int(px / scale));
            const int row_end = std::min(rows - 1, std::max(row_begin, int(std::ceil((px + 1) / scale)) - 1));
            bool ink = false;
            for (int row = row_begin; row <= row_end && !ink; ++row) {
                for (int col = col_begin; col <= col_end; ++col) {
                    if (dot(row, col) != inverted) { ink = true; break; }
                }
            }
            canvas.drawPixel(x + px, y + py, ink != inverted ? TFT_BLACK : TFT_WHITE);
        }
    }
}

constexpr uint16_t rgb(unsigned c) { return ((c >> 19) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31); }
constexpr uint16_t kIvory = rgb(0xF6F1E7), kCard = rgb(0xFFFDF8), kOlive = rgb(0x827A04);
constexpr uint16_t kInk = rgb(0x4A4636), kSub = rgb(0x8C8673), kOutline = rgb(0xE8E0CC);
constexpr uint16_t kYellow = rgb(0xF2C14E), kYellowInk = rgb(0xB8901F), kBlue = rgb(0x8EC3EA), kBlueInk = rgb(0x4F8FC0);
constexpr uint16_t kCoral = rgb(0xF08A7E), kCoralTrack = rgb(0xF3DCD5);
int current_size = 32;
// Radius of the curved role words: inside the button arc, and inside the
// recording ring on the recording screen.
double role_text_radius = 186;
bool quiet_warning = false;
const UiFont &font(int size)
{
    for (const auto &item : kUiFonts) { if (item.size == size) { return item; } }
    return kUiFonts[5];
}
void use_font(int size)
{
    if (current_size != size) { canvas.unloadFont(); canvas.loadFont(font(size).data); current_size = size; }
}
int origin() { return (canvas.width() - 466) / 2; }
void text_at(const std::string &text, int size, int x, int baseline, uint16_t color, bool centered = true, uint16_t background_color = kIvory)
{
    use_font(size); canvas.setTextColor(color, background_color);
    canvas.drawString(text.c_str(), x + origin() - (centered ? canvas.textWidth(text.c_str()) / 2 : 0), baseline + origin() - font(size).ascent);
}
void background()
{
    animated = false;
    character_gesture.clear();
    style_labels = nullptr; role_text_radius = 186;
    canvas.clearClipRect(); canvas.fillSprite(TFT_BLACK);
    canvas.fillCircle(canvas.width()/2, canvas.height()/2, canvas.width()/2 - 1, kIvory);
}
void icon(const uint16_t *pixels, int width, int height, int x, int y)
{
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) { canvas.drawPixel(x+col+origin(), y+row+origin(), pixels[row*width+col]); }
    }
}
uint32_t font_word(const uint8_t *p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
bool prepare_role_cache()
{
    arc_alpha=static_cast<uint8_t *>(heap_caps_malloc(kArcGlyphCount*kArcGlyphSide*kArcGlyphSide,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!arc_alpha) return false;
    hints.unloadFont();hints.loadFont(font(20).data);
    const auto *data=font(20).data;
    const unsigned count=font_word(data);
    const float ascent=font_word(data+16),height=hints.fontHeight();
    int cursor=0;
    for(int side=0;side<2;++side) {
        const char *role=side?kCharacterRight:kCharacterLeft;
        std::vector<std::string> glyphs;
        int total=0;
        for(const char *p=role;*p;) {
            const int bytes=uint8_t(*p)<0x80?1:uint8_t(*p)<0xe0?2:3;
            glyphs.emplace_back(p,bytes);total+=hints.textWidth(glyphs.back().c_str());p+=bytes;
        }
        double along=-total/2.0;
        for(const auto &glyph:glyphs) {
            const int advance=hints.textWidth(glyph.c_str());
            uint32_t code=uint8_t(glyph[0])&(glyph.size()==1?0x7f:glyph.size()==2?0x1f:0xf);
            for(size_t i=1;i<glyph.size();++i) code=(code<<6)|(uint8_t(glyph[i])&63);
            const uint8_t *bitmap=data+24+count*28,*metric=nullptr;
            for(unsigned i=0;i<count;++i) {
                const auto *m=data+24+i*28;
                if(font_word(m)==code) { metric=m;break; }
                bitmap+=font_word(m+4)*font_word(m+8);
            }
            if(!metric || cursor>=kArcGlyphCount) { printf("ERROR role_cache_glyph\n");return false; }
            const int w=font_word(metric+8),h=font_word(metric+4);
            const float left=(40-advance)/2-20+int32_t(font_word(metric+20));
            const float top=ascent-int32_t(font_word(metric+16))-height/2;
            const double angle=(side?315.25:224.75)+(along+advance/2.0)/186*180/M_PI;
            const float center_x=233+origin()+186*std::cos(angle*M_PI/180);
            const float center_y=233+origin()+186*std::sin(angle*M_PI/180);
            const float c=std::cos((angle+90)*M_PI/180),s=std::sin((angle+90)*M_PI/180);
            auto &cached=arc_glyphs[cursor];cached.x=std::floor(center_x-kArcGlyphSide/2);cached.y=std::floor(center_y-kArcGlyphSide/2);
            cached.color=side?kBlueInk:kYellowInk;cached.alpha=arc_alpha+cursor*kArcGlyphSide*kArcGlyphSide;
            auto sample=[&](int x,int y) { return x>=0 && y>=0 && x<w && y<h?bitmap[y*w+x]:0; };
            for(int y=0;y<kArcGlyphSide;++y) for(int x=0;x<kArcGlyphSide;++x) {
                const float dx=cached.x+x+.5f-center_x,dy=cached.y+y+.5f-center_y;
                const float u=c*dx+s*dy-left-.5f,v=-s*dx+c*dy-top-.5f;
                const int ix=std::floor(u),iy=std::floor(v);const float ax=u-ix,ay=v-iy;
                cached.alpha[y*kArcGlyphSide+x]=std::lround((sample(ix,iy)*(1-ax)+sample(ix+1,iy)*ax)*(1-ay)
                                                    +(sample(ix,iy+1)*(1-ax)+sample(ix+1,iy+1)*ax)*ay);
            }
            ++cursor;along+=advance;
        }
    }
    size_t packed=0;
    for(auto &g:arc_glyphs) {
        int left=kArcGlyphSide,top=kArcGlyphSide,right=-1,bottom=-1;
        for(int y=0;y<kArcGlyphSide;++y) for(int x=0;x<kArcGlyphSide;++x) if(g.alpha[y*kArcGlyphSide+x]) {
            left=std::min(left,x);top=std::min(top,y);right=std::max(right,x);bottom=std::max(bottom,y);
        }
        if(right<left) return false;
        g.w=right-left+1;g.h=bottom-top+1;
        auto *source=g.alpha;g.alpha=arc_alpha+packed;
        for(int y=0;y<g.h;++y) std::memmove(g.alpha+y*g.w,source+(top+y)*kArcGlyphSide+left,g.w);
        packed+=g.w*g.h;g.x+=left;g.y+=top;
    }
    auto *trimmed=static_cast<uint8_t *>(heap_caps_malloc(packed,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!trimmed) return false;
    std::memcpy(trimmed,arc_alpha,packed);
    for(auto &g:arc_glyphs) g.alpha=trimmed+(g.alpha-arc_alpha);
    heap_caps_free(arc_alpha);arc_alpha=trimmed;
    printf("CHARACTER_CACHE role_bytes=%u\n",unsigned(packed));
    return cursor==kArcGlyphCount && prepare_arc_cache();
}
void role_arc(int side)
{
    const double centre=side?315.25:224.75,start=centre-12,end=centre+12;
    canvas.fillArc(233+origin(),233+origin(),226,212,start,end,side?kBlue:kYellow);
    for(double angle:{start,end}) canvas.fillCircle(233+origin()+std::lround(219*std::cos(angle*M_PI/180)),233+origin()+std::lround(219*std::sin(angle*M_PI/180)),7,side?kBlue:kYellow);
}
bool prepare_arc_cache()
{
    canvas.clearClipRect();
    auto scan=[&](bool store) {
        size_t count=0;
        for(int side=0;side<2;++side) {
            canvas.fillSprite(TFT_BLACK);role_arc(side);
            const auto *pixels=static_cast<const uint16_t *>(canvas.getBuffer());
            for(int y=0;y<canvas.height();++y) for(int x=0;x<canvas.width();) {
                const uint16_t color=pixels[y*canvas.width()+x];
                if(!color) { ++x;continue; }
                const int begin=x;
                while(x<canvas.width() && pixels[y*canvas.width()+x]==color) ++x;
                if(store) arc_spans[count]={uint16_t(begin),uint16_t(y),uint16_t(x-begin),color};
                ++count;
            }
        }
        return count;
    };
    arc_span_count=scan(false);
    const size_t bytes=arc_span_count*sizeof(ArcSpan);
    // Parent's added-internal-RAM budget: leave room for six damage rectangles.
    constexpr size_t state_bytes=6*4*sizeof(int)+sizeof(arc_spans)+sizeof(arc_span_count)+sizeof(character_background_invalid);
    if(bytes+state_bytes>8*1024) { printf("ERROR arc_internal_budget bytes=%u\n",unsigned(bytes+state_bytes));return false; }
    arc_spans=static_cast<ArcSpan *>(heap_caps_malloc(bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(!arc_spans) return false;
    scan(true);
    printf("CHARACTER_CACHE arc_internal_bytes=%u spans=%u state_bytes=%u\n",unsigned(bytes),unsigned(arc_span_count),unsigned(state_bytes));
    return true;
}
void cached_roles()
{
    auto *pixels=static_cast<uint16_t *>(canvas.getBuffer());
    const int width=canvas.width();
    for(const auto &g:arc_glyphs) {
        const int r=((g.color>>11)&31)*255/31,green=((g.color>>5)&63)*255/63,b=(g.color&31)*255/31;
        for(int y=0;y<g.h;++y) {
          auto *row=pixels+(g.y+y)*width+g.x;
          const auto *alpha_row=g.alpha+y*g.w;
          for(int x=0;x<g.w;++x) {
            const int alpha=alpha_row[x];if(!alpha) continue;
            auto &pixel=row[x];
            if(alpha==255) pixel=__builtin_bswap16(g.color);
            else {
                const uint32_t background=lgfx::convert_to_rgb888(lgfx::swap565_t(pixel));
                pixel=lgfx::swap565_t((r*alpha+int(background>>16)*(255-alpha))/255,
                    (green*alpha+int((background>>8)&255)*(255-alpha))/255,(b*alpha+int(background&255)*(255-alpha))/255).raw;
            }
          }
        }
    }
    for(size_t i=0;i<arc_span_count;++i) {
        const auto span=arc_spans[i];
        std::fill_n(pixels+span.y*width+span.x,span.length,span.color);
    }
}
void draw_roles()
{
    if(animated) { cached_roles();return; }
    for (int side = 0; side < 2; ++side) {
        const char *role = side ? right_role : left_role;
        if (!*role) { continue; }
        const int size = style_labels ? 17 : 20;
        hints.unloadFont(); hints.loadFont(font(size).data);
        hints.setTextColor(side ? kBlueInk : kYellowInk, kIvory);
        // Buttons sit just below the upper diagonals; centres tuned on the device by
        // the human (7 degrees up from 217.75/322.25). Screen degrees, clockwise from +x.
        const double centre = side ? 315.25 : 224.75;
        const double cx = 233 + origin(), cy = 233 + origin();
        // Lay the role text along the circle, one character at a time, each turned
        // to the tangent so the words follow the bezel instead of a straight line.
        const double kTextRadius = role_text_radius;
        std::vector<std::string> glyphs;
        int total = 0;
        for (const char *p = role; *p;) {
            const unsigned char lead = *p;
            const int count = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            glyphs.emplace_back(p, count); total += hints.textWidth(glyphs.back().c_str()); p += count;
        }
        double along = -total / 2.0;
        for (const auto &glyph : glyphs) {
            const int width = hints.textWidth(glyph.c_str());
            const double angle = centre + (along + width / 2.0) / kTextRadius * 180 / M_PI;
            hints.fillSprite(kIvory);
            hints.drawString(glyph.c_str(), (hints.width() - width) / 2, 0);
            hints.setPivot(hints.width() / 2.0, hints.fontHeight() / 2.0);
            hints.pushRotateZoomWithAA(&canvas, cx + kTextRadius * std::cos(angle * M_PI / 180),
                                       cy + kTextRadius * std::sin(angle * M_PI / 180), angle + 90, 1, 1, kIvory);
            along += width;
        }
        // 24-degree arc so its curvature along the round bezel is visible.
        role_arc(side);
    }
}
void present(int64_t frame_started=0,characters::DrawTiming drawing={0,0})
{
    const int64_t arc_started=frame_started?esp_timer_get_time():0;
    use_font(32); draw_roles();
    const int64_t arc_completed=frame_started?esp_timer_get_time():0;
    auto *pixels=static_cast<uint16_t *>(canvas.getBuffer());
    const int width=canvas.width(),height=canvas.height();
    for(int y=0;y<height;++y) {
        auto *row=pixels+y*width;const auto span=circle_spans[y];
        std::memset(row,0,span.left*sizeof(uint16_t));
        std::memset(row+span.right+1,0,(width-span.right-1)*sizeof(uint16_t));
    }
    const int64_t transfer_started=frame_started?esp_timer_get_time():0;
    canvas.pushSprite(0,0);
    if(frame_started) {
        const int64_t completed=esp_timer_get_time();
        if(last_present_us) {
            ++timed_frames;render_time_us+=transfer_started-frame_started;
            background_time_us+=drawing.background_us;character_time_us+=drawing.character_us;
            arc_time_us+=arc_completed-arc_started;
            ui_clip_time_us+=transfer_started-frame_started-drawing.background_us-drawing.character_us-(arc_completed-arc_started);
            transfer_time_us+=completed-transfer_started;
            other_time_us+=std::max<int64_t>(0,frame_started-last_present_us);
        }
        last_present_us=completed;
    }
}

std::string line_text(const Line &line)
{
    if (line.begin == line.end) { return ""; }
    return source.substr(tokens[line.begin].begin, tokens[line.end - 1].end - tokens[line.begin].begin);
}

int chord_width(int y)
{
    const double radius = std::min(canvas.width(), canvas.height()) / 2.0;
    const double dy = std::max(std::abs(y - canvas.height() / 2.0),
                               std::abs(y + canvas.fontHeight() - canvas.height() / 2.0));
    int width = (int)std::floor(2 * std::sqrt(std::max(0.0, radius * radius - dy * dy)));
    return width;
}

struct Break { int cost = std::numeric_limits<int>::max(), previous = -1, end = -1; };

View layout(int offset, Break *workspace)
{
    const int size = tokens.size();
    const int stride = size + 1;
    for (int count = 1; count <= rows; ++count) {
        std::fill(workspace, workspace + (rows + 1) * stride, Break{});
        workspace[offset].cost = 0;
        const int top = region_top + (region_height - count * canvas.fontHeight()) / 2;
        for (int row = 0; row < count; ++row) {
            const int capacity = chord_width(top + row * canvas.fontHeight());
            for (int begin = offset; begin < size; ++begin) {
                const auto &previous = workspace[row * stride + begin];
                if (previous.cost == std::numeric_limits<int>::max()) { continue; }
                int width = 0;
                for (int end = begin; end < size; ++end) {
                    if (tokens[end].newline && end != begin) { break; }
                    width += tokens[end].width;
                    if (width > capacity) { break; }
                    const int ink_end = tokens[end].newline ? end : end + 1;
                    const bool paragraph_end = ink_end == size || tokens[ink_end].newline;
                    const int next = paragraph_end && ink_end < size ? ink_end + 1 : ink_end;
                    // A genuinely one-character paragraph is valid; a wrapped
                    // paragraph ending with a lone character is redistributed.
                    if (ink_end - begin == 1 && paragraph_end && begin > tokens[begin].paragraph) { continue; }
                    const int slack = capacity - width;
                    const int cost = previous.cost + slack * slack;
                    auto &candidate = workspace[(row + 1) * stride + next];
                    if (cost < candidate.cost) { candidate = {cost, begin, ink_end}; }
                    if (tokens[end].newline) { break; }
                }
            }
        }
        int end = size;
        if (workspace[count * stride + end].previous < 0) {
            if (count != rows) { continue; }
            while (end > offset && workspace[count * stride + end].previous < 0) { --end; }
        }
        View view;
        for (int row = count; row > 0; --row) {
            const auto &item = workspace[row * stride + end];
            if (item.previous < 0) { printf("ERROR screen_layout\n"); return {}; }
            Line line{item.previous, item.end, end, 0, chord_width(top + (row - 1) * canvas.fontHeight()), 0,
                      top + (row - 1) * (int)canvas.fontHeight()};
            line.width = canvas.textWidth(line_text(line).c_str());
            line.x = (canvas.width() - line.width) / 2;
            view.lines.push_back(line);
            end = item.previous;
        }
        std::reverse(view.lines.begin(), view.lines.end());
        return view;
    }
    return {};
}

void hex(const uint8_t *bytes, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        putchar(digits[bytes[i] >> 4]);
        putchar(digits[bytes[i] & 15]);
    }
}

void draw()
{
    animated = false;
    canvas.fillSprite(kIvory);
    if (views.empty()) { return; }
    for (const auto &line : views[first].lines) {
        canvas.drawString(line_text(line).c_str(), line.x, line.y);
    }
    if (actions) {
        const int font_height = canvas.fontHeight();
        const int top = (canvas.height() - safe_side) / 2;
        const char *header1 = error_actions ? "印刷に失敗" : "これで";
        const char *header2 = error_actions ? "" : "印刷しますか？";
        canvas.drawString(header1, (canvas.width() - canvas.textWidth(header1)) / 2, top);
        canvas.drawString(header2, (canvas.width() - canvas.textWidth(header2)) / 2, top + font_height);
        const int controls_y = top + safe_side - font_height;
        if (preview_bits) { paint_preview(preview_bits, preview_rows, controls_y - font_height, font_height); }
        if (paper_length > 150) {
            char length[32]; snprintf(length, sizeof(length), "%.0fmm", paper_length);
            canvas.setTextColor(TFT_RED, kIvory);
            canvas.drawString(length, (canvas.width() - canvas.textWidth(length)) / 2, controls_y - 2 * font_height);
            canvas.setTextColor(kInk, kIvory);
        }
        const char *left = error_actions ? "戻る" : "やり直し";
        const char *right = error_actions ? "もう一度" : "印刷";
        const int gap = canvas.textWidth(" ");
        const int width = canvas.textWidth(left) + gap + canvas.textWidth(right);
        const int x = (canvas.width() - width) / 2;
        canvas.drawString(left, x, controls_y);
        canvas.setTextColor(TFT_CYAN, TFT_BLACK);
        canvas.drawString(right, x + canvas.textWidth(left) + gap, controls_y);
        canvas.setTextColor(kInk, kIvory);
    }
    draw_roles();
    canvas.pushSprite(0, 0);
}
}

bool begin()
{
    screen_mutex = xSemaphoreCreateRecursiveMutexStatic(&screen_mutex_storage);
    canvas.setPsram(true);
    canvas.setColorDepth(16);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) { return false; }
    circle_spans=static_cast<CircleSpan *>(heap_caps_malloc(canvas.height()*sizeof(CircleSpan),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(!circle_spans) return false;
    const double radius=canvas.width()/2.0;
    for(int y=0;y<canvas.height();++y) {
        const double dy=y+0.5-canvas.height()/2.0;
        const double half=std::sqrt(std::max(0.0,radius*radius-dy*dy));
        circle_spans[y]={uint16_t(std::max(0,int(std::ceil(radius-half-0.5)))),
            uint16_t(std::min(int(canvas.width())-1,int(std::floor(radius+half-0.5))))};
    }
    if (!canvas.loadFont(noto48_start)) { return false; }
    hints.setPsram(true);
    hints.setColorDepth(16);
    // One 17/20px glyph is rotated at a time; discard only transparent side margins.
    if (!hints.createSprite(40, 40) || !hints.loadFont(noto16_start)) { return false; }
    hints.setTextDatum(top_left);
    canvas.setTextSize(kScale);
    canvas.setTextWrap(false);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(kInk, kIvory);
    // Keep the previous safe vertical band; each row uses its own circle chord.
    safe_side = (int)std::floor(std::min(canvas.width(), canvas.height()) / std::sqrt(2.0));
    rows = safe_side / canvas.fontHeight();
    region_top = 0; region_height = canvas.height();
    return rows > 0;
}

void build_views()
{
    tokens.clear();
    views.clear();
    int paragraph = 0;
    for (size_t offset = 0; offset < source.size();) {
        const unsigned char lead = source[offset];
        const size_t count = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        const std::string character = source.substr(offset, count);
        tokens.push_back({offset, offset + count, lead == '\n' ? 0 : canvas.textWidth(character.c_str()), paragraph, lead == '\n'});
        offset += count;
        if (lead == '\n') { paragraph = tokens.size(); }
    }
    std::unique_ptr<Break, decltype(&heap_caps_free)> workspace(
        static_cast<Break *>(heap_caps_malloc((rows + 1) * (tokens.size() + 1) * sizeof(Break),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!workspace) { printf("ERROR screen_layout allocation\n"); return; }
    int offset = 0;
    while (offset < (int)tokens.size()) {
        View view = layout(offset, workspace.get());
        if (view.lines.empty()) { return; }
        const int next = view.lines.front().next;
        const bool complete = view.lines.back().next == (int)tokens.size();
        views.push_back(std::move(view));
        if (complete) { break; }
        offset = next;
    }
    first = 0;
    draw();
}

void show(const char *status, const std::string &text)
{
    Lock lock;
    use_font(32);
    style_labels = nullptr;
    actions = false; preview_bits = nullptr; paper_length = 0;
    rows = safe_side / canvas.fontHeight(); region_top = 0; region_height = canvas.height();
    source = status;
    if (!text.empty()) { source += '\n'; source += text; }
    build_views();
}

void scroll(int delta)
{
    Lock lock;
    if (style_labels) {
        int content = result_lines.size()*font(32).size;
        for(int i=0;i<style_count;++i) { content+=(i==selected_style ? 86 : 70)+14; }
        list_offset=std::clamp(list_offset+delta*32,0,std::max(0,content-(466-168)));
        first=list_offset/32;
        draw_previews(); return;
    }
    first = std::clamp(first + delta, 0, std::max(0, (int)views.size() - 1));
    draw();
}

void waiting()
{
    Lock lock; background(); roles("おして話す", "");
    icon(kMic,kMicWidth,kMicHeight,157,97);
    text_at("左上のボタンを",34,233,294,kInk);
    text_at("おしながら",34,233,336,kInk); text_at("話してね",34,233,378,kInk);
    canvas.fillCircle(213+origin(),418+origin(),6,rgb(0xB9B26A));
    canvas.fillCircle(233+origin(),418+origin(),6,rgb(0xB9B26A));
    canvas.fillCircle(256+origin(),414+origin(),6,rgb(0xB9B26A));
    present();
}
void recording() { show("録音中…", ""); }
void result(const std::string &text) { show("認識結果", text.empty() ? "(認識結果なし)" : text); }
void confirmation(const std::string &text) { show("これで\n印刷しますか？", text + "\nやり直し 印刷"); }

void confirmation(const std::string &text, const uint8_t *bits, int height, double length)
{
    Lock lock;
    actions = true; error_actions = false; preview_bits = bits; preview_rows = height; paper_length = length;
    const int fh = canvas.fontHeight();
    region_top = (canvas.height() - safe_side) / 2 + 2 * fh;
    region_height = safe_side - 4 * fh - (length > 150 ? fh : 0);
    rows = region_height / fh;
    source = text; build_views();
}

void failure(const std::string &reason)
{
    Lock lock; background(); roles("おして話す","もういちど");
    canvas.fillCircle(233+origin(),160+origin(),54,kCoralTrack);
    text_at("!",56,233,180,rgb(0xD9675A));
    if(reason=="プリンターにつながりません") {
        text_at("プリンターに",32,233,262,kInk);
        text_at("つながらないよ",32,233,302,kInk);
        text_at("電源を見てね",20,233,344,kSub);
        source=reason; actions=false; present();
    } else {
        show("いんさつできなかったよ",reason);
    }
}

Action action_at(int x, int y)
{
    if (!actions) { return Action::none; }
    const int top = (canvas.height() + safe_side) / 2 - canvas.fontHeight();
    if (y < top || y >= top + canvas.fontHeight()) { return Action::none; }
    const char *left = error_actions ? "戻る" : "やり直し";
    const char *right = error_actions ? "もう一度" : "印刷";
    const int gap = canvas.textWidth(" ");
    const int left_width = canvas.textWidth(left), right_width = canvas.textWidth(right);
    const int begin = (canvas.width() - left_width - gap - right_width) / 2;
    if (x >= begin && x < begin + left_width) { return error_actions ? Action::back : Action::retry; }
    if (x >= begin + left_width + gap && x < begin + left_width + gap + right_width) { return Action::print; }
    return Action::none;
}

bool scroll_area(int x, int y)
{
    if (views.empty()) { return false; }
    for (const auto &line : views[first].lines) {
        if (x >= line.x && x < line.x + line.width && y >= line.y && y < line.y + canvas.fontHeight()) { return true; }
    }
    return false;
}

void roles(const char *left, const char *right) { Lock lock; left_role = left; right_role = right; }

void recording_progress(double remaining, double dbfs, double total)
{
    Lock lock; background(); roles("はなすと終わり", ""); role_text_radius = 158;
    canvas.fillArc(233+origin(),233+origin(),199,179,0,360,kCoralTrack);
    canvas.fillArc(233+origin(),233+origin(),199,179,270,270+360*remaining/total,kCoral);
    text_at("きいてるよ",40,233,205,kInk);
    icon(kWave,kWaveWidth,kWaveHeight,170,215);
    char message[64]; snprintf(message,sizeof(message),"あと %.0fびょう",remaining);
    text_at(message,24,233,330,kSub);
    (void)dbfs; present();
}

int first_row() { return first; }
int line_height() { return canvas.fontHeight(); }

void label_preview(const uint8_t *bits, int height)
{
    Lock lock; animated = false;
    canvas.fillSprite(kIvory);
    paint_preview(bits, height, (canvas.height() - safe_side) / 2, safe_side);
    draw_roles(); canvas.pushSprite(0, 0);
}

namespace {
void draw_previews()
{
    animated = false;
    role_text_radius = 186;
    canvas.fillSprite(TFT_BLACK);
    canvas.fillCircle(canvas.width()/2,canvas.height()/2,canvas.width()/2-1,kIvory);
    text_at(list_heading,22,233,150,kSub);
    canvas.setClipRect(0,158+origin(),canvas.width(),canvas.height()-158-origin());
    int y = 168-list_offset;
    for (int style=0;style<style_count;++style) {
        const bool selected = style==selected_style;
        const int width=selected ? 320 : style==selected_style+1 ? 290 : style==selected_style+2 ? 270 : 230;
        const int height=selected ? 86 : 70;
        const int left=(466-width)/2;
        if (y+height>158 && y<466) {
            canvas.fillRoundRect(left+origin(),y+origin(),width,height,selected ? 22 : 18,kCard);
            const int stroke=selected ? 6 : 3;
            for (int edge=0;edge<stroke;++edge) {
                canvas.drawRoundRect(left+edge+origin(),y+edge+origin(),width-2*edge,height-2*edge,(selected ? 22 : 18)-edge,selected ? kOlive : kOutline);
            }
            const int inset=selected ? 30 : 24;
            text_at(style_names[style],selected ? 17 : 15,left+inset,y+(selected ? 26 : 22),selected ? kOlive : kSub,false,kCard);
            const int available_height=selected ? 46 : 38;
            // Draw the exact job bits; no UI-font substitution in the label.
            const int max_width=width-2*inset;
            const double scale=std::min(double(max_width)/paper_rows(style_labels[style].rows),double(available_height)/kPaperDots);
            const int draw_width=std::max(1,int(paper_rows(style_labels[style].rows)*scale));
            const int x=(466-draw_width)/2+origin(),top=y+(selected ? 35 : 29)+origin();
            draw_label(style_labels[style].bits.get(),style_labels[style].rows,scale,x,top);
            if (style_labels[style].paper_mm()>150) {
                char length[32];snprintf(length,sizeof(length),"%.0fmm",style_labels[style].paper_mm());
                text_at(length,15,233,y+height-4,TFT_RED);
            }
        }
        y+=height+14;
    }
    // Preserve access to the complete recognition text for long labels.
    for (const auto &line:result_lines) { text_at(line,32,233,y+32,kInk); y+=canvas.fontHeight(); }
    canvas.clearClipRect();
    if (quiet_warning) { text_at("声が小さいようです",22,233,132,kSub); }
    present();
}

void paint_preview(const uint8_t *bits, int height, int available_top, int available_height)
{
    // Rotate the exact 96-dot print raster back to horizontal for the preview.
    const int width = height;
    const double scale = std::min(double(safe_side) / paper_rows(width), double(available_height) / kPaperDots);
    const int draw_width = std::max(1, int(paper_rows(width) * scale));
    const int draw_height = std::max(1, int(kPaperDots * scale));
    const int left = (canvas.width() - draw_width) / 2;
    const int top = available_top + (available_height - draw_height) / 2;
    draw_label(bits, height, scale, left, top);
}
}

void previews(const std::string &text, const label_raster::Label *labels, const char *const *names, int count,
              int selected, bool reset_scroll, const char *heading, const char *right)
{
    Lock lock;
    roles("おして話す", right);
    style_labels = labels; style_names = names; style_count = count; list_heading = heading;
    selected_style = selected; source = text;
    preview_bits = labels[selected].bits.get(); preview_rows = labels[selected].rows;
    actions = false; first = 0;
    result_lines.clear();
    std::string line;
    for (size_t offset = 0; offset < text.size();) {
        const uint8_t lead = text[offset];
        const size_t count = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : 3;
        const std::string next = text.substr(offset, count);
        if (!line.empty() && canvas.textWidth((line + next).c_str()) > safe_side) { result_lines.push_back(line); line.clear(); }
        line += next; offset += count;
    }
    if (!line.empty()) { result_lines.push_back(line); }
    if (result_lines.size() > 1 && result_lines.back().size() == 3 && result_lines[result_lines.size()-2].size() > 3) {
        auto &previous = result_lines[result_lines.size()-2];
        result_lines.back() = previous.substr(previous.size()-3) + result_lines.back(); previous.resize(previous.size()-3);
    }
    if (reset_scroll) { list_offset = 0; }
    else {
        int begin=0;
        for(int i=0;i<selected;++i) { begin+=70+14; }
        if(begin<list_offset) { list_offset=begin; }
        if(begin+86>list_offset+298) { list_offset=begin+86-298; }
    }
    first=list_offset/32;
    draw_previews();
}

void printing(bool started)
{
    Lock lock; background(); roles(started ? "" : "やめる","");
    if(preview_bits) { paint_preview(preview_bits,preview_rows,176+origin(),56); }
    text_at("いんさつ中…",34,233,290,kInk);
    icon(kDots,kDotsWidth,kDotsHeight,190,319);
    present();
}
void success(int battery_percent)
{
    Lock lock; background(); roles("","");
    icon(kSuccess,kSuccessWidth,kSuccessHeight,170,117);
    text_at("できたよ！",38,233,295,kInk);
    if(battery_percent>=0) {
        char text[96];snprintf(text,sizeof(text),"プリンターの電池 %d%%",battery_percent);
        text_at(text,22,233,340,kSub);
    }
    present();
}
void volume_warning(bool warning) { Lock lock;quiet_warning=warning; }

namespace {
// UTF-8 prefix of `text` that fits `width` at the current font, with "…" when cut.
std::string fit(const std::string &text, int width)
{
    if (canvas.textWidth(text.c_str()) <= width) { return text; }
    std::string kept;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = text[i];
        const size_t count = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        const std::string next = kept + text.substr(i, count);
        if (canvas.textWidth((next + "…").c_str()) > width) { break; }
        kept = next; i += count;
    }
    return kept + "…";
}

CaptionRow caption_row(const std::string &text,int baseline)
{
    use_font(32);canvas.setTextSize(kScale);
    // Reserve half a font on each side of the narrowest chord crossed by the row.
    const int top=baseline+origin()-font(32).ascent;
    const int capacity=chord_width(top)-font(32).size-2*kScale;
    const int natural=canvas.textWidth(text.c_str());
    float scale=natural>capacity?float(capacity)/natural:kScale;
    canvas.setTextSize(scale);
    if(canvas.textWidth(text.c_str())>capacity) {
        scale=float(capacity-1)/natural;canvas.setTextSize(scale);
    }
    CaptionRow row{233+origin()-canvas.textWidth(text.c_str())/2,
        baseline+origin()-int(std::ceil(font(32).ascent*scale)),
        canvas.textWidth(text.c_str()),int(std::ceil(canvas.fontHeight())),scale};
    canvas.setTextSize(kScale);return row;
}

void draw_caption(const std::string &text,const CaptionRow &row,uint16_t ink,bool outlined)
{
    if(text.empty()) return;
    use_font(32);canvas.setTextSize(row.scale);
    if(outlined) {
        canvas.setTextColor(ink==TFT_WHITE?TFT_BLACK:kIvory);
        canvas.drawString(text.c_str(),row.x-kScale,row.y);
        canvas.drawString(text.c_str(),row.x+kScale,row.y);
        canvas.drawString(text.c_str(),row.x,row.y-kScale);
        canvas.drawString(text.c_str(),row.x,row.y+kScale);
    }
    canvas.setTextColor(ink);canvas.drawString(text.c_str(),row.x,row.y);
    canvas.setTextSize(kScale);
}

void draw_avatar()
{
    const int64_t started=esp_timer_get_time();
    canvas.clearClipRect();
    if(character_avatar.background==0 && !character_background_invalid) {
        auto *pixels=static_cast<uint16_t *>(canvas.getBuffer());
        const int width=canvas.width(),height=canvas.height();
        auto clear=[&](int x,int y,int w,int h) {
            for(int row=std::max(0,y);row<std::min(height,y+h);++row)
                std::memset(pixels+row*width+x,0,w*sizeof(uint16_t));
        };
        for(const auto &g:arc_glyphs) clear(g.x,g.y,g.w,g.h);
        use_font(32);
        for(int baseline:{kCaptionBaseline-2*kCaptionStep,kDetailBaseline-2*kCaptionStep,kCaptionBaseline,kDetailBaseline})
            clear(0,baseline+origin()-font(32).ascent-kScale,width,canvas.fontHeight()+2*kScale);
        clear(233+origin()-100,390,200,10);
    }
    const auto drawing=characters::draw(canvas, character_avatar,character_view,character_background_invalid);
    character_background_invalid=false;
    const Avatar &a = character_avatar;
    const int cx=233+origin(),cy=233+origin();
    if(a.ring) canvas.fillArc(cx,cy,233,224,0,360,canvas.color888(a.ring_rgb>>16,(a.ring_rgb>>8)&255,a.ring_rgb&255));
    const uint16_t ink = a.background==0 ? TFT_WHITE : kInk;
    draw_caption(character_caption,character_text[0],ink,!character_kana.empty());
    draw_caption(character_kana,character_text[1],ink,true);
    if(a.gauge>=0) {
        canvas.fillRoundRect(cx-100,390,200,10,5,kOutline);
        canvas.fillRoundRect(cx-100,390,std::max(10,200*std::min(a.gauge,100)/100),10,5,kOlive);
    }
    present(started,drawing);
}
void animation_loop(void *)
{
    TickType_t wake=xTaskGetTickCount();
    while(true) {
        { Lock lock; if(animated) draw_avatar(); }
        // The original action renderer's 30ms frame cadence, now continuous during listening.
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(30));
    }
}
}

bool start_characters()
{
    Lock lock;
    if(characters_ready) return true;
    if(!characters::begin()) return false;
    if(!prepare_role_cache()) return false;
    if(!xTaskCreateStatic(animation_loop,"characters",sizeof(animation_stack),nullptr,1,animation_stack,&animation_task_storage)) return false;
    characters_ready=true;
    return true;
}

void avatar(const Avatar &a, const std::string &caption, const std::string &kana)
{
    Lock lock;
    if(!characters_ready) { printf("ERROR characters_not_ready\n"); return; }
    if(!animated) last_present_us=0;
    if(!animated || a.background!=character_avatar.background || a.ring!=character_avatar.ring || a.ring_rgb!=character_avatar.ring_rgb)
        character_background_invalid=true;
    character_avatar=a;
    use_font(32);
    character_caption=caption;
    character_kana=kana;
    const int shift=kana.empty()?0:2*kCaptionStep;
    character_text[0]=caption_row(caption,kCaptionBaseline-shift);
    character_text[1]=caption_row(kana,kDetailBaseline-shift);
    style_labels=nullptr;
    role_text_radius=186;
    roles("おして話す","もどる");
    animated=true;
    draw_avatar();
}

void animation_stats() {
    Lock lock; characters::stats();
    printf("CHARACTER_TIMING samples=%llu draw_us=%llu transfer_us=%llu other_us=%llu background_us=%llu character_us=%llu arc_us=%llu ui_clip_us=%llu\n",
        (unsigned long long)timed_frames,(unsigned long long)render_time_us,
        (unsigned long long)transfer_time_us,(unsigned long long)other_time_us,
        (unsigned long long)background_time_us,(unsigned long long)character_time_us,
        (unsigned long long)arc_time_us,(unsigned long long)ui_clip_time_us);
    printf("CHARACTER_VIEW group=%d selected=%d revision=%u\n",character_view.group,character_view.selected,character_view.revision);
    for(int i=0;i<2;++i) {
        const auto &row=character_text[i];const auto &text=i?character_kana:character_caption;
        printf("CAPTION_TEXT row=%d x=%d y=%d width=%d height=%d scale=%.6f outline=%d text=%s\n",i,
            row.x,row.y,row.width,row.height,row.scale,!character_kana.empty(),text.c_str());
    }
}

void restore_character(unsigned selected) {
    Lock lock;character_view.select(selected<unsigned(characters::count())?int(selected):0);
}
unsigned selected_character() { Lock lock;return character_view.selected; }
void gather_characters() { Lock lock;character_view.gather(); }

namespace {
bool character_event(const characters::TouchEvent &event,const char *source,characters::TouchGesture &gesture)
{
    using K=characters::TouchKind;
    if(event.kind==K::mode_hold) return true;
    if(!animated) return false;
    bool changed=false;
    if(!character_view.group && (event.kind==K::swipe_left || event.kind==K::swipe_right)) {
        character_view.swipe(event.kind==K::swipe_left?1:-1,characters::count());changed=true;
    } else if(!character_view.group && event.kind==K::double_tap) {
        character_view.gather();changed=true;
    } else if(character_view.group && event.kind==K::tap) {
        const int selected=characters::hit(event.x-origin(),event.y-origin());
        if(selected>=0) { character_view.select(selected);gesture.clear();changed=true; }
    }
    if(changed) {
        printf("CHARACTER_TOUCH source=%s kind=%d x=%d y=%d group=%d selected=%d\n",source,int(event.kind),event.x,event.y,character_view.group,character_view.selected);
        draw_avatar();
    }
    return false;
}
}

bool character_touch(bool down,int x,int y,uint32_t ms)
{
    Lock lock;
    return character_event(character_gesture.update(down,x,y,ms),"physical",character_gesture);
}

bool diagnostic_touch(unsigned kind,int x,int y,int dx,int dy,uint16_t duration)
{
    Lock lock;
    const bool stationary=dx==0 && dy==0;
    const bool valid=kind<=3 && (kind<2?(stationary && duration<characters::kTouchClickMs):
        kind==2?(std::abs(dx)>characters::kTouchMovePixels && std::abs(dx)>std::abs(dy) && duration<characters::kCharacterModeHoldMs):
        (stationary && duration>=characters::kCharacterModeHoldMs));
    if(!valid || (!animated && kind!=3)) { printf("ERROR CHARACTER_DIAG unsupported\n");return false; }
    // Atomic traces feed the exact same classifier as physical coordinates, without
    // leaving a diagnostic state that could disable the user's touch panel.
    characters::TouchGesture trace;
    bool mode=false;
    auto feed=[&](bool down,int px,int py,uint32_t ms) {
        mode=character_event(trace.update(down,px,py,ms),"serial",trace) || mode;
    };
    feed(true,x,y,0);
    feed(true,x+dx,y+dy,duration);
    feed(false,x+dx,y+dy,duration+1);
    if(kind==1) { feed(true,x,y,duration+2);feed(false,x,y,2*duration+2); }
    printf("CHARACTER_DIAG kind=%u group=%d selected=%d mode_hold=%d\n",kind,character_view.group,character_view.selected,mode);
    return mode;
}

void home_panel(const Tile *tiles, int count, int focus, const std::string &caption, const std::string &kana)
{
    Lock lock; background(); roles("", "もどる");
    constexpr int kWidth = 122, kHeight = 150, kGap = 8, kTop = 132;  // clear of the button arc
    const int left = 233 - (count * kWidth + (count - 1) * kGap) / 2;
    for (int i = 0; i < count; ++i) {
        const Tile &t = tiles[i];
        const int x = left + i * (kWidth + kGap) + origin(), y = kTop + origin(), cx = x + kWidth / 2;
        // Keep color888's own type: narrowing it to uint16_t made it read as swapped RGB565.
        const auto lit = canvas.color888(t.rgb >> 16, (t.rgb >> 8) & 255, t.rgb & 255);
        // Off colours in the same type, so a `t.on ? lit : off` never mixes the two encodings.
        const auto tv_off = canvas.color888(0x2A, 0x28, 0x22), bar_off = canvas.color888(0x8C, 0x86, 0x73),
                   bulb_off = canvas.color888(0xE8, 0xE0, 0xCC);
        canvas.fillRoundRect(x, y, kWidth, kHeight, 14, kCard);
        canvas.drawRoundRect(x, y, kWidth, kHeight, 14, i == focus ? kOlive : kOutline);
        if (i == focus) { canvas.drawRoundRect(x + 1, y + 1, kWidth - 2, kHeight - 2, 13, kOlive); }
        switch (t.kind) {
        case Appliance::tv:
            canvas.fillRoundRect(cx - 40, y + 16, 80, 52, 6, kInk);
            canvas.fillRect(cx - 34, y + 22, 68, 40, t.on ? lit : tv_off);
            canvas.fillRect(cx - 12, y + 70, 24, 5, kInk);
            break;
        case Appliance::aircon:
            canvas.fillRoundRect(cx - 44, y + 22, 88, 34, 8, kOutline);
            canvas.fillRoundRect(cx - 40, y + 46, 80, 6, 3, t.on ? lit : bar_off);
            for (int k = 0; t.on && k < 3; ++k) {  // air flow
                canvas.drawWideLine(cx - 24 + k * 24, y + 60, cx - 30 + k * 24, y + 74, 2.0f, lit);
            }
            break;
        case Appliance::light:
            canvas.fillCircle(cx, y + 40, 22, t.on ? lit : bulb_off);
            canvas.fillRect(cx - 9, y + 62, 18, 10, kSub);
            for (int k = 0; t.on && k < 8; ++k) {
                const double a = k * M_PI / 4;
                canvas.drawWideLine(cx + std::lround(28 * std::cos(a)), y + 40 + std::lround(28 * std::sin(a)),
                                    cx + std::lround(36 * std::cos(a)), y + 40 + std::lround(36 * std::sin(a)), 1.5f, lit);
            }
            break;
        }
        text_at(t.name, 24, cx - origin(), kTop + 104, kInk, true, kCard);
        text_at(t.status, 24, cx - origin(), kTop + 134, t.on ? kOlive : kSub, true, kCard);
    }
    use_font(32);
    draw_caption(caption,caption_row(caption,340),kInk,false);
    if (!kana.empty()) { draw_caption(kana,caption_row(kana,376),kSub,false); }
    present();
}

void voice_prompt(const char *line1, const char *line2, const std::string &heard)
{
    Lock lock; background(); roles("", "やめる");
    icon(kMic, kMicWidth, kMicHeight, 157, 97);
    text_at("ラベルいんさつ", 32, 233, 284, kOlive);
    text_at(line1, 32, 233, 330, kInk);
    text_at(line2, 32, 233, 372, kInk);
    if (!heard.empty()) { use_font(32); text_at(fit("「" + heard + "」", 250), 32, 233, 414, kSub); }
    present();
}

void voice_confirm(const uint8_t *bits, int rows, const std::string &text)
{
    Lock lock; background(); roles("", "やめる");
    // The label as it will print, as wide as the circle allows at that height.
    const double scale = std::min(0.8, 280.0 / paper_rows(rows));  // clear of the right button arc
    const int width = std::lround(paper_rows(rows) * scale);
    draw_label(bits, rows, scale, 233 + origin() - width / 2, 170 + origin() - std::lround(kPaperDots * scale / 2));
    use_font(32);
    text_at(fit(text, 340), 32, 233, 268, kInk);
    text_at("「いんさつ」でいんさつ", 32, 233, 336, kOlive);
    text_at("やりなおし／やめる", 32, 233, 384, kSub);
    present();
}

// While the board restarts with the other model: no mode is named, only that it is getting ready.
void mode_switching(const char *message)
{
    Lock lock; background(); roles("", "");
    text_at(message, 32, 233, 248, kInk);
    present();
}

void dump(void (*write_hex)(const uint8_t *, size_t))
{
    // The underlying CO5300 config keeps readable=false even on its framebuffer
    // wrapper. readRectRGB dispatches to that wrapper's implemented readRect;
    // host proof checks every glyph pixel, so an unsupported/no-op read fails.
    if (views.empty()) { printf("ERROR screen_layout empty\n"); return; }
    printf("LAYOUT width=%d height=%d scale=%d font_pixels=%d font_height=%d safe_side=%d first=%d rows=%d total=%u framebuffer=1 views=%u\n",
           (int)canvas.width(), (int)canvas.height(), kScale, kFontPixels, (int)canvas.fontHeight(), safe_side, first, rows, (unsigned)views[first].lines.size(), (unsigned)views.size());
    printf("SOURCE "); hex(reinterpret_cast<const uint8_t *>(source.data()), source.size()); printf("\n");
    for (int i = 0; i < (int)views[first].lines.size(); ++i) {
        const auto &line = views[first].lines[i];
        const std::string text = line_text(line);
        printf("LINE %d %d %d %d ", i, line.x, line.y, line.width);
        hex(reinterpret_cast<const uint8_t *>(text.data()), text.size()); printf("\n");
        printf("SPAN %d %d %d %d\n", i, line.begin, line.end, line.capacity);
    }
    dump_pixels(write_hex);
}

void dump_pixels(void (*write_hex)(const uint8_t *, size_t))
{
    // Read the device's final display framebuffer, after pushSprite. The CO5300
    // glass cannot be read; M5GFX's OPI-PSRAM framebuffer is the scanout source.
    const size_t bytes = canvas.width() * canvas.height() * 3;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> frame(
        static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!frame) {
        // Action mode keeps less PSRAM free than one frame copy: read one row at a time under
        // the screen lock instead, so a frame drawn in the meantime cannot tear the snapshot.
        std::vector<uint8_t> row(canvas.width() * 3);
        Lock lock;
        flockfile(stdout);
        printf("SCREEN %d %d\n", (int)canvas.width(), (int)canvas.height());
        for (int y = 0; y < canvas.height(); ++y) {
            M5.Display.readRectRGB(0, y, canvas.width(), 1, row.data());
            printf("PIXELS %d ", y);
            write_hex(row.data(), row.size());
            printf("\n");
        }
        printf("SCREEN_END\n");
        fflush(stdout);
        funlockfile(stdout);
        return;
    }
    { Lock lock; M5.Display.readRectRGB(0, 0, canvas.width(), canvas.height(), frame.get()); }
    flockfile(stdout);
    printf("SCREEN %d %d\n", (int)canvas.width(), (int)canvas.height());
    for (int y = 0; y < canvas.height(); ++y) {
        printf("PIXELS %d ", y);
        write_hex(frame.get() + y * canvas.width() * 3, canvas.width() * 3);
        printf("\n");
    }
    printf("SCREEN_END\n");
    fflush(stdout);
    funlockfile(stdout);
}
}
