#include "arcade_led.h"
#include "led_ramp.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace arcade_led {
namespace {
constexpr int kPin = 5;
constexpr uint32_t kCount = 10;
led_strip_handle_t strip = nullptr;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
uint8_t requested[3] = {};
bool pulse = false;
led_ramp::Channel channels[3];

void set_target(uint8_t red, uint8_t green, uint8_t blue, bool once) {
    portENTER_CRITICAL(&lock);
    requested[0] = red > led_ramp::maximum ? led_ramp::maximum : red;
    requested[1] = green > led_ramp::maximum ? led_ramp::maximum : green;
    requested[2] = blue > led_ramp::maximum ? led_ramp::maximum : blue;
    pulse = once;
    portEXIT_CRITICAL(&lock);
}
void worker(void*) {
    for (;;) {
        uint8_t target[3];
        portENTER_CRITICAL(&lock);
        for (int i = 0; i < 3; ++i) target[i] = requested[i];
        portEXIT_CRITICAL(&lock);
        const int64_t now = esp_timer_get_time();
        bool changed = false;
        led_ramp::Channel next[3] = {channels[0], channels[1], channels[2]};
        for (int i = 0; i < 3; ++i) {
            next[i].advance(target[i], now);
            changed |= next[i].value != channels[i].value;
        }
        if (changed) {
            bool ok = true;
            for (uint32_t i = 0; i < kCount; ++i)
                ok &= led_strip_set_pixel(strip, i, next[0].value, next[1].value, next[2].value) == ESP_OK;
            if (ok && led_strip_refresh(strip) == ESP_OK) {
                const int64_t sent = esp_timer_get_time();
                for (int i = 0; i < 3; ++i) {
                    if (next[i].value != channels[i].value) next[i].last_change = sent;
                    channels[i] = next[i];
                }
            }
        }
        portENTER_CRITICAL(&lock);
        if (pulse && channels[0].value == requested[0] && channels[1].value == requested[1] && channels[2].value == requested[2]) {
            requested[0] = requested[1] = requested[2] = 0;
            pulse = false;
        }
        portEXIT_CRITICAL(&lock);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}
esp_err_t begin() {
    if (strip) return ESP_ERR_INVALID_STATE;
    led_strip_config_t led = {};
    led.strip_gpio_num = kPin;
    led.max_leds = kCount;
    led.led_model = LED_MODEL_SK6812;
    led.color_component_format.format.r_pos = 1;
    led.color_component_format.format.g_pos = 0;
    led.color_component_format.format.b_pos = 2;
    led.color_component_format.format.w_pos = 3;
    led.color_component_format.format.bytes_per_color = 1;
    led.color_component_format.format.num_components = 3;
    led_strip_rmt_config_t rmt = {};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT;
    esp_err_t err = led_strip_new_rmt_device(&led, &rmt, &strip);
    if (err != ESP_OK) return err;
    err = led_strip_clear(strip);
    if (err != ESP_OK) {
        led_strip_del(strip);
        strip = nullptr;
        return err;
    }
    const int64_t initialized = esp_timer_get_time();
    for (auto& channel : channels) channel.last_change = initialized;
    if (xTaskCreate(worker, "arcade_led", 3072, nullptr, 3, nullptr) != pdPASS) {
        led_strip_del(strip);
        strip = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t show(Color color) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    set_target(color == Color::red || color == Color::white ? 64 : 0,
               color == Color::green || color == Color::white ? 64 : 0,
               color == Color::white ? 64 : 0,
               color == Color::red || color == Color::green);
    return ESP_OK;
}
esp_err_t green_level(uint8_t level) { if (!strip) return ESP_ERR_INVALID_STATE; set_target(0, level, 0, false); return ESP_OK; }
esp_err_t countdown(uint32_t elapsed_ms) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    const auto level = led_ramp::countdown_target(elapsed_ms);
    set_target(level, level, level, false);
    return ESP_OK;
}
esp_err_t clear() { if (!strip) return ESP_ERR_INVALID_STATE; set_target(0, 0, 0, false); return ESP_OK; }
}
