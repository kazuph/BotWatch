#include "network_ui.h"
#include "common.h"
#include "photo_storage.h"
#include "text_viewer.h"
#include "sdkconfig.h"
#include <cstdlib>

namespace network_ui {
namespace {
enum class State { inactive, waiting, message, text };
State state = State::inactive;
network_job::Kind last_kind = network_job::Kind::ocr;
uint64_t photo = 0;
void message(const char* value) {
    state = State::message;
    auto& d = M5.Display;
    d.fillScreen(pal::kBg);
    text(d, "通信", 160, 20, pal::kRed, F40, top_center);
    text(d, value, 160, 90, pal::kText, F24, top_center);
    text(d, "B:再送  A:もどる", 160, 175, pal::kAccent, F24, top_center);
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
    if (network_job::busy()) { message("前の処理が終了待ち"); return false; }
    uint8_t* jpeg = nullptr;
    size_t size = 0;
    char error[80];
    if (!photo_storage::read_latest(&jpeg, &size, error, sizeof(error))) { message(error); return false; }
    if (!network_job::start(kind, photo, jpeg, size, CONFIG_FACES_DESCRIPTION_HOST,
                            CONFIG_FACES_DESCRIPTION_PORT, CONFIG_FACES_DESCRIPTION_MODEL)) {
        free(jpeg);
        message("通信を開始できません");
        return false;
    }
    state = State::waiting;
    dialog("通信", pal::kCyan, "送信しています", "保存しました", "SELECT:中止");
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
                    text_viewer::show(result.text);
                } else message(result.text.c_str());
            }
        } else if (network_job::expired()) {
            network_job::cancel();
            message("通信が時間切れ");
        } else if (!network_job::busy()) {
            message("通信処理に失敗");
        }
        return;
    }
    if (state == State::text) text_viewer::tick();
    if (pad_just & kB) { text_viewer::clear(); begin(last_kind); }
    else if (pad_just & kA) { text_viewer::clear(); state = State::inactive; }
}
void cancel() {
    ++photo;
    network_job::cancel();
    text_viewer::clear();
    state = State::inactive;
}
}
