#pragma once
#include "ir_codes.h"

namespace ir_home {
void receive_diagnostic();
void receive_session();
void skip_session();
void session_result();
bool session_active();
bool start_edge_scope(bool tx_low);
bool edge_scope_active();
void loopback_probe();
void loopback_trials(bool reuse, bool tx_low);
bool send_tv(ir_codes::TvKey key);
bool send_aircon_power(bool on);
enum class Learned { terrestrial, satellite, light_on, light_off, light_orange, light_night, light_white };
bool save_session(Learned slot);
bool has_learned(Learned slot);
bool send_learned(Learned slot);
void transmit_diagnostic();
}
