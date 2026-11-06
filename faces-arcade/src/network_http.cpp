#include "network_http.h"
#include "http_response.h"
#include "esp_timer.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <memory>

namespace network_http {
namespace {
struct SocketDeleter { void operator()(microlink_tcp_socket_t* socket) const { if (socket) microlink_tcp_close(socket); } };
using Socket = std::unique_ptr<microlink_tcp_socket_t, SocketDeleter>;
Error interrupted(std::atomic<bool>& cancelled, int64_t deadline) {
    if (cancelled.load()) return Error::cancelled;
    if (esp_timer_get_time() >= deadline) return Error::timeout;
    return Error::none;
}
}
Response request(microlink_t* ml, uint32_t address, uint16_t port, const char* method,
                 const std::string& path, const std::string& content_type, const std::string& body,
                 std::atomic<bool>& cancelled, int64_t connect_deadline_us, int64_t request_duration_us,
                 std::atomic<uint32_t>* visible_deadline_ms) {
    Response result{Error::none, 0, {}};
    result.error = interrupted(cancelled, connect_deadline_us);
    if (result.error != Error::none) return result;
    const int64_t remaining = connect_deadline_us - esp_timer_get_time();
    if (remaining <= 0) { result.error = Error::timeout; return result; }
    errno = 0;
    Socket socket(microlink_tcp_connect(ml, address, port, (remaining + 999) / 1000));
    const int connect_errno = errno;
    result.error = interrupted(cancelled, connect_deadline_us);
    if (result.error != Error::none) return result;
    if (!socket) { result.error = connect_errno == ECONNREFUSED ? Error::refused : Error::connect; return result; }
    const int64_t request_deadline_us = esp_timer_get_time() + request_duration_us;
    if (visible_deadline_ms) visible_deadline_ms->store(uint32_t(request_deadline_us / 1000));
    char ip[16];
    microlink_ip_to_str(address, ip);
    char prefix[256];
    int n = snprintf(prefix, sizeof(prefix), "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nContent-Length: %zu\r\n",
                     method, path.c_str(), ip, unsigned(port), body.size());
    if (n < 0 || n >= int(sizeof(prefix))) { result.error = Error::send; return result; }
    std::string headers(prefix, n);
    if (!content_type.empty()) headers += "Content-Type: " + content_type + "\r\n";
    headers += "\r\n";
    result.error = interrupted(cancelled, request_deadline_us);
    if (result.error != Error::none) return result;
    if (microlink_tcp_send(socket.get(), headers.data(), headers.size()) != ESP_OK) {
        result.error = Error::send; return result;
    }
    for (size_t offset = 0; offset < body.size();) {
        result.error = interrupted(cancelled, request_deadline_us);
        if (result.error != Error::none) return result;
        const size_t size = std::min(size_t(4096), body.size() - offset);
        if (microlink_tcp_send(socket.get(), body.data() + offset, size) != ESP_OK) {
            result.error = Error::send; return result;
        }
        offset += size;
    }
    HttpResponse response;
    char buffer[2048];
    while (!response.complete()) {
        result.error = interrupted(cancelled, request_deadline_us);
        if (result.error != Error::none) return result;
        const int64_t remaining_us = request_deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) { result.error = Error::timeout; return result; }
        const uint32_t wait_ms = std::min<int64_t>((remaining_us + 999) / 1000, 250);
        const int count = microlink_tcp_recv(socket.get(), buffer, sizeof(buffer), wait_ms);
        if (count < 0) {
            if (!response.finish_eof()) { result.error = Error::receive; return result; }
            break;
        }
        if (count && !response.feed(buffer, count)) { result.error = Error::invalid_http; return result; }
    }
    result.status = response.status();
    result.body = response.body();
    if (result.status < 200 || result.status >= 300) result.error = Error::http_status;
    return result;
}
}
