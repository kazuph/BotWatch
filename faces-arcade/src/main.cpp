// FACES ARCADE: menu for M5Stack Core2 + Faces Gamepad3.
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/select.h>
#include <unistd.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif
#include "common.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "photo_storage.h"
#include "network_serial.h"
#include "network_settings.h"
#endif

namespace {
M5Faces_Gamepad3 gamepad;
nvs_handle_t prefs = 0;
uint8_t pad_injected   = 0;  // set over serial for self tests
bool tap_injected      = false;
int injected_x = 0, injected_y = 0;

const Game* const kGames[] = {&kNoteQuiz, &kChordQuiz, &kKanaQuiz, &kSpectrum, &kBlockDrop, &kSnake, &kFlappy
#if CONFIG_IDF_TARGET_ESP32S3
    , &kCamera
#endif
};
const int kGameCount       = sizeof(kGames) / sizeof(kGames[0]);
const int kVisible         = 4;
int cursor                 = 0;
int scrollTop              = 0;
const Game* running        = nullptr;

void padRead() {
    M5.update();
    gamepad.update();
    uint8_t now = pad_injected;
    // The pad is active-low. Raw 0x00 means no valid sample yet, not "all eight pressed".
    if (gamepad.getState().raw != 0x00) now |= static_cast<uint8_t>(~gamepad.getState().raw);
    pad_just = now & ~pad_now;
    pad_now  = now;

    pad_exit = pad_just & kSelect;

    tap_just = false;
    const auto touch = M5.Touch.getDetail();
    if (touch.wasPressed()) {
        tap_just = true;
        tap_x    = touch.x;
        tap_y    = touch.y;
    }
    if (tap_injected) {
        tap_injected = false;
        tap_just     = true;
        tap_x        = injected_x;
        tap_y        = injected_y;
    }
    if (tap_just) printf("GAMES tap x=%d y=%d\n", tap_x, tap_y);
}

void drawRow(int index) {
    auto& d           = M5.Display;
    const int row     = index - scrollTop;
    if (row < 0 || row >= kVisible) return;
    const bool active = index == cursor;
    const int x = 6, y = 44 + row * 49, w = 296, h = 46;
    d.fillRoundRect(x, y, w, h, 7, active ? rgb(44, 62, 130) : pal::kPanel);
    d.drawRoundRect(x, y, w, h, 7, active ? pal::kCyan : pal::kLine);
    if (active) d.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 6, pal::kCyan);
    kGames[index]->icon(x + 3, y + 3);
    text(d, kGames[index]->name, x + 48, y + 7, active ? pal::kAccent : pal::kText, F32);
    if (kGames[index]->scoreKey) {
        char best[12];
        snprintf(best, sizeof(best), "%d", bestScore(kGames[index]->scoreKey));
        text(d, best, x + w - 8, y + 11, pal::kCyan, F24, top_right);
    }
}

void drawScrollbar() {
    auto& d = M5.Display;
    d.fillRect(306, 44, 12, 194, pal::kBg);
    d.drawRoundRect(308, 44, 8, 193, 3, pal::kLine);
    const int h = 193 * kVisible / kGameCount;
    d.fillRoundRect(308, 44 + (193 - h) * scrollTop / (kGameCount - kVisible), 8, h, 3, pal::kCyan);
}

void drawList() {
    for (int i = scrollTop; i < scrollTop + kVisible; ++i) drawRow(i);
    drawScrollbar();
}

void menuEnter() {
    auto& d = M5.Display;
    d.fillScreen(pal::kBg);
    text(d, "タップか上下+A", 160, 8, pal::kAccent, F24, top_center);
    drawList();
    puts("GAMES menu");
}

void openCursor() {
    running = kGames[cursor];
    const char* name = running->scoreKey ? running->scoreKey : "spectrum";
#if CONFIG_IDF_TARGET_ESP32S3
    if (running == &kCamera) name = "camera";
#endif
    printf("GAMES play=%s\n", name);
    running->enter();
}

void menuTick() {
    int next = cursor;
    if (pad_just & kUp) next = (cursor + kGameCount - 1) % kGameCount;
    if (pad_just & kDown) next = (cursor + 1) % kGameCount;
    if (tap_just && tap_y >= 44 && tap_y < 44 + 49 * kVisible) {
        if (tap_x >= 300) {  // scrollbar: page up or down
            scrollTop = tap_y < 44 + 49 * kVisible / 2 ? max(0, scrollTop - kVisible)
                                                       : min(kGameCount - kVisible, scrollTop + kVisible);
            cursor    = scrollTop;
            drawList();
            return;
        }
        cursor = scrollTop + (tap_y - 44) / 49;
        openCursor();
        return;
    }
    if (next != cursor) {
        const int prev = cursor;
        cursor         = next;
        const int top  = scrollTop;
        if (cursor < scrollTop) scrollTop = cursor;
        if (cursor >= scrollTop + kVisible) scrollTop = cursor - kVisible + 1;
        if (top != scrollTop) {
            drawList();
        } else {
            drawRow(prev);
            drawRow(cursor);
        }
    }
    if (pad_just & (kA | kStart)) openCursor();
}

// Self-test channel:
//   p<hex>  hold buttons   s  send the screen as RGB bytes
//   m<0|1>  mute speaker   f<hz>  use a sine wave instead of the microphone (0 = off)
void sendScreenshot() {
    static uint8_t row[320 * 3];
    puts("SHOT 320 240");
    for (int y = 0; y < 240; ++y) {
        M5.Display.readRectRGB(0, y, 320, 1, row);
        fwrite(row, 1, sizeof(row), stdout);
        vTaskDelay(1);
    }
    puts("\nENDSHOT");
    fflush(stdout);
}

void handleSerial() {
#if CONFIG_IDF_TARGET_ESP32S3
    static char line[1 + sizeof(network_settings::Credentials::ssid) + sizeof(network_settings::Credentials::password) + 1];
#else
    static char line[10];
#endif
    static size_t len = 0;
    static bool overflow = false;
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    timeval timeout = {0, 0};
    while (select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout) > 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;
        if (c == '\r') continue;
        if (c != '\n') {
            if (len + 1 < sizeof(line) && !overflow) line[len++] = c;
            else overflow = true;
            continue;
        }
        if (overflow) {
#if CONFIG_IDF_TARGET_ESP32S3
            network_settings::wipe(line, sizeof(line));
#else
            memset(line, 0, sizeof(line));
#endif
            len = 0;
            overflow = false;
            puts("入力が長すぎます");
            continue;
        }
        line[len] = 0;
        len = 0;
#if CONFIG_IDF_TARGET_ESP32S3
        network_serial_command(line);
#endif
        if (line[0] == 'p') pad_injected = strtoul(line + 1, nullptr, 16);
        if (line[0] == 's') sendScreenshot();
#if CONFIG_IDF_TARGET_ESP32S3
        if (line[0] == 'j') {
            char error[80];
            if (!photo_storage::send_latest(error, sizeof(error))) printf("JPEG error: %s\n", error);
        }
#endif
        if (line[0] == 'f') test_tone_hz = atoi(line + 1);
        if (line[0] == 'l') test_mic_left = line[1] == '1';
        if (line[0] == 'r') {  // clears scores left behind by a self test
            if (prefs) { nvs_erase_all(prefs); nvs_commit(prefs); }
            if (!running) menuEnter();
        }
        if (line[0] == 't' && sscanf(line + 1, "%d,%d", &injected_x, &injected_y) == 2) tap_injected = true;
        if (line[0] == 'm') {
            test_mute = line[1] == '1';
            M5.Speaker.setVolume(test_mute ? 0 : 200);
        }
#if CONFIG_IDF_TARGET_ESP32S3
        network_settings::wipe(line, sizeof(line));
#else
        memset(line, 0, sizeof(line));
#endif
    }
}
}  // namespace

uint8_t pad_now = 0, pad_just = 0;
bool pad_exit     = false;
bool request_menu = false;
bool tap_just     = false;
int tap_x = 0, tap_y = 0;
int test_tone_hz = 0;
bool test_mute   = false;
bool test_mic_left = false;

int bestScore(const char* key) {
    int32_t score = 0;
    if (prefs) nvs_get_i32(prefs, key, &score);
    return score;
}

bool submitScore(const char* key, int score) {
    if (score <= bestScore(key)) return false;
    return prefs && nvs_set_i32(prefs, key, score) == ESP_OK && nvs_commit(prefs) == ESP_OK;
}

void text(lgfx::LovyanGFX& d, const char* s, int x, int y, uint16_t color, const lgfx::IFont* font, textdatum_t datum,
          float scale) {
    d.setFont(font);
    d.setTextSize(scale);
    d.setTextDatum(datum);
    d.setTextColor(color);
    d.drawString(s, x, y);
    d.setTextSize(1);
}

void dialog(const char* title, uint16_t titleColor, const char* line1, const char* line2, const char* line3) {
    auto& d = M5.Display;
    const int w = 304, h = 164, x = (320 - w) / 2, y = (240 - h) / 2;
    d.fillRoundRect(x + 3, y + 3, w, h, 10, rgb(4, 5, 12));
    d.fillRoundRect(x, y, w, h, 10, pal::kPanel);
    d.drawRoundRect(x, y, w, h, 10, titleColor);
    d.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 9, titleColor);
    text(d, title, 160, y + 8, titleColor, F40, top_center);
    if (line1) text(d, line1, 160, y + 54, pal::kText, F32, top_center);
    if (line2) text(d, line2, 160, y + 94, pal::kAccent, F24, top_center);
    if (line3) text(d, line3, 160, y + 126, pal::kDim, F24, top_center);
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
#if CONFIG_IDF_TARGET_ESP32S3
    usb_serial_jtag_driver_config_t serial = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&serial));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
#else
    constexpr uart_port_t console = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    ESP_ERROR_CHECK(uart_driver_install(console, 2048, 2048, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_set_baudrate(console, 460800));
    uart_vfs_dev_use_driver(console);
    uart_vfs_dev_port_set_tx_line_endings(console, ESP_LINE_ENDINGS_LF);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("GAMES reset_reason=%d\n", int(esp_reset_reason()));
    M5.Display.setBrightness(180);
    M5.Display.fillScreen(pal::kBg);
#if CONFIG_IDF_TARGET_ESP32S3
    if (M5.getBoard() == m5::board_t::board_M5StackCoreS3 ||
        M5.getBoard() == m5::board_t::board_M5StackCoreS3SE) {
        esp_err_t led_result = arcade_led::begin();
        if (led_result != ESP_OK) printf("LED init=%s\n", esp_err_to_name(led_result));
    }
#endif
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_OK) nvs = nvs_open("arcade", NVS_READWRITE, &prefs);
    if (nvs != ESP_OK) printf("GAMES nvs=%s\n", esp_err_to_name(nvs));
    if (gamepad.begin(&M5.In_I2C, 0x08, 100000) != M5FACES_OK) {
        text(M5.Display, "ゲームパッドなし", 160, 100, pal::kRed, F32, top_center);
        puts("GAMES gamepad=FAIL");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    puts("GAMES gamepad=OK");
    // The Core2 IMU (0x68) and PDM mic share the small board in the stock bottom module.
    printf("GAMES imu_0x68=%d imu_enabled=%d board=%d\n", M5.In_I2C.scanID(0x68), M5.Imu.isEnabled(), int(M5.getBoard()));
    menuEnter();
}

void loop() {
    handleSerial();
    padRead();
    soundTick();
    if (!running) {
        menuTick();
    } else if (pad_exit || request_menu) {
        request_menu = false;
        if (running->leave) running->leave();
#if CONFIG_IDF_TARGET_ESP32S3
        arcade_led::clear();
#endif
        running = nullptr;
        menuEnter();
    } else {
        running->tick();
    }
    vTaskDelay(pdMS_TO_TICKS(4));
}

extern "C" void app_main() {
    setup();
    while (true) loop();
}
