#include "ir_home.h"
#include "ir_monitor_decode.h"
#include "voice_screen.h"

#include <cstdio>
#include <atomic>
#include <vector>
#include <M5Unified.h>
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace ir_home {
namespace {
constexpr uint32_t kResolution = 1000000;
constexpr uint32_t kRxResolution = 500000;
constexpr unsigned kRxTickUs = 1000000 / kRxResolution;
constexpr size_t kCaptureSymbols = 700;
// Parent-selected floor: shorter NEC repeats and noise do not count as a button press.
constexpr size_t kMinSessionPulses = 20;
QueueHandle_t queue;
rmt_channel_handle_t rx;
rmt_symbol_word_t capture[kCaptureSymbols];
std::atomic<bool> session{false};
std::atomic<bool> skipped{false};
std::atomic<int> session_count{-1};
std::atomic<bool> scope_running{false};
std::atomic<bool> monitor_running{false}, monitor_stop{false}, transmitting{false};
constexpr unsigned kMonitorCapacity = kCaptureSymbols * 2;
constexpr uint32_t kMonitorGapUs = 60000;  // same gap as the selected RMT receive boundary
struct MonitorEdge { uint32_t at; uint8_t level; uint8_t self; };
MonitorEdge *monitor_ring = nullptr;
uint32_t *monitor_intervals = nullptr;
unsigned monitor_write = 0, monitor_read = 0, monitor_dropped = 0;
portMUX_TYPE monitor_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE scope_mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t scope_edges = 0;
volatile int64_t scope_last = 0, scope_min = INT64_MAX, scope_max = 0;

bool send_timings(const uint32_t *timings, size_t count, bool diagnostic = false)
{
    if (!timings || !count || (rx && !diagnostic) || scope_running) return false;
    M5.Power.setExtOutput(true);
    if (M5.Ex_I2C.getPort() == M5.In_I2C.getPort()) return false;
    M5.Ex_I2C.release();
    std::vector<rmt_symbol_word_t> symbols((count + 1) / 2);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t ticks = (timings[i] + 1) / 2;
        if (!ticks || ticks > 32767) return false;
        if ((i & 1) == 0) { symbols[i / 2].level0 = 1; symbols[i / 2].duration0 = ticks; }
        else { symbols[i / 2].level1 = 0; symbols[i / 2].duration1 = ticks; }
    }
    rmt_channel_handle_t tx = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
    rmt_tx_channel_config_t config{}; config.gpio_num = GPIO_NUM_10; config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = kRxResolution; config.mem_block_symbols = 64; config.trans_queue_depth = 1;
    rmt_carrier_config_t carrier{}; carrier.frequency_hz = 38000; carrier.duty_cycle = 0.33f;
    rmt_copy_encoder_config_t ec{}; rmt_transmit_config_t transmit{};
    esp_err_t err = rmt_new_tx_channel(&config, &tx);
    if (err == ESP_OK) err = rmt_apply_carrier(tx, &carrier);
    if (err == ESP_OK) err = rmt_new_copy_encoder(&ec, &encoder);
    bool enabled = false;
    if (err == ESP_OK) { err = rmt_enable(tx); enabled = err == ESP_OK; }
    if (err == ESP_OK) transmitting = true;
    if (err == ESP_OK) err = rmt_transmit(tx, encoder, symbols.data(), symbols.size() * sizeof(symbols[0]), &transmit);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(tx, -1);
    transmitting = false;
    if (enabled) rmt_disable(tx);
    if (encoder) rmt_del_encoder(encoder);
    if (tx) rmt_del_channel(tx);
    gpio_reset_pin(GPIO_NUM_10); gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT); gpio_set_level(GPIO_NUM_10, 0);
    return err == ESP_OK;
}

void IRAM_ATTR monitor_edge(void *)
{
    const MonitorEdge edge{uint32_t(esp_timer_get_time()), uint8_t(gpio_get_level(GPIO_NUM_11)), uint8_t(transmitting.load())};
    portENTER_CRITICAL_ISR(&monitor_mux);
    const unsigned next = (monitor_write + 1) % kMonitorCapacity;
    if (next == monitor_read) ++monitor_dropped;
    else { monitor_ring[monitor_write] = edge; monitor_write = next; }
    portEXIT_CRITICAL_ISR(&monitor_mux);
}

void monitor_task(void *)
{
    unsigned groups = 0, edges = 0, intervals = 0, dropped_seen = 0;
    uint32_t previous = 0;
    bool self = false, overflow = false; uint8_t initial_level = 1;
    auto finish = [&] {
        if (!edges) return;
        uint32_t code = 0;
        const bool decoded = initial_level == 0 && !overflow && decode_monitor_nec(monitor_intervals, intervals, code);
        ++groups;
        printf("IRMN_FRAME n=%u edges=%u first_us=%u self=%d overflow=%d nec=", groups, edges,
               intervals ? unsigned(monitor_intervals[0]) : 0, self, overflow);
        if (decoded) printf("%08lX", static_cast<unsigned long>(code)); else printf("UNKNOWN");
        printf(" intervals=");
        for (unsigned i = 0; i < intervals; ++i) printf("%u,", unsigned(monitor_intervals[i]));
        printf("\n"); fflush(stdout);
        voice_screen::ir_monitor(groups, decoded, code, edges);
        if (M5.Speaker.begin()) M5.Speaker.tone(2000, 100);
        edges = intervals = 0; self = overflow = false;
    };
    while (true) {
        MonitorEdge edge{}; bool available = false; unsigned dropped;
        portENTER_CRITICAL(&monitor_mux);
        if (monitor_read != monitor_write) {
            edge = monitor_ring[monitor_read]; monitor_read = (monitor_read + 1) % kMonitorCapacity; available = true;
        }
        dropped = monitor_dropped;
        portEXIT_CRITICAL(&monitor_mux);
        if (dropped != dropped_seen) {
            printf("IRMN_OVERFLOW dropped=%u\n", dropped); fflush(stdout);
            overflow = true; dropped_seen = dropped;
        }
        if (available) {
            const uint32_t gap = edge.at - previous;
            if (edges && gap >= kMonitorGapUs) finish();
            else if (edges) {
                if (intervals < kMonitorCapacity) monitor_intervals[intervals++] = gap;
                else overflow = true;
            }
            if (!edges) initial_level = edge.level;
            ++edges; self |= edge.self; previous = edge.at;
        } else {
            if (monitor_stop.load()) break;
            if (edges && uint32_t(esp_timer_get_time()) - previous >= kMonitorGapUs) finish();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    gpio_intr_disable(GPIO_NUM_11); gpio_isr_handler_remove(GPIO_NUM_11);
    finish();
    heap_caps_free(monitor_ring); heap_caps_free(monitor_intervals);
    monitor_ring = nullptr; monitor_intervals = nullptr;
    monitor_running = false;
    printf("IRMN stopped frames=%u dropped=%u\n", groups, dropped_seen); fflush(stdout);
    vTaskDelete(nullptr);
}

void scope_edge(void *)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&scope_mux);
    if (scope_last) {
        const int64_t delta = now - scope_last;
        if (delta < scope_min) scope_min = delta;
        if (delta > scope_max) scope_max = delta;
    }
    scope_last = now;
    ++scope_edges;
    portEXIT_CRITICAL_ISR(&scope_mux);
}

bool IRAM_ATTR received(rmt_channel_handle_t, const rmt_rx_done_event_data_t *event, void *context)
{
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(static_cast<QueueHandle_t>(context), event, &wake);
    return wake == pdTRUE;
}

void receive_task(void *)
{
    const TickType_t started = xTaskGetTickCount();
    constexpr TickType_t timeout = pdMS_TO_TICKS(20000);
    bool captured = false;
    rmt_rx_done_event_data_t event{};
    while (session || xTaskGetTickCount() - started < timeout) {
        if (session && skipped) { session_count = -2; captured = true; break; }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (xQueueReceive(queue, &event, session ? pdMS_TO_TICKS(100) : timeout - elapsed) != pdTRUE) {
            if (session) continue;
            break;
        }
        bool has_pulse = false;
        size_t pulse_count = 0;
        for (size_t i = 0; i < event.num_symbols; ++i) {
            const auto &s = event.received_symbols[i];
            has_pulse |= s.duration0 != 0 || s.duration1 != 0;
            pulse_count += (s.duration0 != 0) + (s.duration1 != 0);
        }
        if (session && has_pulse && pulse_count < kMinSessionPulses) {
            const auto &first = event.received_symbols[0];
            printf("IR_RX rejected pulses=%u first_level=%u first_us=%u\n", unsigned(pulse_count),
                   unsigned(first.level0), unsigned(first.duration0) * kRxTickUs);
            fflush(stdout);
            has_pulse = false;
        }
        if (has_pulse) {
            if (session) { printf("IR_RX accepted pulses=%u\n", unsigned(pulse_count)); fflush(stdout); session_count = int(event.num_symbols); captured = true; break; }
            printf("IR_RX symbols=%u overflow=%d raw=", unsigned(event.num_symbols), event.num_symbols == kCaptureSymbols);
            for (size_t i = 0; i < event.num_symbols; ++i) {
                const auto &s = event.received_symbols[i];
                if (s.duration0) printf("%u:%u,", unsigned(s.level0), unsigned(s.duration0) * kRxTickUs);
                if (s.duration1) printf("%u:%u,", unsigned(s.level1), unsigned(s.duration1) * kRxTickUs);
            }
            printf("\n");
            captured = true;
            break;
        }
        rmt_receive_config_t receive{};
        receive.signal_range_min_ns = 2000;
        receive.signal_range_max_ns = 60000000;
        const esp_err_t err = rmt_receive(rx, capture, sizeof(capture), &receive);
        if (session) { printf("IR_RX rearm pulses=%u status=%s\n", unsigned(pulse_count), esp_err_to_name(err)); fflush(stdout); }
        if (err != ESP_OK) { printf("IR_RX error=%s\n", esp_err_to_name(err)); session_count = -3; captured = true; break; }
    }
    if (!captured) printf("IR_RX timeout\n");
    fflush(stdout);
    rmt_disable(rx);
    rmt_del_channel(rx);
    vQueueDelete(queue);
    rx = nullptr;
    queue = nullptr;
    session = false;
    vTaskDelete(nullptr);
}
}

void receive_diagnostic()
{
    if (rx || monitor_running) { printf("IR_RX busy\n"); return; }
    M5.Power.setExtOutput(true);
    const bool uses_ir_pins = M5.Ex_I2C.getSDA() == 10 || M5.Ex_I2C.getSDA() == 11 ||
                              M5.Ex_I2C.getSCL() == 10 || M5.Ex_I2C.getSCL() == 11;
    const bool shared_bus = M5.Ex_I2C.getPort() == M5.In_I2C.getPort();
    printf("IR_I2C enabled=%d port=%d sda=%d scl=%d shared=%d\n", M5.Ex_I2C.isEnabled(),
           int(M5.Ex_I2C.getPort()), int(M5.Ex_I2C.getSDA()), int(M5.Ex_I2C.getSCL()), shared_bus);
    if (uses_ir_pins && M5.Ex_I2C.isEnabled() && !shared_bus)
        printf("IR_I2C release=%d\n", M5.Ex_I2C.release());
    if (uses_ir_pins && shared_bus) { printf("IR_RX error=shared_i2c\n"); session = false; return; }
    gpio_reset_pin(GPIO_NUM_10);
    gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_10, 0);
    printf("IR_POWER ext_output=%d rx_gpio=11 tx_gpio=10\n", M5.Power.getExtOutput());
    rmt_rx_channel_config_t config{};
    config.gpio_num = GPIO_NUM_11;
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = kRxResolution;
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
        receive.signal_range_min_ns = 2000;
        receive.signal_range_max_ns = 60000000;
        err = rmt_receive(rx, capture, sizeof(capture), &receive);
    }
    if (err == ESP_OK && xTaskCreate(receive_task, "ir_rx_diag", 4096, nullptr, 5, nullptr) != pdPASS) err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        printf("IR_RX error=%s\n", esp_err_to_name(err));
        if (rx) { rmt_disable(rx); rmt_del_channel(rx); rx = nullptr; }
        if (queue) { vQueueDelete(queue); queue = nullptr; }
    } else {
        gpio_dump_io_configuration(stdout, (1ULL << 10) | (1ULL << 11));
        printf("IR_RX ready timeout_ms=20000\n");
    }
    fflush(stdout);
}

void receive_session()
{
    if (rx) { printf("IR_RX busy\n"); return; }
    skipped = false;
    session_count = -1;
    session = true;
    receive_diagnostic();
    if (!rx) session = false;
}

void skip_session() { if (session) skipped = true; }
bool session_active() { return session; }

void edge_scope(bool tx_low)
{
    if (rx) { printf("IRSCOPE error=receiver_busy\n"); fflush(stdout); return; }
    M5.Power.setExtOutput(true);
    const bool shared = M5.Ex_I2C.getPort() == M5.In_I2C.getPort();
    printf("IRSCOPE i2c_port=%d sda=%d scl=%d shared=%d power=%d\n", int(M5.Ex_I2C.getPort()),
           int(M5.Ex_I2C.getSDA()), int(M5.Ex_I2C.getSCL()), shared, M5.Power.getExtOutput());
    if (shared) { printf("IRSCOPE error=shared_i2c\n"); fflush(stdout); return; }
    printf("IRSCOPE i2c_release=%d\n", M5.Ex_I2C.release());
    gpio_reset_pin(GPIO_NUM_10);
    gpio_set_direction(GPIO_NUM_10, tx_low ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    if (tx_low) gpio_set_level(GPIO_NUM_10, 0);
    gpio_reset_pin(GPIO_NUM_11);
    gpio_set_direction(GPIO_NUM_11, GPIO_MODE_INPUT);
    gpio_pullup_en(GPIO_NUM_11);
    gpio_set_intr_type(GPIO_NUM_11, GPIO_INTR_ANYEDGE);
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (err == ESP_OK) err = gpio_isr_handler_add(GPIO_NUM_11, scope_edge, nullptr);
    if (err != ESP_OK) { printf("IRSCOPE error=%s\n", esp_err_to_name(err)); fflush(stdout); return; }
    portENTER_CRITICAL(&scope_mux);
    scope_edges = 0; scope_last = 0; scope_min = INT64_MAX; scope_max = 0;
    portEXIT_CRITICAL(&scope_mux);
    printf("IRSCOPE begin tx_low=%d seconds=60\n", tx_low); fflush(stdout);
    for (unsigned second = 1; second <= 60; ++second) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t edges; int64_t last, low, high;
        portENTER_CRITICAL(&scope_mux);
        edges = scope_edges; last = scope_last; low = scope_min; high = scope_max;
        scope_edges = 0; scope_min = INT64_MAX; scope_max = 0;
        portEXIT_CRITICAL(&scope_mux);
        const int64_t since = last ? esp_timer_get_time() - last : -1;
        printf("IRSCOPE second=%u edges=%u level=%d min_us=%lld max_us=%lld since_us=%lld\n",
               second, unsigned(edges), gpio_get_level(GPIO_NUM_11), (long long)(low == INT64_MAX ? -1 : low),
               (long long)high, (long long)since);
        fflush(stdout);
    }
    gpio_intr_disable(GPIO_NUM_11);
    gpio_isr_handler_remove(GPIO_NUM_11);
    printf("IRSCOPE end\n"); fflush(stdout);
}

void scope_task(void *arg)
{
    edge_scope(reinterpret_cast<uintptr_t>(arg) != 0);
    scope_running = false;
    vTaskDelete(nullptr);
}

bool start_edge_scope(bool tx_low)
{
    if (monitor_running) return false;
    bool expected = false;
    if (!scope_running.compare_exchange_strong(expected, true)) return false;
    if (xTaskCreate(scope_task, "ir_scope", 4096, reinterpret_cast<void *>(uintptr_t(tx_low)), 5, nullptr) != pdPASS) {
        scope_running = false;
        return false;
    }
    return true;
}

bool edge_scope_active() { return scope_running; }

bool start_monitor()
{
    if (rx || scope_running || monitor_running) { printf("IRMN error=busy\n"); fflush(stdout); return false; }
    M5.Power.setExtOutput(true);
    if (M5.Ex_I2C.getPort() == M5.In_I2C.getPort()) { printf("IRMN error=shared_i2c\n"); fflush(stdout); return false; }
    const bool released = M5.Ex_I2C.release();
    gpio_reset_pin(GPIO_NUM_10); gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT); gpio_set_level(GPIO_NUM_10, 0);
    gpio_reset_pin(GPIO_NUM_11); gpio_set_direction(GPIO_NUM_11, GPIO_MODE_INPUT);
    gpio_pullup_en(GPIO_NUM_11); gpio_set_intr_type(GPIO_NUM_11, GPIO_INTR_ANYEDGE);
    printf("IRMN memory internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u\n",
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    monitor_ring = static_cast<MonitorEdge *>(heap_caps_malloc(sizeof(MonitorEdge) * kMonitorCapacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    monitor_intervals = static_cast<uint32_t *>(heap_caps_malloc(sizeof(uint32_t) * kMonitorCapacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!monitor_ring || !monitor_intervals) {
        heap_caps_free(monitor_ring); heap_caps_free(monitor_intervals); monitor_ring = nullptr; monitor_intervals = nullptr;
        printf("IRMN error=no_memory\n"); fflush(stdout); return false;
    }
    monitor_write = monitor_read = monitor_dropped = 0; monitor_stop = false;
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (err == ESP_OK) err = gpio_isr_handler_add(GPIO_NUM_11, monitor_edge, nullptr);
    if (err != ESP_OK) {
        heap_caps_free(monitor_ring); heap_caps_free(monitor_intervals); monitor_ring = nullptr; monitor_intervals = nullptr;
        printf("IRMN error=%s\n", esp_err_to_name(err)); fflush(stdout); return false;
    }
    monitor_running = true;
    if (xTaskCreate(monitor_task, "ir_monitor", 4096, nullptr, 5, nullptr) != pdPASS) {
        gpio_intr_disable(GPIO_NUM_11); gpio_isr_handler_remove(GPIO_NUM_11);
        heap_caps_free(monitor_ring); heap_caps_free(monitor_intervals); monitor_ring = nullptr; monitor_intervals = nullptr;
        monitor_running = false; printf("IRMN error=task_start\n"); fflush(stdout); return false;
    }
    printf("IRMN ready mic_running=%d tx_low=%d i2c_release=%d power=%d gap_us=%u\n", M5.Mic.isRunning(),
           gpio_get_level(GPIO_NUM_10) == 0, released, M5.Power.getExtOutput(), unsigned(kMonitorGapUs));
    fflush(stdout);
    voice_screen::ir_monitor(0, false, 0, 0);
    return true;
}

void stop_monitor() { if (monitor_running) { gpio_intr_disable(GPIO_NUM_11); monitor_stop = true; } }
bool monitor_active() { return monitor_running; }

void monitor_probe()
{
    if (!monitor_running) { printf("IRMN_PROBE error=monitor_not_running\n"); fflush(stdout); return; }
    const auto timings = ir_codes::nec(0x00000000);
    const bool sent = send_timings(timings.data(), timings.size());
    printf("IRMN_PROBE sent=%d code=00000000 invalid_complements=1\n", sent); fflush(stdout);
}

bool send_tv(ir_codes::TvKey key, bool diagnostic)
{
    const auto timings = ir_codes::nec(ir_codes::tv_code(key));
    return send_timings(timings.data(), timings.size(), diagnostic);
}

void compare_tv_loopback()
{
    if (rx || scope_running || monitor_running) { printf("TVLOOP error=busy\n"); fflush(stdout); return; }
    M5.Power.setExtOutput(true);
    if (M5.Ex_I2C.getPort() == M5.In_I2C.getPort()) { printf("TVLOOP error=shared_i2c\n"); fflush(stdout); return; }
    M5.Ex_I2C.release();
    queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    rmt_rx_channel_config_t config{}; config.gpio_num = GPIO_NUM_11; config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = kRxResolution; config.mem_block_symbols = 128;
    esp_err_t err = queue ? rmt_new_rx_channel(&config, &rx) : ESP_ERR_NO_MEM;
    rmt_rx_event_callbacks_t callbacks{}; callbacks.on_recv_done = received;
    if (err == ESP_OK) err = rmt_rx_register_event_callbacks(rx, &callbacks, queue);
    bool enabled = false;
    if (err == ESP_OK) { err = rmt_enable(rx); enabled = err == ESP_OK; }
    rmt_receive_config_t receive{}; receive.signal_range_min_ns = 2000; receive.signal_range_max_ns = 60000000;
    if (err == ESP_OK) err = rmt_receive(rx, capture, sizeof(capture), &receive);
    const bool sent = err == ESP_OK && send_tv(ir_codes::TvKey::volume_up, true);
    rmt_rx_done_event_data_t event{};
    const bool observed = sent && xQueueReceive(queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE;
    printf("TVLOOP sent=%d observed=%d error=%s raw=", sent, observed, esp_err_to_name(err));
    if (observed) for (size_t i = 0; i < event.num_symbols; ++i) {
        const auto &s = capture[i];
        if (s.duration0) printf("%u:%u,", unsigned(s.level0), unsigned(s.duration0) * kRxTickUs);
        if (s.duration1) printf("%u:%u,", unsigned(s.level1), unsigned(s.duration1) * kRxTickUs);
    }
    printf("\n"); fflush(stdout);
    if (enabled) rmt_disable(rx);
    if (rx) { rmt_del_channel(rx); rx = nullptr; }
    if (queue) { vQueueDelete(queue); queue = nullptr; }
}

bool send_aircon(bool on, int mode, int temperature)
{
    const auto timings = ir_codes::hitachi_timings(ir_codes::hitachi_frame(on, mode, temperature));
    return send_timings(timings.data(), timings.size());
}

const char *learned_key(Learned slot)
{
    constexpr const char *keys[] = {"terrestrial", "satellite", "light_on", "light_off", "light_orange", "light_night", "light_white"};
    const unsigned index = static_cast<unsigned>(slot);
    return index < sizeof(keys) / sizeof(keys[0]) ? keys[index] : nullptr;
}

bool save_session(Learned slot)
{
    const char *key = learned_key(slot);
    const int count = session_count.load();
    if (!key || session || count <= 0 || count >= int(kCaptureSymbols)) return false;
    std::vector<uint16_t> timings;
    timings.reserve(count * 2);
    unsigned expected = 0;
    for (int i = 0; i < count; ++i) {
        const auto &s = capture[i];
        for (int half = 0; half < 2; ++half) {
            const unsigned duration = half ? s.duration1 : s.duration0;
            const unsigned level = half ? s.level1 : s.level0;
            if (!duration) continue;
            if (level != expected || duration * kRxTickUs > UINT16_MAX) return false;
            timings.push_back(duration * kRxTickUs); expected ^= 1;
        }
    }
    if (timings.size() < kMinSessionPulses) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ir_learn", NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    err = nvs_set_blob(handle, key, timings.data(), timings.size() * sizeof(timings[0]));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    printf("IR_LEARN key=%s pulses=%u result=%s\n", key, unsigned(timings.size()), esp_err_to_name(err)); fflush(stdout);
    return err == ESP_OK;
}

bool has_learned(Learned slot)
{
    const char *key = learned_key(slot); nvs_handle_t handle; size_t bytes = 0;
    if (!key || nvs_open("ir_learn", NVS_READONLY, &handle) != ESP_OK) return false;
    const esp_err_t err = nvs_get_blob(handle, key, nullptr, &bytes);
    nvs_close(handle);
    return err == ESP_OK && bytes >= kMinSessionPulses * sizeof(uint16_t) && bytes <= kCaptureSymbols * 2 * sizeof(uint16_t) && bytes % sizeof(uint16_t) == 0;
}

bool send_learned(Learned slot)
{
    const char *key = learned_key(slot); nvs_handle_t handle; size_t bytes = 0;
    if (!key || nvs_open("ir_learn", NVS_READONLY, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_get_blob(handle, key, nullptr, &bytes);
    if (err != ESP_OK || bytes < kMinSessionPulses * sizeof(uint16_t) || bytes > kCaptureSymbols * 2 * sizeof(uint16_t) || bytes % sizeof(uint16_t)) { nvs_close(handle); return false; }
    std::vector<uint16_t> stored(bytes / sizeof(uint16_t));
    err = nvs_get_blob(handle, key, stored.data(), &bytes); nvs_close(handle);
    if (err != ESP_OK) return false;
    std::vector<uint32_t> timings(stored.begin(), stored.end());
    return send_timings(timings.data(), timings.size());
}

void loopback_probe()
{
    if (rx || scope_running || monitor_running) { printf("IR_LOOP error=busy\n"); fflush(stdout); return; }
    M5.Power.setExtOutput(true);
    if (M5.Ex_I2C.getPort() == M5.In_I2C.getPort()) { printf("IR_LOOP error=shared_i2c\n"); fflush(stdout); return; }
    M5.Ex_I2C.release();
    gpio_reset_pin(GPIO_NUM_10); gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT); gpio_set_level(GPIO_NUM_10, 0);
    rmt_channel_handle_t receiver = nullptr, transmitter = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
    QueueHandle_t events = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (!events) { printf("IR_LOOP error=no_memory\n"); fflush(stdout); return; }
    rmt_rx_channel_config_t rc{}; rc.gpio_num = GPIO_NUM_11; rc.clk_src = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz = kRxResolution; rc.mem_block_symbols = 128;
    rmt_tx_channel_config_t tc{}; tc.gpio_num = GPIO_NUM_10; tc.clk_src = RMT_CLK_SRC_DEFAULT;
    tc.resolution_hz = kResolution; tc.mem_block_symbols = 64; tc.trans_queue_depth = 1;
    rmt_rx_event_callbacks_t callbacks{}; callbacks.on_recv_done = received;
    rmt_copy_encoder_config_t ec{};
    rmt_carrier_config_t carrier{}; carrier.frequency_hz = 38000; carrier.duty_cycle = 0.33f;
    esp_err_t err = rmt_new_rx_channel(&rc, &receiver);
    if (err == ESP_OK) err = rmt_rx_register_event_callbacks(receiver, &callbacks, events);
    if (err == ESP_OK) err = rmt_new_tx_channel(&tc, &transmitter);
    if (err == ESP_OK) err = rmt_apply_carrier(transmitter, &carrier);
    if (err == ESP_OK) err = rmt_new_copy_encoder(&ec, &encoder);
    bool rx_on = false, tx_on = false;
    if (err == ESP_OK) { err = rmt_enable(receiver); rx_on = err == ESP_OK; }
    if (err == ESP_OK) { err = rmt_enable(transmitter); tx_on = err == ESP_OK; }
    rmt_symbol_word_t input[128]{}, signal[34]{};
    signal[0].level0 = 1; signal[0].duration0 = 9000; signal[0].duration1 = 4500;
    // 00/00 address and 00/00 command intentionally violate both complement pairs.
    for (int i = 1; i <= 32; ++i) { signal[i].level0 = 1; signal[i].duration0 = 560; signal[i].duration1 = 560; }
    signal[33].level0 = 1; signal[33].duration0 = 560; signal[33].duration1 = 1;
    rmt_receive_config_t receive{}; receive.signal_range_min_ns = 2000; receive.signal_range_max_ns = 60000000;
    rmt_transmit_config_t transmit{};
    if (err == ESP_OK) err = rmt_receive(receiver, input, sizeof(input), &receive);
    if (err == ESP_OK) err = rmt_transmit(transmitter, encoder, signal, sizeof(signal), &transmit);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(transmitter, 1000);
    rmt_rx_done_event_data_t event{};
    bool observed = false;
    if (err == ESP_OK) observed = xQueueReceive(events, &event, pdMS_TO_TICKS(1000)) == pdTRUE;
    unsigned pulses = 0;
    if (observed) for (size_t i = 0; i < event.num_symbols; ++i) pulses += (input[i].duration0 != 0) + (input[i].duration1 != 0);
    bool matched = observed && pulses == 67 && input[0].level0 == 0 &&
                   input[0].duration0 * kRxTickUs >= 8000 && input[0].duration0 * kRxTickUs <= 10000 &&
                   input[0].duration1 * kRxTickUs >= 4000 && input[0].duration1 * kRxTickUs <= 5000;
    if (matched) for (int i = 1; i <= 32; ++i)
        matched &= input[i].duration0 * kRxTickUs >= 350 && input[i].duration0 * kRxTickUs <= 800 &&
                   input[i].duration1 * kRxTickUs >= 350 && input[i].duration1 * kRxTickUs <= 800;
    printf("IR_LOOP probe status=%s observed=%d matched=%d symbols=%u pulses=%u\n", esp_err_to_name(err), observed, matched, unsigned(observed ? event.num_symbols : 0), pulses);
    if (rx_on) rmt_disable(receiver);
    if (tx_on) rmt_disable(transmitter);
    if (receiver) rmt_del_channel(receiver);
    if (transmitter) rmt_del_channel(transmitter);
    if (encoder) rmt_del_encoder(encoder);
    vQueueDelete(events);
    gpio_reset_pin(GPIO_NUM_10); gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT); gpio_set_level(GPIO_NUM_10, 0);
    fflush(stdout);
}

void loopback_trials(bool reuse, bool tx_low)
{
    if (rx || scope_running || monitor_running) { printf("IR_TRIAL error=busy\n"); fflush(stdout); return; }
    M5.Power.setExtOutput(true);
    if (M5.Ex_I2C.getPort() == M5.In_I2C.getPort()) { printf("IR_TRIAL error=shared_i2c\n"); fflush(stdout); return; }
    M5.Ex_I2C.release();
    QueueHandle_t events = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (!events) { printf("IR_TRIAL error=no_memory\n"); fflush(stdout); return; }
    rmt_channel_handle_t receiver = nullptr;
    rmt_rx_channel_config_t rc{}; rc.gpio_num = GPIO_NUM_11; rc.clk_src = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz = kRxResolution; rc.mem_block_symbols = 128;
    rmt_rx_event_callbacks_t callbacks{}; callbacks.on_recv_done = received;
    rmt_receive_config_t receive{}; receive.signal_range_min_ns = 2000; receive.signal_range_max_ns = 60000000;
    unsigned attempts = 0, passed = 0, first_failure = 0;
    bool long_matched = false, after_long_matched = false;
    esp_err_t err = ESP_OK;
    for (unsigned n = 1; n <= 102; ++n) {
        if (!receiver) {
            err = rmt_new_rx_channel(&rc, &receiver);
            if (err == ESP_OK) err = rmt_rx_register_event_callbacks(receiver, &callbacks, events);
            if (err == ESP_OK) err = rmt_enable(receiver);
            if (err != ESP_OK) break;
        }
        rmt_channel_handle_t transmitter = nullptr;
        rmt_encoder_handle_t encoder = nullptr;
        rmt_tx_channel_config_t tc{}; tc.gpio_num = GPIO_NUM_10; tc.clk_src = RMT_CLK_SRC_DEFAULT;
        tc.resolution_hz = kResolution; tc.mem_block_symbols = 64; tc.trans_queue_depth = 1;
        rmt_copy_encoder_config_t ec{};
        rmt_carrier_config_t carrier{}; carrier.frequency_hz = 38000; carrier.duty_cycle = 0.33f;
        err = rmt_new_tx_channel(&tc, &transmitter);
        if (err == ESP_OK) err = rmt_apply_carrier(transmitter, &carrier);
        if (err == ESP_OK) err = rmt_new_copy_encoder(&ec, &encoder);
        bool tx_on = false;
        if (err == ESP_OK) { err = rmt_enable(transmitter); tx_on = err == ESP_OK; }
        rmt_symbol_word_t input[700]{}, signal[491]{};
        const bool long_wave = n == 101;
        const int bit_count = long_wave ? 489 : 32;
        signal[0].level0 = 1; signal[0].duration0 = long_wave ? 4000 : 9000; signal[0].duration1 = long_wave ? 2000 : 4500;
        for (int i = 1; i <= bit_count; ++i) { signal[i].level0 = 1; signal[i].duration0 = 560; signal[i].duration1 = long_wave && (i % 2) ? 1690 : 560; }
        signal[bit_count + 1].level0 = 1; signal[bit_count + 1].duration0 = 560; signal[bit_count + 1].duration1 = 1;
        rmt_transmit_config_t transmit{};
        if (err == ESP_OK) err = rmt_receive(receiver, input, sizeof(input), &receive);
        if (err == ESP_OK) err = rmt_transmit(transmitter, encoder, signal, sizeof(signal[0]) * (bit_count + 2), &transmit);
        if (err == ESP_OK) err = rmt_tx_wait_all_done(transmitter, 1000);
        rmt_rx_done_event_data_t event{};
        bool observed = err == ESP_OK && xQueueReceive(events, &event, pdMS_TO_TICKS(1000)) == pdTRUE;
        unsigned pulses = 0;
        if (observed) for (size_t i = 0; i < event.num_symbols; ++i) pulses += (input[i].duration0 != 0) + (input[i].duration1 != 0);
        bool matched = observed && pulses == unsigned(2 * bit_count + 3) && input[0].level0 == 0 &&
            input[0].duration0 * kRxTickUs >= unsigned(long_wave ? 3000 : 8000) && input[0].duration0 * kRxTickUs <= unsigned(long_wave ? 5000 : 10000) &&
            input[0].duration1 * kRxTickUs >= unsigned(long_wave ? 1500 : 4000) && input[0].duration1 * kRxTickUs <= unsigned(long_wave ? 2500 : 5000);
        if (matched) for (int i = 1; i <= bit_count; ++i)
            matched &= input[i].duration0 * kRxTickUs >= 350 && input[i].duration0 * kRxTickUs <= 800 &&
                       input[i].duration1 * kRxTickUs >= unsigned(long_wave && (i % 2) ? 1400 : 350) &&
                       input[i].duration1 * kRxTickUs <= unsigned(long_wave && (i % 2) ? 1900 : 800);
        if (n <= 100) { ++attempts; if (matched) ++passed; else if (!first_failure) first_failure = n; }
        else if (n == 101) long_matched = matched;
        else after_long_matched = matched;
        printf("IR_TRIAL sample=%u observed=%d matched=%d pulses=%u status=%s\n", n, observed, matched, pulses, esp_err_to_name(err)); fflush(stdout);
        if (tx_on) rmt_disable(transmitter);
        if (transmitter) rmt_del_channel(transmitter);
        if (encoder) rmt_del_encoder(encoder);
        gpio_reset_pin(GPIO_NUM_10);
        gpio_set_direction(GPIO_NUM_10, tx_low ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
        if (tx_low) gpio_set_level(GPIO_NUM_10, 0);
        if (!reuse || !observed || err != ESP_OK) {
            rmt_disable(receiver); rmt_del_channel(receiver); receiver = nullptr;
        }
        if (reuse && !observed) break;
        if (err != ESP_OK) break;
    }
    if (receiver) { rmt_disable(receiver); rmt_del_channel(receiver); }
    vQueueDelete(events);
    gpio_reset_pin(GPIO_NUM_10); gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT); gpio_set_level(GPIO_NUM_10, 0);
    printf("IR_TRIAL done reuse=%d tx_low=%d attempts=%u passed=%u first_failure=%u long=%d after_long=%d status=%s\n",
           reuse, tx_low, attempts, passed, first_failure, long_matched, after_long_matched, esp_err_to_name(err)); fflush(stdout);
}

void session_result()
{
    const int count = session_count.load();
    if (session) { printf("IR_RESULT waiting\n"); }
    else if (count == -2) { printf("IR_RESULT skipped\n"); }
    else if (count < 0) { printf("IR_RESULT unavailable\n"); }
    else {
        printf("IR_RESULT symbols=%d overflow=%d raw=", count, count == int(kCaptureSymbols));
        for (int i = 0; i < count; ++i) {
            const auto &s = capture[i];
            if (s.duration0) printf("%u:%u,", unsigned(s.level0), unsigned(s.duration0) * kRxTickUs);
            if (s.duration1) printf("%u:%u,", unsigned(s.level1), unsigned(s.duration1) * kRxTickUs);
        }
        printf("\n");
    }
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
