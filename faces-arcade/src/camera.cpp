#include "common.h"
#include "photo_storage.h"
#include "vision.h"
#include "arcade_led.h"
#include "network_ui.h"
#include "imagenet_ja.h"
#include "esp_camera.h"
#include "esp_jpeg_dec.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <sys/stat.h>

namespace {
enum class Mode { face, classify, text, describe };
Mode mode = Mode::face;
bool camera_ready = false;
bool showing_message = false;
enum class CaptureState { idle, countdown, saving };
CaptureState capture_state = CaptureState::idle;
uint32_t capture_start = 0;
uint8_t* captured = nullptr;
size_t captured_size = 0;
int captured_width = 0, captured_height = 0;
M5Canvas preview(&M5.Display);
const char* const mode_names[] = {"顔の検出", "画像全体の分類", "文字", "説明"};

void memory_log(const char* stage) {
    printf("CAMERA memory %s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u\n", stage,
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

void show_error(const char* message) {
    showing_message = true;
    dialog("カメラ", pal::kRed, "処理できません", message, "A:もどる");
    printf("CAMERA error: %s\n", message);
}

bool start_camera(framesize_t frame_size) {
    camera_config_t config = {};
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.pin_xclk = -1;
    config.pin_sccb_sda = -1;
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = M5.In_I2C.getPort();
    config.pin_d7 = 47; config.pin_d6 = 48; config.pin_d5 = 16; config.pin_d4 = 15;
    config.pin_d3 = 42; config.pin_d2 = 41; config.pin_d1 = 40; config.pin_d0 = 39;
    config.pin_vsync = 46; config.pin_href = 38; config.pin_pclk = 45;
    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0; config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = frame_size;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    const esp_err_t result = esp_camera_init(&config);
    camera_ready = result == ESP_OK;
    if (!camera_ready) printf("CAMERA init=%s\n", esp_err_to_name(result));
    return camera_ready;
}

void enter() {
    mode = Mode::face;
    showing_message = false;
    capture_state = CaptureState::idle;
    preview.setPsram(true);
    preview.setColorDepth(16);
    void* buffer = preview.createSprite(320, 240);
    if (!buffer || !esp_ptr_external_ram(buffer)) {
        preview.deleteSprite();
        show_error("メモリ不足");
        return;
    }
    if (start_camera(FRAMESIZE_QVGA)) vision::set_mode(vision::Mode::faces);
    memory_log("camera_init");
    if (!camera_ready) {
        show_error("初期化失敗");
    }
}

void view_latest() {
    char error[80];
    uint8_t* data = nullptr;
    size_t read_size = 0;
    if (!photo_storage::read_latest(&data, &read_size, error, sizeof(error))) {
        show_error(error);
        return;
    }
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t decoder = nullptr;
    jpeg_dec_header_info_t info = {};
    jpeg_dec_io_t io = {};
    io.inbuf = data;
    const bool size_ok = read_size <= static_cast<size_t>(std::numeric_limits<int>::max());
    io.inbuf_len = size_ok ? static_cast<int>(read_size) : 0;
    bool valid = size_ok && jpeg_dec_open(&config, &decoder) == JPEG_ERR_OK;
    if (valid) valid = jpeg_dec_parse_header(decoder, &io, &info) == JPEG_ERR_OK && info.width && info.height;
    if (decoder) jpeg_dec_close(decoder);
    const float scale = valid ? std::min(1.0f, std::min(320.0f / info.width, 240.0f / info.height)) : 1.0f;
    const bool drawn = valid && M5.Display.drawJpg(data, read_size, 0, 0, 0, 0, 0, 0, scale);
    free(data);
    if (!drawn) { show_error("JPEG が不正です"); return; }
    showing_message = true;
    puts("CAMERA viewed latest");
}

void draw_prediction(const vision::Prediction& prediction, int y) {
    auto& display = preview;
    constexpr int name_x = 8;
    constexpr int name_width = 224;
    constexpr int row_height = 51;
    display.fillRect(0, y - 2, 320, row_height, pal::kBg);
    display.setFont(F24);
    display.setTextSize(1);
    const std::string name = kImagenetJa[prediction.category];
    size_t split = 0;
    while (split < name.size()) {
        const unsigned char c = name[split];
        const size_t bytes = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        const size_t next = std::min(split + bytes, name.size());
        const std::string candidate = name.substr(0, next) + (next < name.size() ? "..." : "");
        if (display.textWidth(candidate.c_str(), F24) > name_width) break;
        split = next;
    }
    const std::string label = name.substr(0, split) + (split < name.size() ? "..." : "");
    display.setClipRect(name_x, y, name_width, row_height - 2);
    text(display, label.c_str(), name_x, y, pal::kText, F24);
    display.clearClipRect();
    char score[16];
    snprintf(score, sizeof(score), "%.1f%%", prediction.probability * 100.0f);
    text(display, score, 316, y, pal::kAccent, F24, top_right);
}

void tick() {
    if (network_ui::active()) { network_ui::tick(); return; }
    if (capture_state == CaptureState::saving) {
        arcade_led::clear();
        capture_state = CaptureState::idle;
        char path[64], error[80];
        const int64_t save_start = esp_timer_get_time();
        const bool saved = photo_storage::save_rgb565(captured, captured_size, captured_width, captured_height,
                                                       path, sizeof(path), error, sizeof(error));
        printf("CAMERA save_us=%lld\n", static_cast<long long>(esp_timer_get_time() - save_start));
        free(captured); captured = nullptr;
        memory_log("save");
        if (!saved) { show_error(error); return; }
        printf("CAMERA saved=%s\n", path);
        if (mode == Mode::text || mode == Mode::describe) {
            network_ui::begin(mode == Mode::text ? network_job::Kind::ocr : network_job::Kind::describe);
        } else {
            showing_message = true;
            dialog("カメラ", pal::kGreen, "保存しました", nullptr, "A:もどる");
        }
        return;
    }
    if (showing_message) {
        if (pad_just & (kA | kB | kStart)) showing_message = false;
        return;
    }
    if (!camera_ready) return;
    if (capture_state == CaptureState::idle && (pad_just & kStart)) { view_latest(); return; }
    if (capture_state == CaptureState::idle && (pad_just & kB)) {
        mode = static_cast<Mode>((static_cast<int>(mode) + 1) % 4);
        arcade_led::clear();
        vision::set_mode(mode == Mode::face ? vision::Mode::faces :
                         mode == Mode::classify ? vision::Mode::classify : vision::Mode::off);
    }
    camera_fb_t* frame = esp_camera_fb_get();
    if (!frame) { capture_state = CaptureState::idle; arcade_led::clear(); show_error("取得失敗"); return; }
    preview.pushImage(0, 0, frame->width, frame->height, reinterpret_cast<uint16_t*>(frame->buf));
    int countdown = 0;
    if (capture_state == CaptureState::countdown) {
        const uint32_t elapsed = lgfx::millis() - capture_start;
        if (elapsed >= 3000) {
            arcade_led::clear();
            esp_camera_fb_return(frame);
            const int64_t start = esp_timer_get_time();
            captured_size = 0; captured_width = 0; captured_height = 0;
            esp_camera_deinit();
            camera_ready = false;
            const bool capture_ok = start_camera(FRAMESIZE_VGA);
            camera_fb_t* photo = capture_ok ? esp_camera_fb_get() : nullptr;
            if (photo && photo->width == 640 && photo->height == 480) {
                captured = static_cast<uint8_t*>(heap_caps_malloc(photo->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (captured) {
                    captured_size = photo->len; captured_width = photo->width; captured_height = photo->height;
                    memcpy(captured, photo->buf, photo->len);
                }
            }
            memory_log("photo");
            if (photo) esp_camera_fb_return(photo);
            if (camera_ready) esp_camera_deinit();
            camera_ready = false;
            const bool preview_ok = start_camera(FRAMESIZE_QVGA);
            printf("CAMERA capture_us=%lld size=%dx%d\n", static_cast<long long>(esp_timer_get_time() - start), captured_width, captured_height);
            memory_log("capture");
            if (!captured || !preview_ok) {
                free(captured); captured = nullptr;
                capture_state = CaptureState::idle;
                show_error(preview_ok ? "撮影失敗" : "初期化失敗");
                return;
            }
            capture_state = CaptureState::saving;
            return;
        } else {
            const unsigned step = elapsed / 1000;
            arcade_led::countdown(elapsed);
            char count[2] = {char('3' - step), 0};
            countdown = count[0] - '0';
        }
    }
    preview.fillRect(0, 0, 320, 30, pal::kBg);
    char title[80];
    snprintf(title, sizeof(title), "モード: %s", mode_names[static_cast<int>(mode)]);
    text(preview, title, 160, 2, pal::kText, F24, top_center);
    if (mode == Mode::face) {
        std::vector<vision::Box> boxes;
        if (vision::faces(frame->buf, frame->width, frame->height, boxes)) {
            arcade_led::show(boxes.empty() ? arcade_led::Color::off : arcade_led::Color::white);
            for (const auto& box : boxes)
                preview.drawRect(box.x1, box.y1, box.x2 - box.x1 + 1, box.y2 - box.y1 + 1, pal::kGreen);
            if (!boxes.empty()) {
                char count[32];
                snprintf(count, sizeof(count), "顔 %u人", unsigned(boxes.size()));
                preview.fillRect(0, 210, 320, 30, pal::kBg);
                text(preview, count, 160, 212, pal::kText, F24, top_center);
            }
        } else {
            esp_camera_fb_return(frame); show_error("顔検出に失敗"); return;
        }
    } else if (mode == Mode::classify) {
        std::vector<vision::Prediction> results;
        if (vision::classify(frame->buf, frame->width, frame->height, results)) {
            preview.fillRect(0, 84, 320, 156, pal::kBg);
            for (size_t i = 0; i < results.size(); ++i)
                draw_prediction(results[i], 86 + i * 51);
        } else {
            esp_camera_fb_return(frame); show_error("分類に失敗"); return;
        }
    }
    if (pad_just & kA) {
        capture_start = lgfx::millis();
        capture_state = CaptureState::countdown;
        arcade_led::countdown(0);
    }
    if (countdown) {
        char count[2] = {char('0' + countdown), 0};
        text(preview, count, 160, 90, pal::kAccent, F40, top_center, 2.0f);
    }
    preview.pushSprite(0, 0);
    esp_camera_fb_return(frame);
}

void leave() {
    network_ui::cancel();
    free(captured); captured = nullptr;
    capture_state = CaptureState::idle;
    arcade_led::clear();
    vision::stop();
    if (camera_ready) esp_camera_deinit();
    camera_ready = false;
    preview.deleteSprite();
}

void icon(int x, int y) {
    // Existing built-in font supplies the menu symbol.
    text(M5.Display, "写", x + 20, y + 5, pal::kCyan, F24, top_center);
}
}

const Game kCamera = {"カメラ", nullptr, enter, tick, icon, leave};
