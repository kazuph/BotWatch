#include "home.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "voice_screen.h"

namespace home {
namespace {
Speaker g_speaker = nullptr;
Sender g_sender = nullptr;
using Kind = Command::Kind;

// The panel's state (only on the screen: there are no real appliances behind it).
struct State {
    bool tv = false, aircon = false, light = false;
    int band = 0, volume = 0;   // TV: 0 地上波 1 BS 2 CS; volume is a relative step
    int mode = 0, degrees = 27;  // captured air conditioner template
} g_state;
int g_focus = 0;

const char *const kBands[] = {"地上波", "BS", "CS"};
const char *const kModes[] = {"冷房", "暖房", "ドライ"};
const char *const kDeviceIds[] = {"tv", "aircon", "light"};
const char *const kModeIds[] = {"cool", "heat", "dry"};
const char *const kBandIds[] = {"terrestrial", "bs", "cs"};
constexpr uint32_t kModeColours[] = {0x5EB1EF, 0xF0883E, 0x46A758};

void draw(const char *caption, const std::string &kana,bool quote=true)
{
    const State &s = g_state;
    char tv[48], aircon[48];
    snprintf(tv, sizeof(tv), "電源切替 %d", s.volume);
    snprintf(aircon, sizeof(aircon), "%s%d℃", kModes[s.mode], s.degrees);  // "ドライ26℃" fits the tile
    const voice_screen::Tile tiles[] = {
        {voice_screen::Appliance::tv, "テレビ", tv, true, 0x5EB1EF},
        {voice_screen::Appliance::aircon, "エアコン", s.aircon ? aircon : "オフ", s.aircon, kModeColours[s.mode]},
        {voice_screen::Appliance::light, "でんき", s.light ? "オン" : "オフ", s.light, 0xF2C14E},
    };
    voice_screen::home_panel(tiles, 3, g_focus, caption, !quote || kana.empty() ? kana : "「" + kana + "」");
}
}  // namespace

void set_speaker(Speaker speaker) { g_speaker = speaker; }
void set_sender(Sender sender) { g_sender = sender; }

bool perform(const Command &command, const std::string &kana, bool *sent)
{
    if (sent) *sent = false;
    State &s = g_state;
    char caption[96], detail[96];
    if (command.kind == Kind::none) return false;
    if (command.device < 0 || command.device > 2) return false;
    if (command.kind == Kind::temperature && (command.value < 16 || command.value > 31)) {
        snprintf(caption, sizeof(caption), "%d℃はむりだよ", command.value);
        g_focus = command.device; draw(caption, kana);
        printf("HOME_ACTION {\"device\":\"aircon\",\"command\":\"temperature\",\"value\":%d,\"refused\":true} caption=%s\n", command.value, caption);
        fflush(stdout); if (g_speaker) g_speaker(caption); return true;
    }
    if ((command.kind == Kind::tv_volume && (command.value == 0 || std::abs(command.value) > 10)) ||
        (command.kind == Kind::tv_channel && (command.value < 1 || command.value > 9)) ||
        (command.kind == Kind::tv_channel_step && command.value != 1 && command.value != -1)) {
        g_focus = command.device; draw("できなかったよ", kana);
        printf("HOME_ACTION {\"device\":\"tv\",\"refused\":true,\"reason\":\"out_of_range\"} caption=できなかったよ\n");
        fflush(stdout); if (g_speaker) g_speaker("できなかったよ"); return true;
    }
    const SendResult result = g_sender ? g_sender(command) : SendResult{false, 0, "sender_unavailable"};
    if (!result.ok) {
        if (command.kind == Kind::tv_volume && result.sent > 0)
            snprintf(caption, sizeof(caption), "音量%d段で失敗", result.sent);
        else snprintf(caption, sizeof(caption), "できなかったよ");
        g_focus = command.device; draw(caption, kana);
        printf("HOME_ACTION {\"device\":\"%s\",\"sent\":%d,\"success\":false,\"reason\":\"%s\"} caption=できなかったよ\n",
               kDeviceIds[command.device], result.sent, result.reason ? result.reason : "send_failed");
        fflush(stdout); if (g_speaker) g_speaker("できなかったよ"); return true;
    }
    if (sent) *sent = true;
    switch (command.kind) {
    case Kind::none:
        return false;
    case Kind::power: {
        static const char *const kOn[] = {"テレビをつけたよ", "エアコンをつけたよ", "でんきをつけたよ"};
        static const char *const kOff[] = {"テレビをけしたよ", "エアコンをけしたよ", "でんきをけしたよ"};
        bool *power[] = {&s.tv, &s.aircon, &s.light};
        if (command.device != 0) *power[command.device] = command.value != 0;
        snprintf(caption, sizeof(caption), "%s", command.device == 0 ? "テレビの電源を切り替えたよ" : command.value ? kOn[command.device] : kOff[command.device]);
        snprintf(detail, sizeof(detail), "\"command\":\"power\",\"value\":\"%s\"", command.device == 0 ? "toggle" : command.value ? "on" : "off");
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
        s.volume = command.value;
        snprintf(caption, sizeof(caption), command.value > 0 ? "おとを%dあげたよ" : "おとを%dさげたよ",
                 std::abs(command.value));
        snprintf(detail, sizeof(detail), "\"command\":\"volume\",\"step\":%d,\"value\":%d", command.value, s.volume);
        break;
    case Kind::tv_band:
        if (command.value == 0) s.band = 0;
        snprintf(caption, sizeof(caption), "%s", command.value == 0 ? "地上波にしたよ" : "BS/CSを切り替えたよ");
        snprintf(detail, sizeof(detail), "\"command\":\"band\",\"value\":\"%s\"", command.value == 0 ? kBandIds[0] : "satellite_toggle");
        break;
    case Kind::tv_channel:
        snprintf(caption, sizeof(caption), "チャンネル%dにしたよ", command.value);
        snprintf(detail, sizeof(detail), "\"command\":\"channel\",\"value\":%d", command.value);
        break;
    case Kind::tv_channel_step:
        snprintf(caption, sizeof(caption), "%s", command.value > 0 ? "チャンネルをあげたよ" : "チャンネルをさげたよ");
        snprintf(detail, sizeof(detail), "\"command\":\"channel_step\",\"step\":%d", command.value);
        break;
    case Kind::light_mode:
        snprintf(caption, sizeof(caption), "でんきを切り替えたよ");
        snprintf(detail, sizeof(detail), "\"command\":\"light_mode\",\"value\":%d", command.value);
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
void redraw(const std::string &caption,const std::string &kana) { draw(caption.c_str(),kana,false); }
}  // namespace home
