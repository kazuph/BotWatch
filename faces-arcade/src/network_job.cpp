#include "network_job.h"
#include "sdkconfig.h"
#if CONFIG_ML_ENABLE_CONFIG_HTTPD
#error "FACES ARCADE must not expose MicroLink's configuration server"
#endif
#include "network_http.h"
#include "network_payload.h"
#include "network_result.h"
#include "network_settings.h"
#include "network_wifi.h"
#include "microlink.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>

namespace network_job {
namespace {
constexpr int64_t kConnectUs = 30LL * 1000000;
constexpr int64_t kOcrUs = 60LL * 1000000;
constexpr int64_t kDescribeUs = 180LL * 1000000;
constexpr char kGpuAddress[] = "100.80.152.112";
constexpr uint16_t kOcrPort = 8016;
std::atomic<bool> active{false}, cancelled{false};
std::atomic<uint32_t> visible_deadline_ms{0};
QueueHandle_t results = nullptr;
struct Work {
    Kind kind;
    uint64_t photo;
    uint8_t* jpeg;
    size_t size;
    std::string host;
    uint16_t port;
    std::string model;
    int64_t started_us;
};
struct LinkDeleter { void operator()(microlink_t* link) const { if (link) microlink_destroy(link); } };
using Link = std::unique_ptr<microlink_t, LinkDeleter>;
void log_memory(const char* stage) {
    printf("NETWORK %s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u\n", stage,
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
const char* http_error(const network_http::Response& response) {
    switch (response.error) {
    case network_http::Error::cancelled: return "中止しました";
    case network_http::Error::timeout: return "通信が時間切れ";
    case network_http::Error::connect: return "接続できません";
    case network_http::Error::refused: return "接続を拒否されました";
    case network_http::Error::send: return "送信に失敗";
    case network_http::Error::receive: return "接続が切れました";
    case network_http::Error::invalid_http: return "応答が不正";
    case network_http::Error::http_status: return "HTTP エラー";
    default: return "通信エラー";
    }
}
void do_work(void* argument) {
    std::unique_ptr<Work> work(static_cast<Work*>(argument));
    Result output{work->photo, false, "通信に失敗"};
    network_settings::Credentials credentials{};
    Link link;
    auto perform = [&]() {
        if (work->kind == Kind::describe &&
            (work->host.empty() || !work->port || work->model.empty())) {
            output.text = "説明先が未設定"; return;
        }
        if (network_settings::load(&credentials) != ESP_OK) { output.text = "通信設定がありません"; return; }
        const int64_t connect_deadline = work->started_us + kConnectUs;
        auto wifi = network_wifi::connect(credentials, cancelled, connect_deadline);
        if (wifi != network_wifi::Result::connected) {
            output.text = wifi == network_wifi::Result::cancelled ? "中止しました" :
                          wifi == network_wifi::Result::timeout ? "接続が時間切れ" : "Wi-Fi に接続できません";
            return;
        }
        microlink_config_t config = {};
        config.auth_key = credentials.auth_key;
        config.device_name = "faces-arcade";
        config.enable_derp = true;
        config.enable_stun = true;
        config.enable_disco = true;
        config.priority_peer_ip = microlink_parse_ip(kGpuAddress);
        link.reset(microlink_init(&config));
        if (!link || microlink_start(link.get()) != ESP_OK) { output.text = "VPN を開始できません"; return; }
        while (!microlink_is_connected(link.get())) {
            if (cancelled.load()) { output.text = "中止しました"; return; }
            if (esp_timer_get_time() >= connect_deadline) { output.text = "接続が時間切れ"; return; }
            if (microlink_get_state(link.get()) == ML_STATE_ERROR) { output.text = "VPN に接続できません"; return; }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (work->kind == Kind::describe) {
            in_addr address = {};
            uint32_t target = inet_aton(work->host.c_str(), &address) == 1
                ? ntohl(address.s_addr) : microlink_resolve(link.get(), work->host.c_str());
            if (target != microlink_parse_ip(kGpuAddress)) {
                output.text = "送信先が 3090 ではありません";
                return;
            }
        }
        std::string body, content_type, path;
        uint16_t port;
        if (work->kind == Kind::ocr) {
            network_payload::Multipart multipart;
            if (!network_payload::ocr(work->jpeg, work->size, multipart)) { output.text = "画像の準備に失敗"; return; }
            body = std::move(multipart.body);
            content_type = std::move(multipart.content_type);
            path = "/v1/yomitoku/analyze";
            port = kOcrPort;
        } else {
            if (!network_payload::describe(work->jpeg, work->size, work->model.c_str(), body)) { output.text = "画像の準備に失敗"; return; }
            content_type = "application/json";
            path = "/v1/chat/completions";
            port = work->port;
        }
        log_memory("send");
        const int64_t duration = work->kind == Kind::ocr ? kOcrUs : kDescribeUs;
        auto response = network_http::request(link.get(), microlink_parse_ip(kGpuAddress), port, "POST", path,
                                              content_type, body, cancelled, connect_deadline, duration,
                                              &visible_deadline_ms);
        if (response.error != network_http::Error::none) {
            output.text = response.error == network_http::Error::http_status
                ? std::string("HTTP エラー ") + std::to_string(response.status) : http_error(response);
            return;
        }
        if (work->kind == Kind::describe) {
            auto parsed = network_result::parse_description(response.body);
            if (parsed.error != network_result::Error::none) {
                output.text = parsed.error == network_result::Error::malformed_json ? "JSON が不正" : "説明がありません";
                return;
            }
            output.text = std::move(parsed.text);
            output.ok = true;
            return;
        }
        auto parsed = network_result::parse_ocr(response.body);
        if (!parsed.job_id.empty()) {
            std::atomic<bool> cleanup_cancelled{false};
            const int64_t cleanup_deadline = esp_timer_get_time() + kConnectUs;
            auto cleanup = network_http::request(link.get(), microlink_parse_ip(kGpuAddress), kOcrPort, "DELETE",
                "/v1/yomitoku/jobs/" + parsed.job_id, "", "", cleanup_cancelled, cleanup_deadline, kConnectUs);
            if (cleanup.error != network_http::Error::none) { output.text = "サーバーの写真を消せません"; return; }
        }
        if (parsed.error != network_result::Error::none) {
            output.text = parsed.error == network_result::Error::missing_text ? "文字がありません" : "JSON またはジョブ ID が不正";
            return;
        }
        output.text = std::move(parsed.text);
        output.ok = true;
    };
    perform();
    link.reset();
    network_wifi::stop();
    network_settings::wipe(&credentials, sizeof(credentials));
    free(work->jpeg);
    if (!cancelled.load()) {
        if (int32_t(uint32_t(esp_timer_get_time() / 1000) - visible_deadline_ms.load()) >= 0) {
            output.ok = false;
            output.text = "通信が時間切れ";
        }
        Result* published = new(std::nothrow) Result(std::move(output));
        if (published && xQueueSend(results, &published, 0) != pdTRUE) delete published;
    }
    active.store(false);
}
void worker(void* argument) {
    do_work(argument);
    vTaskDelete(nullptr);
}
}
bool start(Kind kind, uint64_t photo, uint8_t* jpeg, size_t size,
           const char* host, uint16_t port, const char* model) {
    if (!jpeg || !size) return false;
    bool expected = false;
    if (!active.compare_exchange_strong(expected, true)) return false;
    if (!results) results = xQueueCreate(1, sizeof(Result*));
    if (!results) { active.store(false); return false; }
    Result* stale = nullptr;
    while (xQueueReceive(results, &stale, 0) == pdTRUE) delete stale;
    cancelled.store(false);
    const int64_t started_us = esp_timer_get_time();
    visible_deadline_ms.store(uint32_t((started_us + kConnectUs) / 1000));
    auto* work = new(std::nothrow) Work{kind, photo, jpeg, size, host ? host : "", port, model ? model : "", started_us};
    if (!work) { active.store(false); return false; }
    if (xTaskCreate(worker, "camera_net", 16384, work, 5, nullptr) != pdPASS) {
        delete work; active.store(false); return false;
    }
    return true;
}
void cancel() { cancelled.store(true); }
bool busy() { return active.load(); }
bool expired() {
    return active.load() && int32_t(uint32_t(esp_timer_get_time() / 1000) - visible_deadline_ms.load()) >= 0;
}
bool take(Result& result) {
    Result* item = nullptr;
    if (!results || xQueueReceive(results, &item, 0) != pdTRUE) return false;
    result = std::move(*item);
    delete item;
    return true;
}
}
