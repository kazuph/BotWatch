#pragma once
#include <string>
namespace network_result {
enum class Error { none, malformed_json, missing_job, invalid_job, missing_text };
struct Ocr { Error error; std::string text; std::string job_id; };
struct Description { Error error; std::string text; };
Ocr parse_ocr(const std::string& json);
Description parse_description(const std::string& json);
}
