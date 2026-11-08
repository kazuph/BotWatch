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
bool is_cancelled(void* context) { return static_cast<std::atomic<bool>*>(context)->load(); }
}
Response request(microlink_t* ml, uint32_t address, uint16_t port, const char* method,
                 const std::string& path, const std::string& content_type, const std::string& body,
                 std::atomic<bool>& cancelled, int64_t connect_deadline_us, int64_t request_duration_us,
                 std::atomic<uint32_t>* visible_deadline_ms, std::atomic<Phase>* phase,
                 std::atomic<bool>* link_state, bool recover_ocr, int64_t absolute_limit_us,
                 std::atomic<int64_t>* cancelled_at_us) {
    Response result{Error::none, 0, {}};
    const int64_t trace_start = esp_timer_get_time();
    const auto trace_ms = [&]() { return (long long)((esp_timer_get_time() - trace_start) / 1000); };
    const auto trace_route = [&]() {
        for (int i = 0; i < microlink_get_peer_count(ml); ++i) {
            microlink_peer_info_t peer = {};
            if (microlink_get_peer_info(ml, i, &peer) == ESP_OK && peer.vpn_ip == address) {
                printf("NETWORK route direct=%d elapsed_ms=%lld\n", peer.direct_path, trace_ms());
                return;
            }
        }
        printf("NETWORK route unknown elapsed_ms=%lld\n", trace_ms());
    };
    printf("NETWORK connect_start elapsed_ms=%lld\n", trace_ms());
    trace_route();
    result.error = interrupted(cancelled, connect_deadline_us);
    if (result.error != Error::none) return result;
    const int64_t remaining = connect_deadline_us - esp_timer_get_time();
    if (remaining <= 0) { result.error = Error::timeout; return result; }
    errno = 0;
    Socket socket(microlink_tcp_connect_until(ml, address, port, connect_deadline_us, is_cancelled, &cancelled));
    const int connect_errno = errno;
    printf("NETWORK connect_end connected=%d elapsed_ms=%lld\n", bool(socket), trace_ms());
    trace_route();
    result.error = interrupted(cancelled, connect_deadline_us);
    if (result.error != Error::none) { printf("NETWORK connect_result error=%d elapsed_ms=%lld\n", int(result.error), trace_ms()); return result; }
    if (!socket) { result.error = connect_errno == ECONNREFUSED ? Error::refused : Error::connect; return result; }
    if (phase) phase->store(Phase::sending);
    const int64_t request_deadline_us = std::min(esp_timer_get_time() + request_duration_us, absolute_limit_us);
    const auto cleanup_deadline = [&]() {
        const int64_t now = esp_timer_get_time();
        const int64_t cancelled_at = cancelled_at_us ? cancelled_at_us->load() : 0;
        const int64_t start = std::min(now >= request_deadline_us ? request_deadline_us : now,
                                       cancelled_at > 0 ? cancelled_at : now);
        return start + 120LL * 1000000;
    };
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
    if (microlink_tcp_send_until(socket.get(), headers.data(), headers.size(), request_deadline_us, is_cancelled, &cancelled) != ESP_OK) {
        result.error = interrupted(cancelled, request_deadline_us); if (result.error == Error::none) result.error = Error::send; return result;
    }
    for (size_t offset = 0; offset < body.size();) {
        if (link_state) link_state->store(microlink_is_connected(ml));
        result.error = interrupted(cancelled, request_deadline_us);
        if (result.error != Error::none) return result;
        const size_t size = std::min(size_t(4096), body.size() - offset);
        if (microlink_tcp_send_until(socket.get(), body.data() + offset, size, request_deadline_us, is_cancelled, &cancelled) != ESP_OK) {
            result.error = interrupted(cancelled, request_deadline_us); if (result.error == Error::none) result.error = Error::send; return result;
        }
        offset += size;
        if (offset % (16 * 1024) == 0 || offset == body.size())
            printf("NETWORK sent_bytes=%zu total_bytes=%zu elapsed_ms=%lld\n", offset, body.size(), trace_ms());
    }
    printf("NETWORK send_complete elapsed_ms=%lld\n", trace_ms());
    trace_route();
    if (phase) phase->store(Phase::processing);
    HttpResponse response;
    char buffer[2048];
    bool first_response = true;
    while (!response.complete()) {
        if (link_state) link_state->store(microlink_is_connected(ml));
        if (!result.cleanup_only) {
            result.error = interrupted(cancelled, request_deadline_us);
            if (result.error != Error::none) {
                if (!recover_ocr) return result;
                result.cleanup_only = true;
                result.cleanup_deadline_us = cleanup_deadline();
                if (phase) phase->store(Phase::cancelled_cleanup);
            }
        }
        const int64_t remaining_us = (result.cleanup_only ? result.cleanup_deadline_us : request_deadline_us) - esp_timer_get_time();
        if (remaining_us <= 0) { result.error = Error::timeout; return result; }
        const uint32_t wait_ms = std::min<int64_t>((remaining_us + 999) / 1000, 250);
        const int count = microlink_tcp_recv(socket.get(), buffer, sizeof(buffer), wait_ms);
        if (count < 0) {
            if (!response.finish_eof()) { result.error = Error::receive; return result; }
            break;
        }
        if (count && first_response) { printf("NETWORK first_response elapsed_ms=%lld\n", trace_ms()); trace_route(); first_response = false; }
        if (count && !response.feed(buffer, count)) { result.error = Error::invalid_http; return result; }
    }
    result.status = response.status();
    result.body = response.body();
    if (!result.cleanup_only) {
        result.error = interrupted(cancelled, request_deadline_us);
        if (result.error != Error::none && recover_ocr) {
            result.cleanup_only = true;
            result.cleanup_deadline_us = cleanup_deadline();
        }
        if (result.error == Error::none && (result.status < 200 || result.status >= 300)) result.error = Error::http_status;
        if (phase) phase->store(result.cleanup_only ? Phase::cancelled_cleanup : Phase::cleanup);
    }
    return result;
}
}
