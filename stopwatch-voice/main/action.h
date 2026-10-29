#pragma once
// KoeAct-JP Action JSON on the StopWatch: the decoder's piece ids become the JSON of the 11-tool
// schema v1, and each call moves the on-screen character (the board has no motors or LED).
#include <string>

namespace action {
using Speaker = void (*)(const char *caption);
void set_speaker(Speaker speaker);
// KoeAct-JP config.json "threshold": below this p_act the utterance is not a request.
constexpr float kThreshold = 0.97f;

// The JSON text of generated piece ids, up to </s>.
std::string json_text(const int *ids, int count);

// Show the character at rest, with an optional caption.
void idle(const char *caption);
// Back to the start: neutral face, centred, light off, gauge hidden (brightness is kept).
void reset();
// Play the Action JSON (an array of calls) on the character; `kana` is what the model heard.
// Returns the number of calls played, 0 for "[]" or a rejected utterance, -1 for unreadable JSON.
int perform(const std::string &json, const std::string &kana, bool accepted);
// Extra kana commands absent from KoeAct's fixed tool schema (like home::parse).
bool perform_extra(const std::string &kana);
bool perform_talk(const std::string &kana);
}
