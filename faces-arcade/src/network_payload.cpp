#include "network_payload.h"
#include "cJSON.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace network_payload {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
}
bool ocr(const uint8_t* jpeg, size_t size, Multipart& out) {
    if (!jpeg || !size) return false;
    std::string image(reinterpret_cast<const char*>(jpeg), size);
    char token[40];
    do {
        snprintf(token, sizeof(token), "faces-%08lx%08lx", static_cast<unsigned long>(esp_random()),
                 static_cast<unsigned long>(esp_random()));
    } while (image.find(token) != std::string::npos);
    const std::string boundary(token);
    out.content_type = "multipart/form-data; boundary=" + boundary;
    out.body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"output_format\"\r\n\r\nmd\r\n";
    out.body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"lite\"\r\n\r\nfalse\r\n";
    out.body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"photo.jpg\"\r\n";
    out.body += "Content-Type: image/jpeg\r\n\r\n";
    out.body.append(image);
    out.body += "\r\n--" + boundary + "--\r\n";
    return true;
}
bool describe(const uint8_t* jpeg, size_t size, const char* model, std::string& out) {
    if (!jpeg || !size || !model || !*model) return false;
    size_t encoded_size = 0;
    if (mbedtls_base64_encode(nullptr, 0, &encoded_size, jpeg, size) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
        return false;
    std::string encoded(encoded_size, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&encoded[0]), encoded.size(), &encoded_size,
                              jpeg, size) != 0) return false;
    encoded.resize(encoded_size);
    const std::string image_url = "data:image/jpeg;base64," + encoded;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "model", model) ||
        !cJSON_AddNumberToObject(root.get(), "max_tokens", 128)) return false;
    cJSON* messages = cJSON_AddArrayToObject(root.get(), "messages");
    cJSON* message = cJSON_CreateObject();
    if (!messages || !message) { cJSON_Delete(message); return false; }
    cJSON_AddItemToArray(messages, message);
    if (!cJSON_AddStringToObject(message, "role", "user")) return false;
    cJSON* content = cJSON_AddArrayToObject(message, "content");
    cJSON* text = cJSON_CreateObject();
    if (!content || !text) { cJSON_Delete(text); return false; }
    cJSON_AddItemToArray(content, text);
    if (!cJSON_AddStringToObject(text, "type", "text") ||
        !cJSON_AddStringToObject(text, "text", "この写真に何が写っていますか。日本語で1〜2文で簡潔に説明してください。")) return false;
    cJSON* image = cJSON_CreateObject();
    if (!image) return false;
    cJSON_AddItemToArray(content, image);
    if (!cJSON_AddStringToObject(image, "type", "image_url")) return false;
    cJSON* url = cJSON_AddObjectToObject(image, "image_url");
    if (!url || !cJSON_AddStringToObject(url, "url", image_url.c_str())) return false;
    char* serialized = cJSON_PrintUnformatted(root.get());
    if (!serialized) return false;
    out = serialized;
    cJSON_free(serialized);
    return true;
}
}
