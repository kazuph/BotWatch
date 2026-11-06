#pragma once
#include <atomic>
#include <cstdint>
#include "network_settings.h"
namespace network_wifi {
enum class Result { connected, cancelled, timeout, disconnected, error };
Result connect(const network_settings::Credentials& credentials, std::atomic<bool>& cancelled, int64_t deadline_us);
void stop();
}
