#include "http_response.h"

HttpResponse::HttpResponse() {
    http_parser_init(&parser_, HTTP_RESPONSE);
    parser_.data = this;
    http_parser_settings_init(&settings_);
    settings_.on_body = body_callback;
    settings_.on_message_complete = complete_callback;
}
int HttpResponse::body_callback(http_parser* parser, const char* bytes, size_t size) {
    static_cast<HttpResponse*>(parser->data)->body_.append(bytes, size);
    return 0;
}
int HttpResponse::complete_callback(http_parser* parser) {
    static_cast<HttpResponse*>(parser->data)->complete_ = true;
    return 0;
}
bool HttpResponse::feed(const char* bytes, size_t size) {
    if (!bytes || complete_) return false;
    return http_parser_execute(&parser_, &settings_, bytes, size) == size &&
           HTTP_PARSER_ERRNO(&parser_) == HPE_OK;
}
bool HttpResponse::finish_eof() {
    http_parser_execute(&parser_, &settings_, "", 0);
    return complete_ && HTTP_PARSER_ERRNO(&parser_) == HPE_OK;
}
