#pragma once
#include "network_job.h"
namespace network_ui {
bool begin(network_job::Kind kind);
bool active();
void tick();
void cancel();
}
