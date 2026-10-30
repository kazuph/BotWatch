#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "label_raster.h"
#include "pitch_detector.h"
#include "ear_game.h"
#include "character_motion.h"
#include "character_touch.h"

namespace voice_screen {
bool begin();
bool begin_overlay();
void end_overlay();
void discard_overlay();
int app_count();
void app_radio(const char *state,int volume);
void app_menu(int rotation,const char *message=nullptr);
void app_timer(const char *title,const std::string &value,const std::string &detail,const char *left,const char *right);
void app_clock(const std::string &date,const std::string &time,const char *field,bool editing);
void app_spectrum(std::span<const float> power, double peakHz);
void app_ear(const ear_game::Question &question,int feedback,int volume);
void app_tuner(const tuner::PitchResult &pitch, int calibration, bool japanese);
int app_at(int rotation, int x, int y);
void show(const char *status, const std::string &text);
void waiting();
void recording();
void result(const std::string &text);
void confirmation(const std::string &text);
enum class Action { none, retry, print, back };
void confirmation(const std::string &text, const uint8_t *bits, int rows, double paper_mm);
void failure(const std::string &reason);
Action action_at(int x, int y);
bool scroll_area(int x, int y);
void roles(const char *left, const char *right);
void recording_progress(double remaining, double dbfs, double total = 15);
void label_preview(const uint8_t *bits, int rows);
void dump_pixels(void (*write_hex)(const uint8_t *, size_t));
void scroll(int rows);
int first_row();
int line_height();
void dump(void (*write_hex)(const uint8_t *, size_t));
// A selectable list of label previews: the recognized text's styles, the preset
// people, or a preset's styles. names[i] titles labels[i].
void previews(const std::string &text, const label_raster::Label *labels, const char *const *names, int count,
              int selected, bool reset_scroll, const char *heading, const char *right);
void printing(bool started);
void success(int battery_percent);
void volume_warning(bool warning);

// Action mode (KoeAct): the on-screen character that the spoken Actions move.
enum class Expression { neutral, happy, sad, surprised, angry, sleepy, doubt };
struct Avatar {
    Expression expression = Expression::neutral;
    double eye_x = 0, eye_y = 0;    // gaze, -1..1
    double body_x = 0, body_y = 0;  // face offset in pixels
    bool eyes_closed = false;
    bool ring = false;              // the "light" around the bezel
    uint32_t ring_rgb = 0;
    int gauge = -1;                 // 0..100 shown under the caption, -1 for none
    bool back = false;
    characters::Move move = characters::Move::rest;
    unsigned movement = 0;          // a new command restarts its timeline
    int background = 0;             // black, field, room, beach (approved plan)
};
// caption: what the character did; kana: the line under it (what the model heard, or a hint).
void avatar(const Avatar &avatar, const std::string &caption, const std::string &kana);
bool start_characters();
void animation_stats();
void restore_character(unsigned selected);
unsigned selected_character();
void gather_characters();
bool character_touch(bool down,int x,int y,uint32_t ms);
bool diagnostic_touch(unsigned kind,int x,int y,int dx,int dy,uint16_t duration);
void font_audit(bool enabled);
void mode_switching(const char *next_mode);

// Voice printing (kana mode entered from the action mode by voice).
void voice_prompt(const char *line1, const char *line2, const std::string &heard);
void voice_confirm(const uint8_t *bits, int rows, const std::string &text);

// Action mode's home panel (TV, air conditioner, room light). Text comes from home.cpp so the
// font builder sees it there.
enum class Appliance { tv, aircon, light };
struct Tile {
    Appliance kind;
    const char *name;   // 24px label under the drawing
    std::string status; // 24px, e.g. "BS" or "冷房 26℃"
    bool on;
    uint32_t rgb;       // colour of the lit part
};
void home_panel(const Tile *tiles, int count, int focus, const std::string &caption, const std::string &kana);
}
