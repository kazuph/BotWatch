#pragma once
namespace tts {
void restore_diagnostic();
bool begin();
bool is_ready();
bool is_muted();
bool say(const char *caption);
void diagnostic(bool muted);
void diagnostic_phrases(void (*speak)(const char *));
}
