#pragma once
namespace tts {
bool begin();
bool is_ready();
bool say(const char *caption);
void diagnostic(bool muted);
void diagnostic_phrases(void (*speak)(const char *));
}
