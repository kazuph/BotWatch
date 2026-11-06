#pragma once
#include "http_parser.h"
#include <cstddef>
#include <string>

class HttpResponse {
public:
    HttpResponse();
    bool feed(const char* bytes, size_t size);
    bool finish_eof();
    bool complete() const { return complete_; }
    unsigned status() const { return parser_.status_code; }
    const std::string& body() const { return body_; }
private:
    static int body_callback(http_parser* parser, const char* bytes, size_t size);
    static int complete_callback(http_parser* parser);
    http_parser parser_{};
    http_parser_settings settings_{};
    std::string body_;
    bool complete_ = false;
};
