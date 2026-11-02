#pragma once
#include "ir_codes.h"
#include "home.h"

namespace ir_home {
void receive_diagnostic();
void receive_session();
void skip_session();
void session_result();
bool session_active();
bool start_edge_scope(bool tx_low);
bool edge_scope_active();
bool start_monitor();
void stop_monitor();
bool monitor_active();
void monitor_probe();
void loopback_probe();
void loopback_trials(bool reuse, bool tx_low);
bool send_tv(ir_codes::TvKey key, bool diagnostic = false);
bool send_light(ir_codes::LightKey key);
void compare_tv_loopback();
bool send_aircon(bool on, int mode, int temperature);
enum class Learned { terrestrial, satellite, light_on, light_off, light_orange, light_night, light_white };
inline bool learned_slot(const home::Command &command, Learned &slot) {
    using K = home::Command::Kind;
    if (command.device == 0 && command.kind == K::tv_band && command.value >= 0 && command.value <= 2) {
        slot = command.value == 0 ? Learned::terrestrial : Learned::satellite; return true;
    }
    if (command.device == 2 && command.kind == K::power) { slot = command.value ? Learned::light_on : Learned::light_off; return true; }
    if (command.device == 2 && command.kind == K::light_mode && command.value >= 0 && command.value <= 2) {
        slot = static_cast<Learned>(int(Learned::light_orange) + command.value); return true;
    }
    return false;
}
bool save_session(Learned slot);
bool has_learned(Learned slot);
bool send_learned(Learned slot);
void transmit_diagnostic();
}
