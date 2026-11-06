#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace network_payload {
struct Multipart { std::string content_type; std::string body; };
bool ocr(const uint8_t* jpeg, size_t size, Multipart& out);
bool describe(const uint8_t* jpeg, size_t size, const char* model, std::string& out);
}
