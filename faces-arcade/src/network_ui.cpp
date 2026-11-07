#include "network_ui.h"
#include "common.h"
#include "photo_storage.h"
#include "text_viewer.h"
#include "ui_icons.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"
#include <cmath>
#include <cstdlib>
#include <initializer_list>

namespace network_ui {
namespace {
enum class State { inactive, waiting, message, text };
State state = State::inactive;
network_job::Kind last_kind = network_job::Kind::ocr;
uint64_t photo = 0;
M5Canvas thumbnail(&uiFrame);
uint32_t startedAt = 0;
bool retryAllowed = false;

void fitted(const char* value, int x, int y, int width, uint16_t color, const lgfx::IFont* font) {
    std::string label = value ? value : "";
    const std::string original = label;
    while (!label.empty() && uiFrame.textWidth((label + "…").c_str(), font) > width) {
        size_t n = label.size() - 1;
        while (n && (static_cast<unsigned char>(label[n]) & 0xc0) == 0x80) --n;
        label.resize(n);
    }
    if (uiFrame.textWidth(original.c_str(), font) <= width) label = original;
    else label += "…";
    text(uiFrame, label.c_str(), x, y, color, font, top_center);
}

void message(const char* value, bool retry = true) {
    state = State::message;
    retryAllowed = retry && strcmp(value, "通信設定がありません") != 0 && strcmp(value, "説明先が未設定") != 0;
    const char* title = "つながりません";
    if (!strcmp(value, "通信設定がありません")) title = "通信の設定がありません";
    else if (!strcmp(value, "Wi-Fi に接続できません")) title = "Wi-Fiにつながりません";
    else if (!strcmp(value, "写真がありません")) title = "写真がありません";
    else if (!strcmp(value, "写真を読めません")) title = "写真を読めません";
    else if (!strcmp(value, "SD を使えません")) title = "SDを使えません";
    else if (!strcmp(value, "メモリ不足")) title = "メモリ不足";
    else if (strstr(value, "時間切れ")) title = "時間切れ";
    else if (!strcmp(value, "文字がありません")) title = "文字が見つかりません";
    else if (strstr(value, "JSON") || strstr(value, "応答が不正")) title = "うまく読めません";
    auto& d = uiFrame;
    d.fillScreen(rgb(7, 9, 18));
    d.fillSmoothRoundRect(16, 20, 288, 172, 18, rgb(255, 181, 71));
    d.fillSmoothRoundRect(18, 22, 284, 168, 16, rgb(20, 24, 52));
    ui_icon::warning(d, 160, 34, 50, rgb(255, 181, 71), rgb(20, 24, 52));
    const lgfx::IFont* font = d.textWidth(title, F32) <= 272 ? F32 : F24;
    fitted(title, 160, 93, 272, pal::kText, font);
    fitted(value, 160, 145, 272, pal::kDim, F24);
    darkenRoundRect(0, 202, 320, 38, 0);
    ui_icon::buttonA(d, 22, 221);
    text(d, "カメラ", 40, 207, pal::kText, F24);
    if (retryAllowed) {
        ui_icon::buttonB(d, 148, 221);
        text(d, "送り直す", 166, 207, pal::kText, F24);
    }
    d.pushSprite(0, 0);
}

void drawWaiting() {
    auto& d = uiFrame;
    const uint32_t elapsed = lgfx::millis() - startedAt;
    const uint16_t accent = last_kind == network_job::Kind::ocr ? pal::kAccent : rgb(155, 123, 255);
    d.fillScreen(rgb(7, 9, 18));
    d.fillSmoothRoundRect(6, 6, 140, 34, 17, accent);
    d.fillSmoothRoundRect(7, 7, 138, 32, 16, rgb(14, 18, 38));
    if (last_kind == network_job::Kind::ocr) ui_icon::kana(d, 26, 23, accent);
    else ui_icon::bubble(d, 26, 23, accent);
    text(d, last_kind == network_job::Kind::ocr ? "文字よみ" : "せつめい", 42, 10, pal::kText, F24);
    thumbnail.pushSprite(&d, 14, 54);
    d.fillArc(234, 102, 52, 44, 0, 360, rgb(31, 42, 72));
    const float start = (elapsed % 2000) * 360.0f / 2000.0f;
    const float end = start + 90.0f;
    d.fillArc(234, 102, 52, 44, start, end, accent);
    for (float angle : {start, end}) {
        const float radians = angle * PI / 180.0f;
        d.fillSmoothCircle(234 + int(48 * cosf(radians)), 102 + int(48 * sinf(radians)), 4, accent);
    }
    char seconds[24];
    snprintf(seconds, sizeof(seconds), "%lu秒", static_cast<unsigned long>(elapsed / 1000));
    text(d, seconds, 234, 91, pal::kText, F24, top_center);
    const auto phase = network_job::phase();
    const char* status = phase == network_http::Phase::connecting ? "つないでいます" :
                         phase == network_http::Phase::sending ? "送っています" : "3090で読んでいます";
    text(d, status, 160, 160, pal::kText, F24, top_center);
    darkenRoundRect(0, 202, 320, 38, 0);
    text(d, "SELECT やめてメニューへ", 160, 207, pal::kText, F24, top_center);
    d.pushSprite(0, 0);
}
}
bool begin(network_job::Kind kind) {
    last_kind = kind;
    ++photo;
    if (kind == network_job::Kind::describe &&
        (!CONFIG_FACES_DESCRIPTION_HOST[0] || !CONFIG_FACES_DESCRIPTION_PORT || !CONFIG_FACES_DESCRIPTION_MODEL[0])) {
        message("説明先が未設定");
        return false;
    }
    if (network_job::busy()) { message("前の処理を片付け中"); return false; }
    uint8_t* jpeg = nullptr;
    size_t size = 0;
    char error[80];
    if (!photo_storage::read_latest(&jpeg, &size, error, sizeof(error))) { message(error, false); return false; }
    if (!thumbnail.getBuffer()) {
        thumbnail.setColorDepth(16);
        thumbnail.setPsram(true);
        void* memory = thumbnail.createSprite(128, 96);
        if (!memory || !esp_ptr_external_ram(memory)) {
            thumbnail.deleteSprite(); free(jpeg); message("メモリ不足", false); return false;
        }
    }
    if (!thumbnail.drawJpg(jpeg, size, 0, 0, 0, 0, 0, 0, 0.2f)) {
        free(jpeg); message("写真を読めません", false); return false;
    }
    if (!network_job::start(kind, photo, jpeg, size, CONFIG_FACES_DESCRIPTION_HOST,
                            CONFIG_FACES_DESCRIPTION_PORT, CONFIG_FACES_DESCRIPTION_MODEL)) {
        free(jpeg);
        message("通信を開始できません");
        return false;
    }
    state = State::waiting;
    startedAt = lgfx::millis();
    drawWaiting();
    return true;
}
bool active() { return state != State::inactive; }
void tick() {
    if (state == State::waiting) {
        network_job::Result result;
        if (network_job::take(result)) {
            if (result.photo == photo) {
                if (result.ok) {
                    state = State::text;
                    text_viewer::show(result.text, last_kind);
                } else message(result.text.c_str());
            }
        } else if (network_job::expired()) {
            puts("NETWORK cancel_reason=deadline");
            network_job::cancel();
            if (last_kind == network_job::Kind::ocr) request_menu = true;
            else message("通信が時間切れ");
        } else if (!network_job::busy()) {
            message("通信処理に失敗");
        } else drawWaiting();
        return;
    }
    if (state == State::text) text_viewer::tick();
    if ((pad_just & kB) && (state != State::message || retryAllowed)) { text_viewer::clear(); begin(last_kind); }
    else if (pad_just & kA) { text_viewer::clear(); thumbnail.deleteSprite(); state = State::inactive; }
}
void cancel() {
    printf("NETWORK cancel_reason=%s\n", pad_exit ? "select" : "other");
    ++photo;
    network_job::cancel();
    text_viewer::clear();
    thumbnail.deleteSprite();
    state = State::inactive;
}
}
