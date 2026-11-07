#include "common.h"
#include "photo_storage.h"
#include "vision.h"
#include "arcade_led.h"
#include "network_ui.h"
#include "network_settings.h"
#include "ui_icons.h"
#include "imagenet_ja.h"
#include "esp_camera.h"
#include "esp_jpeg_dec.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <sys/stat.h>

namespace {
enum class Mode { face, classify, text, describe };
Mode mode = Mode::face;
bool camera_ready = false;
bool showing_message = false;
bool viewerActive = false, viewerFromMenu = false, viewerTouchActive = false;
enum class CaptureState { idle, countdown, saving };
CaptureState capture_state = CaptureState::idle;
uint32_t capture_start = 0;
uint8_t* captured = nullptr;
size_t captured_size = 0;
int captured_width = 0, captured_height = 0;
M5Canvas preview(&M5.Display);
const char* const mode_names[] = {"顔さがし", "これなに？", "文字よみ", "せつめい"};
uint32_t toastAt = 0;
char toastText[80] = {};
bool toastError = false;
int64_t previewMeasureStart = 0;
unsigned previewMeasureFrames = 0;
Mode previewMeasureMode = Mode::face;
struct Classification {
    uint8_t* pixels;
    uint16_t width, height;
    unsigned generation;
    bool ok = false;
    std::vector<vision::Prediction> results;
};
portMUX_TYPE classifyLock = portMUX_INITIALIZER_UNLOCKED;
bool classifyBusy = false;
bool classifyStop = false;
unsigned classifyGeneration = 0;
Classification* classifyDone = nullptr;
std::vector<vision::Prediction> lastPredictions;

void classifyWorker(void* argument) {
    auto* job = static_cast<Classification*>(argument);
    job->ok = vision::classify(job->pixels, job->width, job->height, job->results);
    free(job->pixels);
    portENTER_CRITICAL(&classifyLock);
    const bool stop = classifyStop;
    if (!stop && job->generation == classifyGeneration) {
        classifyDone = job;
        job = nullptr;
    }
    if (!stop) classifyBusy = false;
    portEXIT_CRITICAL(&classifyLock);
    delete job;
    if (stop) {
        vision::stop();
        portENTER_CRITICAL(&classifyLock);
        classifyBusy = false;
        portEXIT_CRITICAL(&classifyLock);
    }
    vTaskDelete(nullptr);
}

void invalidateClassification(bool stop) {
    portENTER_CRITICAL(&classifyLock);
    ++classifyGeneration;
    Classification* done = classifyDone;
    classifyDone = nullptr;
    const bool busy = classifyBusy;
    if (stop && busy) classifyStop = true;
    portEXIT_CRITICAL(&classifyLock);
    delete done;
    lastPredictions.clear();
    if (stop && !busy) vision::stop();
}

bool classificationBusy() {
    portENTER_CRITICAL(&classifyLock);
    const bool busy = classifyBusy;
    portEXIT_CRITICAL(&classifyLock);
    return busy;
}

bool startClassification(const camera_fb_t* frame) {
    auto* job = new(std::nothrow) Classification;
    if (!job) return false;
    job->pixels = static_cast<uint8_t*>(heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!job->pixels) { delete job; return false; }
    memcpy(job->pixels, frame->buf, frame->len);
    job->width = frame->width;
    job->height = frame->height;
    portENTER_CRITICAL(&classifyLock);
    job->generation = classifyGeneration;
    classifyStop = false;
    classifyBusy = true;
    portEXIT_CRITICAL(&classifyLock);
    if (xTaskCreate(classifyWorker, "classify", CONFIG_ESP_MAIN_TASK_STACK_SIZE, job,
                    uxTaskPriorityGet(nullptr), nullptr) == pdPASS) return true;
    portENTER_CRITICAL(&classifyLock);
    classifyBusy = false;
    portEXIT_CRITICAL(&classifyLock);
    free(job->pixels);
    delete job;
    return false;
}

void measurePreview() {
    const int64_t now = esp_timer_get_time();
    if (!previewMeasureStart || previewMeasureMode != mode || capture_state != CaptureState::idle) {
        previewMeasureStart = now;
        previewMeasureFrames = 0;
        previewMeasureMode = mode;
        return;
    }
    ++previewMeasureFrames;
    const int64_t elapsed = now - previewMeasureStart;
    if (elapsed >= 1000000) {
        printf("CAMERA preview_fps mode=%d frames=%u elapsed_us=%lld fps=%.2f\n", int(mode),
               previewMeasureFrames, static_cast<long long>(elapsed), previewMeasureFrames * 1000000.0 / elapsed);
        previewMeasureStart = now;
        previewMeasureFrames = 0;
    }
}

void memory_log(const char* stage) {
    printf("CAMERA memory %s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u\n", stage,
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

void fittedCameraText(const char* value, int x, int y, int width, uint16_t color,
                      const lgfx::IFont* font, textdatum_t datum) {
    std::string label = value;
    if (preview.textWidth(label.c_str(), font) > width) {
        while (!label.empty() && preview.textWidth((label + "…").c_str(), font) > width) {
            size_t n = label.size() - 1;
            while (n && (static_cast<unsigned char>(label[n]) & 0xC0) == 0x80) --n;
            label.resize(n);
        }
        label += "…";
    }
    text(preview, label.c_str(), x, y, color, font, datum);
}

void show_error(const char* message) {
    showing_message = true;
    preview.fillScreen(rgb(7, 9, 18));
    preview.fillSmoothRoundRect(16, 20, 288, 172, 18, rgb(255, 181, 71));
    preview.fillSmoothRoundRect(18, 22, 284, 168, 16, rgb(20, 24, 52));
    ui_icon::warning(preview, 160, 34, 50, rgb(255, 181, 71), rgb(20, 24, 52));
    const char* title = message;
    if (!strcmp(message, "SD を使えません")) title = "SDを使えません";
    else if (!strcmp(message, "JPEG が不正です")) title = "写真を読めません";
    const auto* font = preview.textWidth(title, F32) <= 272 ? F32 : F24;
    fittedCameraText(title, 160, 98, 272, pal::kText, font, top_center);
    text(preview, "A・B・START カメラへ", 160, 207, pal::kText, F24, top_center);
    preview.pushSprite(0, 0);
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
    preview.setBuffer(uiFrame.getBuffer(), 320, 240, 16);
    viewerActive = false;
    mode = Mode::face;
    showing_message = false;
    capture_state = CaptureState::idle;
    toastAt = 0;
    previewMeasureStart = 0;
    previewMeasureFrames = 0;
    start_camera(FRAMESIZE_QVGA);
    memory_log("camera_init");
    if (!camera_ready) {
        show_error("初期化失敗");
    }
}

unsigned viewerIndex = 0, viewerTotal = 0;
int viewerTouchX = 0, viewerTouchY = 0;
uint32_t viewerNoticeAt = 0;
const char* viewerNotice = nullptr;

void drawViewer() {
    const int64_t started = esp_timer_get_time();
    char error[80] = {}, name[16] = {};
    uint8_t* data = nullptr;
    size_t size = 0;
    const bool read = photo_storage::read_index(viewerIndex, &viewerTotal, name, sizeof(name), &data, &size, error, sizeof(error));
    if (!read && !viewerTotal) { show_error(error); return; }
    preview.fillScreen(rgb(7, 9, 18));
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t decoder = nullptr;
    jpeg_dec_header_info_t info = {};
    jpeg_dec_io_t io = {};
    io.inbuf = data;
    const bool sizeOk = read && size <= static_cast<size_t>(std::numeric_limits<int>::max());
    io.inbuf_len = sizeOk ? static_cast<int>(size) : 0;
    bool valid = sizeOk && jpeg_dec_open(&config, &decoder) == JPEG_ERR_OK;
    if (valid) valid = jpeg_dec_parse_header(decoder, &io, &info) == JPEG_ERR_OK && info.width && info.height;
    if (decoder) jpeg_dec_close(decoder);
    const float scale = valid ? std::min(1.0f, std::min(320.0f / info.width, 240.0f / info.height)) : 1.0f;
    const bool drawn = valid && preview.drawJpg(data, size, 0, 0, 0, 0, 0, 0, scale);
    free(data);
    if (!drawn) text(preview, "この写真は読めません", 160, 105, pal::kText, F24, top_center);
    darkenRoundRect(preview, 0, 0, 320, 36, 0);
    char count[32]; snprintf(count, sizeof(count), "%u / %u", viewerIndex + 1, viewerTotal);
    text(preview, count, 12, 4, pal::kText, F24);
    text(preview, name, 150, 4, pal::kText, F24);
    ui_icon::arrowButton(preview, 30, 118, 22, false, rgb(25, 28, 40), pal::kText);
    ui_icon::arrowButton(preview, 290, 118, 22, true, rgb(25, 28, 40), pal::kText);
    darkenRoundRect(preview, 0, 202, 320, 38, 0);
    ui_icon::buttonB(preview, 26, 221);
    text(preview, "もどる", 44, 207, pal::kText, F24);
    if (viewerNotice) {
        darkenRoundRect(preview, 14, 76, 292, 88, 12);
        text(preview, viewerNotice, 160, 90, pal::kText, F24, top_center);
        text(preview, viewerIndex ? "◀ で新しいほうへ" : "▶ で古いほうへ", 160, 130, pal::kDim, F24, top_center);
    }
    preview.pushSprite(0, 0);
    printf("CAMERA viewer index=%u total=%u display_us=%lld valid=%d\n", viewerIndex, viewerTotal,
           (long long)(esp_timer_get_time() - started), drawn);
}

void beginViewer(bool fromMenu) {
    viewerFromMenu = fromMenu;
    viewerActive = true;
    viewerTouchActive = false;
    viewerIndex = 0; viewerTotal = 0;
    viewerNotice = nullptr;
    showing_message = false;
    preview.setBuffer(uiFrame.getBuffer(), 320, 240, 16);
    drawViewer();
}

void viewerMove(int direction) {
    if (!viewerTotal) return;
    if ((direction < 0 && viewerIndex == 0) || (direction > 0 && viewerIndex + 1 >= viewerTotal)) {
        viewerNotice = direction < 0 ? "いちばん新しい写真です" : "いちばん古い写真です";
        viewerNoticeAt = lgfx::millis();
    } else {
        viewerIndex = direction < 0 ? viewerIndex - 1 : viewerIndex + 1;
        viewerNotice = nullptr;
    }
    drawViewer();
}

void viewerTick() {
    if ((pad_just & kB) || (tap_just && tap_y >= 202 && tap_x < 160)) {
        viewerActive = false; showing_message = false;
        if (viewerFromMenu) request_menu = true;
        return;
    }
    if (viewerNotice && lgfx::millis() - viewerNoticeAt >= 2000) { viewerNotice = nullptr; drawViewer(); }
    if (pad_just & kRight) { viewerMove(1); return; }
    if (pad_just & kLeft) { viewerMove(-1); return; }
    const auto touch = M5.Touch.getDetail();
    if (touch.wasPressed()) { viewerTouchActive = true; viewerTouchX = touch.x; viewerTouchY = touch.y; }
    if (viewerTouchActive && touch.wasReleased()) {
        viewerTouchActive = false;
        const int dx = touch.x - viewerTouchX, dy = touch.y - viewerTouchY;
        if (abs(dx) > abs(dy) && abs(dx) > 12) viewerMove(dx < 0 ? 1 : -1);
        else if (viewerTouchY >= 40 && viewerTouchY < 202) {
            if (viewerTouchX < 106) viewerMove(-1);
            else if (viewerTouchX >= 214) viewerMove(1);
        }
    } else if (tap_just && !touch.wasPressed() && tap_y >= 40 && tap_y < 202) {
        if (tap_x < 106) viewerMove(-1);
        else if (tap_x >= 214) viewerMove(1);
    }
}

void draw_prediction(const vision::Prediction& prediction, int y, bool first) {
    auto& display = preview;
    constexpr int name_x = 18;
    constexpr int name_width = 208;
    constexpr int row_height = 44;
    display.setFont(F24);
    display.setTextSize(1);
    const std::string name = kImagenetJa[prediction.category];
    size_t split = 0;
    while (split < name.size()) {
        const unsigned char c = name[split];
        const size_t bytes = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        const size_t next = std::min(split + bytes, name.size());
        const std::string candidate = name.substr(0, next) + (next < name.size() ? "…" : "");
        if (display.textWidth(candidate.c_str(), F24) > name_width) break;
        split = next;
    }
    const std::string label = name.substr(0, split) + (split < name.size() ? "…" : "");
    display.setClipRect(name_x, y, name_width, row_height - 2);
    text(display, label.c_str(), name_x, y, first ? pal::kText : rgb(184, 181, 207), F24);
    display.clearClipRect();
    char score[16];
    snprintf(score, sizeof(score), "%.0f%%", prediction.probability * 100.0f);
    text(display, score, 302, y, pal::kAccent, F24, top_right);
    display.fillSmoothRoundRect(18, y + 30, 284, 6, 3, rgb(38, 41, 90));
    const int barWidth = std::clamp(int(prediction.probability * 284.0f), 0, 284);
    if (barWidth) display.fillSmoothRoundRect(18, y + 30, barWidth, 6, 3, rgb(155, 123, 255));
}

int64_t frameIconUs = 0;

void drawBadge() {
    const uint16_t accent = mode == Mode::face ? pal::kCyan : mode == Mode::text ? pal::kAccent : rgb(155, 123, 255);
    const int width = mode == Mode::classify ? 158 : 140;
    darkenRoundRect(preview, 6, 6, width, 34, 17);
    const int64_t iconStart = esp_timer_get_time();
    if (mode == Mode::face) ui_icon::face(preview, 26, 23, accent);
    else if (mode == Mode::classify) ui_icon::question(preview, 26, 23, accent);
    else if (mode == Mode::text) ui_icon::kana(preview, 26, 23, accent);
    else ui_icon::bubble(preview, 26, 23, accent);
    frameIconUs += esp_timer_get_time() - iconStart;
    text(preview, mode_names[static_cast<int>(mode)], 42, 10, pal::kText, F24);
    if (mode == Mode::text || mode == Mode::describe)
        ui_icon::link(preview, width + 17, 23, network_job::link_connected());
}

void drawFooter(bool countdown) {
    darkenRoundRect(preview, 0, 202, 320, 38, 0);
    if (countdown) {
        ui_icon::buttonB(preview, 124, 221);
        text(preview, "やめる", 142, 207, pal::kText, F24);
        return;
    }
    int64_t iconStart = esp_timer_get_time();
    ui_icon::buttonA(preview, 20, 221);
    frameIconUs += esp_timer_get_time() - iconStart;
    text(preview, mode == Mode::text ? "撮って読む" : mode == Mode::describe ? "撮って聞く" : "撮る",
         38, 207, pal::kText, F24);
    const int b = (mode == Mode::text || mode == Mode::describe) ? 176 : 108;
    iconStart = esp_timer_get_time();
    ui_icon::buttonB(preview, b, 221);
    frameIconUs += esp_timer_get_time() - iconStart;
    text(preview, "モード", b + 18, 207, pal::kText, F24);
    const int photoX = b + 18 + preview.textWidth("モード", F24) + 8;
    if (photoX + 38 + preview.textWidth("写真", F24) <= 320) {
        iconStart = esp_timer_get_time();
        ui_icon::buttonStart(preview, photoX, 209);
        frameIconUs += esp_timer_get_time() - iconStart;
        text(preview, "写真", photoX + 38, 207, pal::kText, F24);
    }
}

void drawToast() {
    if (!toastAt) return;
    const uint32_t elapsed = lgfx::millis() - toastAt;
    if (elapsed >= 2400) { toastAt = 0; return; }
    int y = 146;
    if (elapsed < 200) y += int((200 - elapsed) * 56 / 200);
    else if (elapsed >= 2200) y += int((elapsed - 2200) * 56 / 200);
    const uint16_t accent = toastError ? rgb(255, 181, 71) : pal::kGreen;
    preview.fillSmoothRoundRect(22, y, 276, 48, 24, accent);
    preview.fillSmoothRoundRect(24, y + 2, 272, 44, 22, rgb(14, 18, 38));
    if (toastError) ui_icon::warning(preview, 48, y + 9, 28, accent, rgb(20, 24, 52));
    else ui_icon::check(preview, 48, y + 24, 15, accent, rgb(6, 33, 15));
    char label[80];
    strlcpy(label, toastText, sizeof(label));
    if (preview.textWidth(label, F24) > 214) {
        size_t n = strlen(label);
        while (n && preview.textWidth((std::string(label) + "…").c_str(), F24) > 214) {
            do { --n; } while (n && (static_cast<unsigned char>(label[n]) & 0xC0) == 0x80);
            label[n] = 0;
        }
        strlcat(label, "…", sizeof(label));
    }
    text(preview, label, 72, y + 11, pal::kText, F24);
}

void drawFinder(int x1, int y1, int x2, int y2, int length, uint16_t color) {
    preview.drawWideLine(x1, y1, x1 + length, y1, 3, color);
    preview.drawWideLine(x1, y1, x1, y1 + length, 3, color);
    preview.drawWideLine(x2, y1, x2 - length, y1, 3, color);
    preview.drawWideLine(x2, y1, x2, y1 + length, 3, color);
    preview.drawWideLine(x1, y2, x1 + length, y2, 3, color);
    preview.drawWideLine(x1, y2, x1, y2 - length, 3, color);
    preview.drawWideLine(x2, y2, x2 - length, y2, 3, color);
    preview.drawWideLine(x2, y2, x2, y2 - length, 3, color);
}

void drawCountdown(uint32_t elapsed, int count) {
    darkenRoundRect(preview, 0, 0, 320, 240, 0);
    darkenRoundRect(preview, 102, 46, 116, 116, 58);
    const float start = 270.0f;
    const float end = start + 360.0f * (3000 - min(elapsed, uint32_t(3000))) / 3000.0f;
    preview.fillArc(160, 104, 61, 55, start, end, pal::kText);
    for (float angle : {start, end}) {
        const float radians = angle * PI / 180.0f;
        preview.fillSmoothCircle(160 + int(58 * cosf(radians)), 104 + int(58 * sinf(radians)), 3, pal::kText);
    }
    char label[2] = {char('0' + count), 0};
    uiGlyph.fillSprite(TFT_BLACK);
    text(uiGlyph, label, 96, 24, pal::kText, F40, middle_center);
    uiGlyph.pushRotateZoomWithAA(&preview, 160, 104, 0, 2.0f, 2.0f, TFT_BLACK);
    drawFooter(true);
}

void tick() {
    if (viewerActive) { viewerTick(); return; }
    if (network_ui::active()) { previewMeasureStart = 0; network_ui::tick(); return; }
    if (capture_state == CaptureState::saving) {
        previewMeasureStart = 0;
        arcade_led::clear();
        capture_state = CaptureState::idle;
        char path[64], error[80];
        const int64_t save_start = esp_timer_get_time();
        const bool saved = photo_storage::save_rgb565(captured, captured_size, captured_width, captured_height,
                                                       path, sizeof(path), error, sizeof(error));
        printf("CAMERA save_us=%lld\n", static_cast<long long>(esp_timer_get_time() - save_start));
        free(captured); captured = nullptr;
        memory_log("save");
        if (!saved) {
            toastError = true;
            strlcpy(toastText, error, sizeof(toastText));
            toastAt = lgfx::millis();
            return;
        }
        printf("CAMERA saved=%s\n", path);
        if (mode == Mode::text || mode == Mode::describe) {
            network_ui::begin(mode == Mode::text ? network_job::Kind::ocr : network_job::Kind::describe);
        } else {
            toastError = false;
            const char* name = strrchr(path, '/');
            char shortName[16];
            strlcpy(shortName, name ? name + 1 : path, sizeof(shortName));
            char* extension = strrchr(shortName, '.');
            if (extension) *extension = 0;
            snprintf(toastText, sizeof(toastText), "%s 保存", shortName);
            toastAt = lgfx::millis();
        }
        return;
    }
    if (showing_message) {
        previewMeasureStart = 0;
        if (pad_just & (kA | kB | kStart)) showing_message = false;
        return;
    }
    if (!camera_ready) return;
    const bool cancelledCapture = capture_state == CaptureState::countdown && (pad_just & kB);
    if (cancelledCapture) {
        capture_state = CaptureState::idle;
        arcade_led::clear();
    }
    if (!cancelledCapture && capture_state == CaptureState::idle &&
        ((pad_just & kStart) || (tap_just && tap_x >= 222 && tap_x < 316 && tap_y >= 42 && tap_y < 94))) {
        beginViewer(false); return;
    }
    if (!cancelledCapture && capture_state == CaptureState::idle && (pad_just & kB)) {
        mode = static_cast<Mode>((static_cast<int>(mode) + 1) % 4);
        arcade_led::clear();
        invalidateClassification(false);
    }
    const bool inferenceBusy = classificationBusy();
    if (!inferenceBusy)
        vision::set_mode(mode == Mode::face ? vision::Mode::faces :
                         mode == Mode::classify ? vision::Mode::classify : vision::Mode::off);
    const bool profile = mode == Mode::face && capture_state == CaptureState::idle;
    int64_t stageStart = esp_timer_get_time();
    int64_t getUs = 0, faceUs = 0, composeUs = 0, iconUs = 0, pushUs = 0;
    camera_fb_t* frame = esp_camera_fb_get();
    getUs = esp_timer_get_time() - stageStart;
    stageStart = esp_timer_get_time();
    if (!frame) { capture_state = CaptureState::idle; arcade_led::clear(); show_error("取得失敗"); return; }
    if (frame->width != 320 || frame->height != 240 || frame->format != PIXFORMAT_RGB565 || frame->len < 320 * 240 * 2) {
        esp_camera_fb_return(frame); show_error("取得失敗"); return;
    }
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
    preview.setBuffer(frame->buf, 320, 240, 16);
    composeUs += esp_timer_get_time() - stageStart;
    if (capture_state == CaptureState::idle && mode == Mode::face && !inferenceBusy) {
        std::vector<vision::Box> boxes;
        stageStart = esp_timer_get_time();
        const bool faceOk = vision::faces(frame->buf, frame->width, frame->height, boxes);
        faceUs = esp_timer_get_time() - stageStart;
        stageStart = esp_timer_get_time();
        if (faceOk) {
            arcade_led::show(boxes.empty() ? arcade_led::Color::off : arcade_led::Color::white);
            for (const auto& box : boxes) {
                const int x1 = std::clamp(box.x1, 0, 319), y1 = std::clamp(box.y1, 0, 239);
                const int x2 = std::clamp(box.x2, 0, 319), y2 = std::clamp(box.y2, 0, 239);
                if (x2 > x1 && y2 > y1) drawFinder(x1, y1, x2, y2, min(14, min(x2 - x1, y2 - y1) / 2), pal::kCyan);
            }
            if (!boxes.empty()) {
                char count[32];
                snprintf(count, sizeof(count), "%u人", unsigned(boxes.size()));
                preview.fillSmoothRoundRect(244, 6, 70, 34, 17, pal::kCyan);
                fittedCameraText(count, 279, 10, 62, rgb(3, 32, 43), F24, top_center);
            }
        } else {
            preview.setBuffer(uiFrame.getBuffer(), 320, 240, 16);
            esp_camera_fb_return(frame); show_error("顔検出に失敗"); return;
        }
    } else if (capture_state == CaptureState::idle && mode == Mode::classify) {
        portENTER_CRITICAL(&classifyLock);
        Classification* done = classifyDone;
        classifyDone = nullptr;
        portEXIT_CRITICAL(&classifyLock);
        if (done) {
            const bool ok = done->ok;
            lastPredictions = std::move(done->results);
            delete done;
            if (!ok) { preview.setBuffer(uiFrame.getBuffer(), 320, 240, 16); esp_camera_fb_return(frame); show_error("分類に失敗"); return; }
        }
        if (!inferenceBusy && !startClassification(frame)) {
            preview.setBuffer(uiFrame.getBuffer(), 320, 240, 16);
            esp_camera_fb_return(frame); show_error("分類に失敗"); return;
        }
        if (!lastPredictions.empty()) {
            darkenRoundRect(preview, 6, 94, 308, 140, 16);
            darkenRoundRect(preview, 6, 94, 308, 140, 16);
            darkenRoundRect(preview, 6, 94, 308, 140, 16);
            for (size_t i = 0; i < lastPredictions.size(); ++i)
                draw_prediction(lastPredictions[i], 102 + i * 44, i == 0);
        }
    }
    if (!cancelledCapture && (pad_just & kA)) {
        capture_start = lgfx::millis();
        capture_state = CaptureState::countdown;
        arcade_led::countdown(0);
    }
    composeUs += esp_timer_get_time() - stageStart;
    stageStart = esp_timer_get_time();
    frameIconUs = 0;
    if (countdown) drawCountdown(lgfx::millis() - capture_start, countdown);
    else {
        if (mode == Mode::text || mode == Mode::describe) drawFinder(50, 54, 270, 186, 16, pal::kAccent);
        drawBadge();
        darkenRoundRect(preview, 226, 46, 86, 44, 12);
        text(preview, "写真", 269, 56, pal::kText, F24, top_center);
        if (mode == Mode::classify) {
            const float angle = float(lgfx::millis() % 2000) * 360.0f / 2000.0f;
            preview.fillArc(180, 23, 10, 7, angle, angle + 240, pal::kText);
        }
        drawToast();
        drawFooter(false);
    }
    iconUs = esp_timer_get_time() - stageStart;
    stageStart = esp_timer_get_time();
    preview.pushSprite(0, 0);
    pushUs = esp_timer_get_time() - stageStart;
    if (profile && capture_state == CaptureState::idle) {
        printf("CAMERA stages_us get=%lld face=%lld compose=%lld overlay=%lld icons=%lld push=%lld\n",
               (long long)getUs, (long long)faceUs, (long long)composeUs,
               (long long)iconUs, (long long)frameIconUs, (long long)pushUs);
    }
    measurePreview();
    preview.setBuffer(uiFrame.getBuffer(), 320, 240, 16);
    esp_camera_fb_return(frame);
}

void leave() {
    network_ui::cancel();
    free(captured); captured = nullptr;
    capture_state = CaptureState::idle;
    arcade_led::clear();
    invalidateClassification(true);
    if (camera_ready) esp_camera_deinit();
    camera_ready = false;
}

void icon(int x, int y) {
    ui_icon::camera(uiGlyph, x + 2, y + 5, 36, pal::kText, rgb(59, 37, 128), pal::kCyan);
}
}

const Game kCamera = {"カメラ", nullptr, enter, tick, icon, leave, GameCategory::view, GameFooter::photo};
const Game kPhotoViewer = {"写真", nullptr, [] { beginViewer(true); }, viewerTick, nullptr, nullptr, GameCategory::view, GameFooter::photo};
