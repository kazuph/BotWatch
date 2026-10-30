#pragma once
// Home appliance commands (test): KoeAct-JP's Action schema is fixed to 11 tools, so these are read
// from the kana the same model hears (its CTC output) with simple rules, and shown on a panel.
// Nothing is sent to real appliances; each command is printed as "HOME_ACTION <json>".
#include <string>

namespace home {
using Speaker = void (*)(const char *caption);
void set_speaker(Speaker speaker);
struct Command {
    enum class Kind { none, power, aircon_mode, temperature, tv_volume, tv_band } kind = Kind::none;
    int device = 0;  // 0 TV, 1 air conditioner, 2 light
    int value = 0;   // power 0/1, mode 0 cool 1 heat 2 dry, degrees, volume step (+/-), band 0..2
};

// The command in `kana`, or Kind::none. A negated request ("…ないで") is none.
Command parse(const std::string &kana);
// Apply it, draw the panel and print HOME_ACTION. Returns false for Kind::none.
bool perform(const Command &command, const std::string &kana);
// The panel as it stands (B button returns to the character).
void show(const char *caption);
void redraw(const std::string &caption,const std::string &kana);
}
