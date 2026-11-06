#pragma once
#include "microlink.h"
#include <atomic>
#include <cstdint>
#include <string>
namespace network_http {
enum class Error { none, cancelled, timeout, connect, refused, send, receive, invalid_http, http_status };
enum class Phase : uint8_t { connecting, sending, processing };
struct Response { Error error; unsigned status; std::string body; };
Response request(microlink_t* ml, uint32_t address, uint16_t port, const char* method,
                 const std::string& path, const std::string& content_type, const std::string& body,
                 std::atomic<bool>& cancelled, int64_t connect_deadline_us, int64_t request_duration_us,
                 std::atomic<uint32_t>* visible_deadline_ms = nullptr,
                 std::atomic<Phase>* phase = nullptr, std::atomic<bool>* link_state = nullptr);
}
