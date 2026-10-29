#include "action.h"

#include <M5Unified.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_screen.h"
#include "character_commands.h"
#include "talk_commands.h"

namespace action {
namespace {
Speaker g_speaker = nullptr;
using voice_screen::Expression;

// koeact.model.pieces.VOCAB (KoeAct-JP v0.1.1): JSON pieces, enum values and digits, then
// </s>, <s>, <pad>. The ids match a2_grammar.h (A2_VOCAB 62, A2_EOS 59).
const char *const kPieces[] = {
    "[]", "[", "]", ",",
    "{\"name\":\"look\",\"arguments\":{\"direction\":\"",
    "{\"name\":\"turn\",\"arguments\":{\"direction\":\"",
    "{\"name\":\"nod\",\"arguments\":{\"count\":",
    "{\"name\":\"shake\",\"arguments\":{\"count\":",
    "{\"name\":\"bow\",\"arguments\":{}}",
    "{\"name\":\"set_expression\",\"arguments\":{\"expression\":\"",
    "{\"name\":\"set_led\",\"arguments\":{\"color\":\"",
    "{\"name\":\"set_volume\",\"arguments\":{\"level\":",
    "{\"name\":\"adjust_volume\",\"arguments\":{\"direction\":\"",
    "{\"name\":\"set_brightness\",\"arguments\":{\"level\":",
    "{\"name\":\"adjust_brightness\",\"arguments\":{\"direction\":\"",
    "\",\"amount\":\"", "\",\"degrees\":", "\",\"by\":", "\"}}", "}}",
    "left", "right", "up", "down", "up_left", "up_right", "down_left", "down_right", "center",
    "slight", "normal", "large",
    "happy", "sad", "surprised", "neutral", "angry", "sleepy", "doubt",
    "red", "orange", "yellow", "green", "light_blue", "blue", "purple", "pink", "white", "off",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "</s>", "<s>", "<pad>",
};
constexpr int kPieceCount = sizeof(kPieces) / sizeof(kPieces[0]);
static_assert(kPieceCount == 62, "KoeAct-JP v0.1.1 has 62 pieces");
constexpr int kEos = 59, kBos = 60, kPad = 61;

voice_screen::Avatar g_avatar;
int g_volume = 50;  // 0..100; the speaker stays off (it shares I2S with the microphone)

struct Direction { const char *name, *word; double x, y; };
constexpr Direction kDirections[] = {
    {"left", "ひだり", -1, 0}, {"right", "みぎ", 1, 0}, {"up", "うえ", 0, -1}, {"down", "した", 0, 1},
    {"up_left", "ひだりうえ", -0.71, -0.71}, {"up_right", "みぎうえ", 0.71, -0.71},
    {"down_left", "ひだりした", -0.71, 0.71}, {"down_right", "みぎした", 0.71, 0.71},
    {"center", "まんなか", 0, 0},
};
struct Color { const char *name, *word; uint32_t rgb; };
constexpr Color kColors[] = {
    {"red", "あか", 0xE5484D}, {"orange", "オレンジ", 0xF0883E}, {"yellow", "きいろ", 0xF2C14E},
    {"green", "みどり", 0x46A758}, {"light_blue", "みずいろ", 0x5EB1EF}, {"blue", "あお", 0x3E63DD},
    {"purple", "むらさき", 0x8E4EC6}, {"pink", "ピンク", 0xE93D82}, {"white", "しろ", 0xD9D9D9},
};
struct Face { const char *name, *word; Expression expression; };
constexpr Face kFaces[] = {
    {"happy", "にっこり", Expression::happy}, {"sad", "しょんぼり", Expression::sad},
    {"surprised", "びっくり！", Expression::surprised}, {"neutral", "ふつうのかお", Expression::neutral},
    {"angry", "ぷんぷん", Expression::angry}, {"sleepy", "ねむねむ…", Expression::sleepy},
    {"doubt", "はてな？", Expression::doubt},
};

const char *string_of(const cJSON *arguments, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(arguments, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}
int number_of(const cJSON *arguments, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(arguments, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}
const Direction *direction_of(const char *name)
{
    for (const auto &d : kDirections) { if (name && std::strcmp(d.name, name) == 0) { return &d; } }
    return nullptr;
}
// slight / normal / large, or degrees (1..180) as a share of a quarter turn.
double extent_of(const cJSON *arguments)
{
    const char *amount = string_of(arguments, "amount");
    if (amount) { return std::strcmp(amount, "slight") == 0 ? 0.35 : std::strcmp(amount, "large") == 0 ? 1.0 : 0.7; }
    return std::min(1.0, number_of(arguments, "degrees", 45) / 90.0);
}
int step_of(const cJSON *arguments)
{
    const char *amount = string_of(arguments, "amount");
    if (amount) { return std::strcmp(amount, "slight") == 0 ? 10 : std::strcmp(amount, "large") == 0 ? 50 : 25; }
    return std::clamp(number_of(arguments, "by", 25), 1, 100);
}

void frame(const std::string &caption, const std::string &kana, int ms = 30)
{
    voice_screen::avatar(g_avatar, caption, kana.empty() ? kana : "「" + kana + "」");
    if (ms > 0) { vTaskDelay(pdMS_TO_TICKS(ms)); }
}

// Ease the gaze and the face offset to their targets over a few frames.
void glide(double eye_x, double eye_y, double body_x, double body_y, const std::string &caption,
           const std::string &kana, int frames = 6)
{
    const voice_screen::Avatar from = g_avatar;
    for (int i = 1; i <= frames; ++i) {
        const double t = 0.5 - 0.5 * std::cos(M_PI * i / frames);
        g_avatar.eye_x = from.eye_x + (eye_x - from.eye_x) * t;
        g_avatar.eye_y = from.eye_y + (eye_y - from.eye_y) * t;
        g_avatar.body_x = from.body_x + (body_x - from.body_x) * t;
        g_avatar.body_y = from.body_y + (body_y - from.body_y) * t;
        frame(caption, kana, 10);
    }
}

int brightness_percent() { return std::lround((M5.Display.getBrightness() - 8) * 100.0 / 247); }
void set_brightness(int percent)
{
    // Level 0 stays readable: the AMOLED would otherwise go black and look switched off.
    M5.Display.setBrightness(8 + std::clamp(percent, 0, 100) * 247 / 100);
}

// One call of schema v1. Returns false for an unknown name.
bool play(const cJSON *call, const std::string &kana)
{
    const char *name = string_of(call, "name");
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(call, "arguments");
    if (!name || !cJSON_IsObject(arguments)) { return false; }
    char caption[96];
    g_avatar.gauge = -1;
    g_avatar.move = characters::Move::rest;
    if (std::strcmp(name, "look") == 0 || std::strcmp(name, "turn") == 0) {
        const Direction *d = direction_of(string_of(arguments, "direction"));
        if (!d) { return false; }
        const double e = extent_of(arguments);
        g_avatar.back = false;
        const bool turn = name[0] == 't';
        const cJSON *degrees = cJSON_GetObjectItemCaseSensitive(arguments, "degrees");
        if (std::strcmp(d->name, "center") == 0) { snprintf(caption, sizeof(caption), "まんなかを見たよ"); }
        else if (cJSON_IsNumber(degrees)) {
            snprintf(caption, sizeof(caption), turn ? "%sに%dどまわったよ" : "%sを%dど見たよ", d->word, degrees->valueint);
        } else { snprintf(caption, sizeof(caption), turn ? "%sにまわったよ" : "%sを見たよ", d->word); }
        // look moves the eyes only; turn moves the whole face as well.
        glide(d->x * e, d->y * e, turn ? d->x * e * 48 : g_avatar.body_x, turn ? d->y * e * 40 : g_avatar.body_y,
              caption, kana);
    } else if (std::strcmp(name, "nod") == 0 || std::strcmp(name, "shake") == 0) {
        const int count = std::clamp(number_of(arguments, "count", 1), 1, 5);
        const bool nod = name[0] == 'n';
        snprintf(caption, sizeof(caption), nod ? "%dかいうなずいたよ" : "%dかいくびをふったよ", count);
        const double x = g_avatar.body_x, y = g_avatar.body_y, ex = g_avatar.eye_x, ey = g_avatar.eye_y;
        for (int i = 0; i < count; ++i) {
            if (nod) { glide(ex, 0.6, x, y + 26, caption, kana, 4); glide(ex, ey, x, y, caption, kana, 4); }
            else {
                glide(-0.6, ey, x - 30, y, caption, kana, 3); glide(0.6, ey, x + 30, y, caption, kana, 5);
                glide(ex, ey, x, y, caption, kana, 3);
            }
        }
    } else if (std::strcmp(name, "bow") == 0) {
        snprintf(caption, sizeof(caption), "ぺこり");
        const double x = g_avatar.body_x, y = g_avatar.body_y, ex = g_avatar.eye_x, ey = g_avatar.eye_y;
        g_avatar.eyes_closed = true;
        glide(ex, 0.8, x, y + 50, caption, kana, 8);
        frame(caption, kana, 500);
        glide(ex, ey, x, y, caption, kana, 8);
        g_avatar.eyes_closed = false;
    } else if (std::strcmp(name, "set_expression") == 0) {
        const char *value = string_of(arguments, "expression");
        const Face *face = nullptr;
        for (const auto &f : kFaces) { if (value && std::strcmp(f.name, value) == 0) { face = &f; } }
        if (!face) { return false; }
        g_avatar.expression = face->expression;
        g_avatar.back = false;
        snprintf(caption, sizeof(caption), "%s", face->word);
        // A small hop so the change of face is noticed.
        const double y = g_avatar.body_y;
        glide(g_avatar.eye_x, g_avatar.eye_y, g_avatar.body_x, y - 14, caption, kana, 3);
        glide(g_avatar.eye_x, g_avatar.eye_y, g_avatar.body_x, y, caption, kana, 3);
    } else if (std::strcmp(name, "set_led") == 0) {
        const char *value = string_of(arguments, "color");
        if (!value) { return false; }
        if (std::strcmp(value, "off") == 0) { g_avatar.ring = false; snprintf(caption, sizeof(caption), "ライトをけしたよ"); }
        else {
            const Color *color = nullptr;
            for (const auto &c : kColors) { if (std::strcmp(c.name, value) == 0) { color = &c; } }
            if (!color) { return false; }
            g_avatar.ring = true; g_avatar.ring_rgb = color->rgb;
            snprintf(caption, sizeof(caption), "ライトを%sにしたよ", color->word);
        }
        frame(caption, kana, 0);
    } else if (std::strcmp(name, "set_volume") == 0 || std::strcmp(name, "adjust_volume") == 0) {
        if (name[0] == 's') { g_volume = std::clamp(number_of(arguments, "level", g_volume), 0, 100); }
        else {
            const char *direction = string_of(arguments, "direction");
            const int step = step_of(arguments);
            g_volume = std::clamp(g_volume + (direction && std::strcmp(direction, "down") == 0 ? -step : step), 0, 100);
        }
        M5.Speaker.setVolume(g_volume * 255 / 100);
        snprintf(caption, sizeof(caption), "おとのおおきさ %d", g_volume);
        g_avatar.gauge = g_volume; frame(caption, kana, 0);
    } else if (std::strcmp(name, "set_brightness") == 0 || std::strcmp(name, "adjust_brightness") == 0) {
        int level = brightness_percent();
        if (name[0] == 's') { level = std::clamp(number_of(arguments, "level", level), 0, 100); }
        else {
            const char *direction = string_of(arguments, "direction");
            const int step = step_of(arguments);
            level = std::clamp(level + (direction && std::strcmp(direction, "down") == 0 ? -step : step), 0, 100);
        }
        set_brightness(level);
        snprintf(caption, sizeof(caption), "あかるさ %d", level);
        g_avatar.gauge = level; frame(caption, kana, 0);
    } else {
        return false;
    }
    printf("ACTION_PLAYED name=%s caption=%s\n", name, caption);
    if (g_speaker) { g_speaker(caption); }
    return true;
}
}  // namespace

void set_speaker(Speaker speaker) { g_speaker = speaker;M5.Speaker.setVolume(g_volume*255/100); }

std::string json_text(const int *ids, int count)
{
    std::string text;
    for (int i = 0; i < count; ++i) {
        if (ids[i] == kEos) { break; }
        if (ids[i] == kBos || ids[i] == kPad || ids[i] < 0 || ids[i] >= kPieceCount) { continue; }
        text += kPieces[ids[i]];
    }
    return text;
}

void idle(const char *caption) { voice_screen::avatar(g_avatar, caption, ""); }

void reset()
{
    g_avatar = voice_screen::Avatar{};
    idle("話しかけてね");
}

bool perform_extra(const std::string &kana)
{
    using C=characters::Command;
    const C command=characters::parse(kana);
    if(command==C::none) return false;
    g_avatar.gauge=-1;
    const char *caption="話しかけてね";
    if(command==C::jump || command==C::play) {
        g_avatar.move=command==C::jump?characters::Move::jump:characters::Move::play;
        ++g_avatar.movement; g_avatar.back=false;
        caption=command==C::jump?"ジャンプ！":"あそぼう！";
    } else if(command==C::back) { g_avatar.back=true; g_avatar.move=characters::Move::rest; caption="うしろをむいたよ"; }
    else if(command==C::gather) {
        voice_screen::gather_characters();caption="みんなあつまったよ";
    } else {
        g_avatar.background=int(command)-int(C::black);
        caption="背景をかえたよ";
    }
    frame(caption,kana,0);
    printf("CHARACTER_COMMAND kind=%d kana=%s\n",int(command),kana.c_str());
    if(g_speaker) g_speaker(caption);
    printf("ACTION_DONE played=1 source=kana\n");
    return true;
}

bool perform_talk(const std::string &kana)
{
    const char *caption=talk::parse(kana);
    if(!caption) return false;
    g_avatar.expression=Expression::happy;
    g_avatar.back=false;g_avatar.move=characters::Move::rest;g_avatar.gauge=-1;
    frame(caption,kana,0);
    printf("TALK_REPLY kana=%s caption=%s expression=happy selected=%u\n",kana.c_str(),caption,voice_screen::selected_character());
    if(g_speaker) g_speaker(caption);
    printf("ACTION_DONE played=1 source=talk\n");
    return true;
}

int perform(const std::string &json, const std::string &kana, bool accepted)
{
    g_avatar.gauge = -1;
    if (!accepted) {
        // Not a request (p_act under the threshold): a puzzled look, nothing else changes.
        const Expression before = g_avatar.expression;
        g_avatar.expression = Expression::doubt;
        frame(kana.empty() ? "ききとれなかったよ" : "それはできないよ", kana, 1200);
        g_avatar.expression = before;
        frame(kana.empty() ? "ききとれなかったよ" : "それはできないよ", kana, 0);
        return 0;
    }
    cJSON *root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        printf("ERROR action_json %s\n", json.c_str());
        frame("うまくよめなかったよ", kana, 0);
        return -1;
    }
    int played = 0;
    const cJSON *call = nullptr;
    cJSON_ArrayForEach(call, root) {
        if (played) { vTaskDelay(pdMS_TO_TICKS(400)); }
        played += play(call, kana) ? 1 : 0;
    }
    cJSON_Delete(root);
    if (played == 0) { frame("しないでおくね", kana, 0); }  // "[]": e.g. a cancelled or negated request
    return played;
}
}  // namespace action
