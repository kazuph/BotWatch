#include "network_result.h"
#include "cJSON.h"
#include <cctype>
#include <memory>

namespace network_result {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json parse(const std::string& input) {
    return Json(cJSON_ParseWithOpts(input.c_str(), nullptr, true), cJSON_Delete);
}
bool has_text(const char* s) {
    if (!s) return false;
    while (*s) {
        if (!std::isspace(static_cast<unsigned char>(*s))) return true;
        ++s;
    }
    return false;
}
bool valid_job(const char* s) {
    if (!s || !*s) return false;
    for (; *s; ++s) {
        const unsigned char c = *s;
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}
}
Ocr parse_ocr(const std::string& input) {
    Ocr result{Error::malformed_json, {}, {}};
    auto root = parse(input);
    if (!root || !cJSON_IsObject(root.get())) return result;
    const cJSON* job = cJSON_GetObjectItemCaseSensitive(root.get(), "job_id");
    const cJSON* text = cJSON_GetObjectItemCaseSensitive(root.get(), "text");
    if (!cJSON_IsString(job) || !job->valuestring || !*job->valuestring) {
        result.error = Error::missing_job;
        return result;
    }
    if (!valid_job(job->valuestring)) {
        result.error = Error::invalid_job;
        return result;
    }
    result.job_id = job->valuestring;
    if (!cJSON_IsString(text) || !has_text(text->valuestring)) {
        result.error = Error::missing_text;
        return result;
    }
    result.text = text->valuestring;
    result.error = Error::none;
    return result;
}
Description parse_description(const std::string& input) {
    Description result{Error::malformed_json, {}};
    auto root = parse(input);
    if (!root || !cJSON_IsObject(root.get())) return result;
    const cJSON* choices = cJSON_GetObjectItemCaseSensitive(root.get(), "choices");
    const cJSON* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    const cJSON* message = cJSON_IsObject(first) ? cJSON_GetObjectItemCaseSensitive(first, "message") : nullptr;
    const cJSON* content = cJSON_IsObject(message) ? cJSON_GetObjectItemCaseSensitive(message, "content") : nullptr;
    if (!cJSON_IsString(content) || !has_text(content->valuestring)) {
        result.error = Error::missing_text;
        return result;
    }
    result.text = content->valuestring;
    result.error = Error::none;
    return result;
}
}
