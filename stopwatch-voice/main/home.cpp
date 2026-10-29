#include "home.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "voice_screen.h"

namespace home {
namespace {
Speaker g_speaker = nullptr;
using Kind = Command::Kind;

// The panel's state (only on the screen: there are no real appliances behind it).
struct State {
    bool tv = false, aircon = false, light = false;
    int band = 0, volume = 20;   // TV: 0 地上波 1 BS 2 CS
    int mode = 0, degrees = 26;  // air conditioner: 0 cool 1 heat 2 dry
} g_state;
int g_focus = 0;

const char *const kBands[] = {"地上波", "BS", "CS"};
const char *const kModes[] = {"冷房", "暖房", "ドライ"};
const char *const kDeviceIds[] = {"tv", "aircon", "light"};
const char *const kModeIds[] = {"cool", "heat", "dry"};
const char *const kBandIds[] = {"terrestrial", "bs", "cs"};
constexpr uint32_t kModeColours[] = {0x5EB1EF, 0xF0883E, 0x46A758};

bool has(const std::string &text, std::initializer_list<const char *> words)
{
    for (const char *word : words) { if (text.find(word) != std::string::npos) { return true; } }
    return false;
}
bool ends_with(const std::string &text, const char *word)
{
    const size_t n = std::strlen(word);
    return text.size() >= n && text.compare(text.size() - n, n, word) == 0;
}
bool at(const std::string &text, size_t pos, const char *word)
{
    return text.compare(pos, std::strlen(word), word) == 0;
}

// A kana number read greedily at `pos` ("にじゅーはち" -> 28, "ひゃくご" -> 105); returns the
// value and moves `pos` past it, or -1 when no number starts there.
int number_at(const std::string &text, size_t &pos)
{
    struct Word { const char *kana; int value; };
    static const Word kMultipliers[] = {{"じゅー", 10}, {"じゅう", 10}, {"じゅっ", 10}, {"じゅ", 10},
                                        {"ひゃく", 100}, {"びゃく", 100}, {"ぴゃく", 100}};
    static const Word kDigits[] = {{"きゅー", 9}, {"きゅう", 9}, {"いち", 1}, {"いっ", 1}, {"さん", 3},
                                   {"よん", 4}, {"ろく", 6}, {"ろっ", 6}, {"なな", 7}, {"しち", 7},
                                   {"はち", 8}, {"はっ", 8}, {"れい", 0}, {"ぜろ", 0}, {"に", 2},
                                   {"よ", 4}, {"し", 4}, {"ご", 5}, {"く", 9}};
    size_t p = pos;
    int total = 0, digit = -1;
    bool any = false;
    while (p < text.size()) {
        bool matched = false;
        for (const Word &m : kMultipliers) {
            if (at(text, p, m.kana)) {
                total += (digit > 0 ? digit : 1) * m.value;
                digit = -1; p += std::strlen(m.kana); matched = any = true;
                break;
            }
        }
        if (matched) { continue; }
        if (digit < 0) {
            for (const Word &d : kDigits) {
                if (at(text, p, d.kana)) { digit = d.value; p += std::strlen(d.kana); matched = any = true; break; }
            }
        }
        if (!matched) { break; }
    }
    if (!any) { return -1; }
    pos = p;
    return total + std::max(digit, 0);
}

// The first kana number immediately followed by one of `suffixes`, or -1.
int number_before(const std::string &text, std::initializer_list<const char *> suffixes)
{
    for (size_t start = 0; start < text.size();) {
        size_t end = start;
        const int value = number_at(text, end);
        if (value >= 0) {
            for (const char *suffix : suffixes) { if (at(text, end, suffix)) { return value; } }
        }
        const unsigned char lead = text[start];
        start += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    }
    return -1;
}

// "BS" / "CS" read as kana come out loosely ("びーえす", "りえす", "しんえーす"): decide by the
// one or two kana before "えす"/"えーす". 1 BS, 2 CS, -1 none.
int satellite_band(const std::string &text)
{
    for (const char *es : {"えーす", "えす"}) {
        for (size_t pos = text.find(es); pos != std::string::npos; pos = text.find(es, pos + 1)) {
            const std::string before = text.substr(pos >= 6 ? pos - 6 : 0, pos >= 6 ? 6 : pos);
            if (has(before, {"し"})) { return 2; }
            if (has(before, {"び", "ぴ", "り", "ぶ", "べ"})) { return 1; }
        }
    }
    return -1;
}

void draw(const char *caption, const std::string &kana)
{
    const State &s = g_state;
    char tv[48], aircon[48];
    snprintf(tv, sizeof(tv), "%s %d", kBands[s.band], s.volume);
    snprintf(aircon, sizeof(aircon), "%s%d℃", kModes[s.mode], s.degrees);  // "ドライ26℃" fits the tile
    const voice_screen::Tile tiles[] = {
        {voice_screen::Appliance::tv, "テレビ", s.tv ? tv : "オフ", s.tv, 0x5EB1EF},
        {voice_screen::Appliance::aircon, "エアコン", s.aircon ? aircon : "オフ", s.aircon, kModeColours[s.mode]},
        {voice_screen::Appliance::light, "でんき", s.light ? "オン" : "オフ", s.light, 0xF2C14E},
    };
    voice_screen::home_panel(tiles, 3, g_focus, caption, kana.empty() ? kana : "「" + kana + "」");
}
}  // namespace

void set_speaker(Speaker speaker) { g_speaker = speaker; }

Command parse(const std::string &kana)
{
    Command c;
    if (has(kana, {"ないで", "なくて"})) { return c; }  // "テレビを消さないで" asks for nothing
    if (has(kana, {"ちじょーは", "ちじょは", "ちじょーわ", "ちじょわ"})) { c = {Kind::tv_band, 0, 0}; }
    else if (satellite_band(kana) > 0 && (has(kana, {"にして", "にかえて", "にきりかえて"}) || has(kana, {"てれび"}))) {
        c = {Kind::tv_band, 0, satellite_band(kana)};
    }
    else if (has(kana, {"れいぼー", "れーぼー", "れいぼう", "れぼー"})) { c = {Kind::aircon_mode, 1, 0}; }
    else if (has(kana, {"だんぼー", "だんぼう", "だんぼ"})) { c = {Kind::aircon_mode, 1, 1}; }
    else if (has(kana, {"どらい", "じょしつ"})) { c = {Kind::aircon_mode, 1, 2}; }
    else if (has(kana, {"おんど", "えあこん"}) && number_before(kana, {"ど"}) >= 0) {
        c = {Kind::temperature, 1, number_before(kana, {"ど"})};
    } else if (has(kana, {"おんりょー", "おんりょう", "ぼりゅーむ"}) && has(kana, {"あげ", "さげ"})) {
        const int step = number_before(kana, {"あげ", "さげ", "つあげ", "つさげ", "だけあげ", "だけさげ"});
        // A bare "音量を上げて" stays with KoeAct's adjust_volume unless the TV is named.
        if (step > 0 || has(kana, {"てれび"})) {
            const int amount = step > 0 ? step : 1;
            c = {Kind::tv_volume, 0, has(kana, {"さげ"}) ? -amount : amount};
        }
    } else {
        const int device = has(kana, {"てれび"}) ? 0 : has(kana, {"えあこん", "くーらー"}) ? 1
                         : has(kana, {"でんき", "あかり", "しょーめい"}) ? 2 : -1;
        const bool on = has(kana, {"つけ", "おんにし", "おんして"}) || ends_with(kana, "おん");
        const bool off = has(kana, {"けし", "けせ", "おふ", "きって", "とめて"});  // not "けて": "つけて"
        if (device >= 0 && on != off) { c = {Kind::power, device, on ? 1 : 0}; }
    }
    return c;
}

bool perform(const Command &command, const std::string &kana)
{
    State &s = g_state;
    char caption[96], detail[96];
    switch (command.kind) {
    case Kind::none:
        return false;
    case Kind::power: {
        static const char *const kOn[] = {"テレビをつけたよ", "エアコンをつけたよ", "でんきをつけたよ"};
        static const char *const kOff[] = {"テレビをけしたよ", "エアコンをけしたよ", "でんきをけしたよ"};
        bool *power[] = {&s.tv, &s.aircon, &s.light};
        *power[command.device] = command.value != 0;
        snprintf(caption, sizeof(caption), "%s", command.value ? kOn[command.device] : kOff[command.device]);
        snprintf(detail, sizeof(detail), "\"command\":\"power\",\"value\":\"%s\"", command.value ? "on" : "off");
        break;
    }
    case Kind::aircon_mode:
        s.aircon = true; s.mode = command.value;
        snprintf(caption, sizeof(caption), "%sにしたよ", kModes[s.mode]);
        snprintf(detail, sizeof(detail), "\"command\":\"mode\",\"value\":\"%s\"", kModeIds[s.mode]);
        break;
    case Kind::temperature:
        if (command.value < 16 || command.value > 31) {
            snprintf(caption, sizeof(caption), "%d℃はむりだよ", command.value);
            snprintf(detail, sizeof(detail), "\"command\":\"temperature\",\"value\":%d,\"refused\":true", command.value);
        } else {
            s.aircon = true; s.degrees = command.value;
            snprintf(caption, sizeof(caption), "%d℃にしたよ", s.degrees);
            snprintf(detail, sizeof(detail), "\"command\":\"temperature\",\"value\":%d", s.degrees);
        }
        break;
    case Kind::tv_volume:
        s.tv = true; s.volume = std::clamp(s.volume + command.value, 0, 100);
        snprintf(caption, sizeof(caption), command.value > 0 ? "おとを%dあげたよ" : "おとを%dさげたよ",
                 std::abs(command.value));
        snprintf(detail, sizeof(detail), "\"command\":\"volume\",\"step\":%d,\"value\":%d", command.value, s.volume);
        break;
    case Kind::tv_band:
        s.tv = true; s.band = command.value;
        snprintf(caption, sizeof(caption), "%sにしたよ", kBands[s.band]);
        snprintf(detail, sizeof(detail), "\"command\":\"band\",\"value\":\"%s\"", kBandIds[s.band]);
        break;
    }
    g_focus = command.device;
    draw(caption, kana);
    printf("HOME_ACTION {\"device\":\"%s\",%s} caption=%s\n", kDeviceIds[command.device], detail, caption);
    fflush(stdout);
    if (g_speaker) { g_speaker(caption); }
    return true;
}

void show(const char *caption) { draw(caption, ""); }
}  // namespace home
