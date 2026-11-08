#pragma once
#include "microlink.h"
#include <atomic>
#include <cstdint>
#include <string>
namespace network_http {
enum class Error { none, cancelled, timeout, connect, refused, send, receive, invalid_http, http_status };
enum class Phase : uint8_t { connecting, sending, processing, cleanup, cancelled_cleanup };
struct Response { Error error; unsigned status; std::string body; bool cleanup_only = false; int64_t cleanup_deadline_us = 0; };
Response request(microlink_t* ml, uint32_t address, uint16_t port, const char* method,
                 const std::string& path, const std::string& content_type, const std::string& body,
                 std::atomic<bool>& cancelled, int64_t connect_deadline_us, int64_t request_duration_us,
                 std::atomic<uint32_t>* visible_deadline_ms = nullptr,
                 std::atomic<Phase>* phase = nullptr, std::atomic<bool>* link_state = nullptr,
                 bool recover_ocr = false, int64_t absolute_limit_us = INT64_MAX,
                 std::atomic<int64_t>* cancelled_at_us = nullptr);
}
