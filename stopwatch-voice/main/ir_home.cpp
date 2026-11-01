#include "ir_home.h"

#include <cstdio>
#include <M5Unified.h>
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace ir_home {
namespace {
constexpr uint32_t kResolution = 1000000;
constexpr size_t kCaptureSymbols = 700;
QueueHandle_t queue;
rmt_channel_handle_t rx;
rmt_symbol_word_t capture[kCaptureSymbols];

bool IRAM_ATTR received(rmt_channel_handle_t, const rmt_rx_done_event_data_t *event, void *context)
{
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(static_cast<QueueHandle_t>(context), event, &wake);
    return wake == pdTRUE;
}

void receive_task(void *)
{
    rmt_rx_done_event_data_t event{};
    if (xQueueReceive(queue, &event, pdMS_TO_TICKS(30000)) == pdTRUE) {
        printf("IR_RX symbols=%u overflow=%d raw=", unsigned(event.num_symbols), event.num_symbols == kCaptureSymbols);
        for (size_t i = 0; i < event.num_symbols; ++i) {
            const auto &s = event.received_symbols[i];
            if (s.duration0) printf("%u:%u,", unsigned(s.level0), unsigned(s.duration0));
            if (s.duration1) printf("%u:%u,", unsigned(s.level1), unsigned(s.duration1));
        }
        printf("\n");
    } else {
        printf("IR_RX timeout\n");
    }
    fflush(stdout);
    rmt_disable(rx);
    rmt_del_channel(rx);
    vQueueDelete(queue);
    rx = nullptr;
    queue = nullptr;
    vTaskDelete(nullptr);
}
}

void receive_diagnostic()
{
    if (rx) { printf("IR_RX busy\n"); return; }
    M5.Power.setExtOutput(true);
    printf("IR_POWER ext_output=%d rx_gpio=11 tx_gpio=10\n", M5.Power.getExtOutput());
    rmt_rx_channel_config_t config{};
    config.gpio_num = GPIO_NUM_11;
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = kResolution;
    config.mem_block_symbols = 128;
    esp_err_t err = rmt_new_rx_channel(&config, &rx);
    if (err == ESP_OK) {
        queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
        if (!queue) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        rmt_rx_event_callbacks_t callbacks{};
        callbacks.on_recv_done = received;
        err = rmt_rx_register_event_callbacks(rx, &callbacks, queue);
    }
    if (err == ESP_OK) err = rmt_enable(rx);
    if (err == ESP_OK) {
        rmt_receive_config_t receive{};
        receive.signal_range_min_ns = 1250;
        receive.signal_range_max_ns = 12000000;
        err = rmt_receive(rx, capture, sizeof(capture), &receive);
    }
    if (err == ESP_OK && xTaskCreate(receive_task, "ir_rx_diag", 4096, nullptr, 5, nullptr) != pdPASS) err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        printf("IR_RX error=%s\n", esp_err_to_name(err));
        if (rx) { rmt_disable(rx); rmt_del_channel(rx); rx = nullptr; }
        if (queue) { vQueueDelete(queue); queue = nullptr; }
    } else printf("IR_RX ready timeout_ms=30000\n");
    fflush(stdout);
}

void transmit_diagnostic()
{
    M5.Power.setExtOutput(true);
    rmt_channel_handle_t tx = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
    rmt_tx_channel_config_t config{};
    config.gpio_num = GPIO_NUM_10;
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = kResolution;
    config.mem_block_symbols = 64;
    config.trans_queue_depth = 1;
    esp_err_t err = rmt_new_tx_channel(&config, &tx);
    rmt_carrier_config_t carrier{};
    carrier.frequency_hz = 38000;
    carrier.duty_cycle = 0.33f;
    if (err == ESP_OK) err = rmt_apply_carrier(tx, &carrier);
    rmt_copy_encoder_config_t encoder_config{};
    if (err == ESP_OK) err = rmt_new_copy_encoder(&encoder_config, &encoder);
    if (err == ESP_OK) err = rmt_enable(tx);
    rmt_symbol_word_t symbols[34]{};
    symbols[0].level0 = 1; symbols[0].duration0 = 9000; symbols[0].duration1 = 4500;
    constexpr uint32_t code = 0x20DF10EF;
    for (int i = 0; i < 32; ++i) {
        symbols[i + 1].level0 = 1; symbols[i + 1].duration0 = 560;
        symbols[i + 1].duration1 = code & (1UL << (31 - i)) ? 1690 : 560;
    }
    symbols[33].level0 = 1; symbols[33].duration0 = 560; symbols[33].duration1 = 1;
    rmt_transmit_config_t transmit{};
    if (err == ESP_OK) err = rmt_transmit(tx, encoder, symbols, sizeof(symbols), &transmit);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(tx, 1000);
    printf("IR_TX code=20DF10EF result=%s\n", esp_err_to_name(err));
    fflush(stdout);
    if (tx) rmt_disable(tx);
    if (encoder) rmt_del_encoder(encoder);
    if (tx) rmt_del_channel(tx);
}
}
