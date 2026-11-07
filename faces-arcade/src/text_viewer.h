#pragma once
#include <string>
#include "network_job.h"
namespace text_viewer {
void show(const std::string& text, network_job::Kind kind);
void tick();
void clear();
}
