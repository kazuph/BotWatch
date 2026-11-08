#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "network_http.h"
namespace network_job {
enum class Kind { ocr, describe };
struct Result { uint64_t photo; bool ok; std::string text; };
// Image ownership transfers only if start returns true. Host is the configured 3090 address.
bool start(Kind kind, uint64_t photo, uint8_t* jpeg, size_t size,
           const char* host, uint16_t port, const char* model);
void cancel();
bool busy();
bool expired();
bool take(Result& result);
network_http::Phase phase();
bool link_connected();
}
