// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ayousanz
//
// Ralomi 990k on ESP32-S3 with ESP-DL.
//
// Boot: load the .espdl from the "model" partition, compare against the test values ESP-PPQ
// embedded in it (Model::test), report memory and neural latency on that input.
// Then serve the host link on USB-Serial/JTAG: the host sends log-Mel frames, the board runs
// the model and prints the per-frame argmax so the host can compare with its ESP-PPQ simulation.
// "INFO" repeats the boot report and the memory minimum since serving started. A touch on the screen (or "MIC" over serial) records 4 s from the microphone and shows the kana.
// The model is either the 1500-frame export or its chunk rewrite (scripts/device/stream_espdl.py),
// which runs only the chunks that hold frames. Microphone PCM is normalized as each second arrives.
//
// Host -> board: "RLM1" | uint16 frames (1..1500, little endian) | frames*40 float32 features
// Host -> board: "RLYR" | same as RLM1: run module by module and print "LAYER <i> <op> <tensor> <dtype> <exp>
//                <size> <hex of the first 4096 elements>" per output, then "LAYERS_END <n>"
// Host -> board: "RLMA" | uint32 samples | uint8 dump | samples*int16 PCM: on-board log-Mel + model
//                (dump=1 also prints "FEATURES <n> <hex float32>" for frontend comparison)
// Host -> board: "RLMS" | 64000*int16 raw PCM: timed four-second replay of the microphone pipeline
// Board -> host: "RESULT <frames> <run_us> <argmax of the first frames/4 output steps...>"
//                "TEXT frames=<n> feature_us=<us> run_us=<us> <kana>"
//                after recording: "RAW_PCM <samples> <hex int16>" before gain,
//                "PCM <samples> <hex int16>" as used by inference,
//                "MIC seconds=<s> rms_dbfs=<dB> tail_us=<recording end to result>"
//
// Two modes, kept in NVS and switched by a long touch on the waiting screen or "MODE" (reboots):
// kana mode is the above (Ralomi stream100, label printing); action mode loads KoeAct-JP
// (Ralomi e13 stream40 from "koeact" + the decoder in "a2") and moves the on-screen character.
// Action mode: Host -> board "RLMA" | uint32 samples | uint8 0 | samples*int16 raw PCM runs the
//                same path as a recording; board -> host "KOEACT samples=<n> frames=<n>
//                run_us=<us> generate_us=<us> tail_us=<us> p_act=<p> accepted=<0|1> ids=<...>",
//                "KOEACT_JSON <json>", "KOEACT_KANA <kana>"
//              The action mode listens all the time (no button needed); "LPCM" | uint32 samples |
//                int16 PCM feeds the listener instead of the microphone, "LSTA" prints
//                "LISTEN_STAT ..." and the memory, which is also printed every minute; "LHLD" /
//                "LRUN" hold and resume the cutting for host tests (RLMA), and
//                "HOME_ACTION <json>" reports a home appliance command (home.cpp).

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <atomic>

#include <M5Unified.h>
#include "a2.h"
#include "action.h"
#include "tts.h"
#include "audio_gain.h"
#include "ble_printer.h"
#include "home.h"
#include "label_raster.h"

#include "dl_model_base.hpp"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "features.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "voice_screen.h"

namespace {

constexpr const char *TAG = "ralomi";
constexpr int kFrames = 1500;
constexpr int kBins = 40;
constexpr int kSubsampling = 4;
constexpr int kBenchRuns = 10;
constexpr int kMaxSamples = 240000;  // 15 s at 16 kHz
constexpr int kMicSeconds = 4;

const char *const kVocabulary[] = {
#include "vocabulary.inc"
};

// Boot lines are kept so "INFO" can repeat them: USB-Serial/JTAG output is lost until the host opens the port.
std::string g_boot_report;
std::string g_last_text;
constexpr int kBleCycles = 10;  // SOL_BRIEF USB and battery acceptance count.
constexpr int kMemoryLineBytes = 192;
// Per cycle: model, two recordings (2 phases each), six BLE phases, summary.
constexpr size_t kCycleTraceBytes = (kBleCycles * (1 + 2 + 6 + 2 + 1) + 2) * kMemoryLineBytes;
std::unique_ptr<char, decltype(&heap_caps_free)> g_cycle_trace(nullptr, heap_caps_free);
size_t g_cycle_trace_size = 0;
bool g_tracing = false, g_trace_overflow = false, g_battery_armed = false;
label_raster::Label g_confirm_label;
// The choices on the preview list: the recognized text's 6 styles, or a preset's 9.
constexpr int kMaxChoices = 9;
label_raster::Label g_styles[kMaxChoices];
const char *const *g_style_names = nullptr;
int g_style = 0, g_style_count = 0;
// Human-requested presets, printable from the waiting screen; only presets also print in kanji.
struct Preset { const char *kana, *name, *kanji; };
constexpr Preset kPresets[] = {
#if __has_include("presets_local.h") && !defined(BOTWATCH_SAMPLE_PRESETS)
#include "presets_local.h"
#else
#include "presets_local.example.h"
#endif
};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
label_raster::Label g_people[kPresetCount];
const char *g_people_names[kPresetCount];
int g_person = 0;
bool g_quiet_warning = false;
constexpr int kPrintHoldMs = 700;  // Human-approved blue long press, about 0.7 s.
StaticStreamBuffer_t serial_buffer_storage;
StreamBufferHandle_t serial_commands = nullptr;
void print_hex(const uint8_t *bytes, size_t size);
struct ProductCapture {
    int16_t *raw = nullptr;
    std::atomic<int> completed{0};
    std::atomic<int64_t> completed_us{0};
    std::atomic<bool> accepting{false}, finished{false}, failed{false};
} g_product;
std::atomic<bool> g_product_active{false};

// Which model this boot runs; stored in NVS so a switch survives the reboot it needs (the two
// models do not fit PSRAM together).
enum class Mode : uint8_t { kana = 0, action = 1 };
Mode g_mode = Mode::kana;
constexpr const char *kModeNamespace = "voice", *kModeKey = "mode";
constexpr const char *kCharacterKey="character";
constexpr int kModeHoldMs = 1500;  // long touch on the waiting screen switches the mode

Mode load_mode()
{
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open(kModeNamespace, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, kModeKey, &value);
        uint8_t character=0;
        nvs_get_u8(handle,kCharacterKey,&character);
        voice_screen::restore_character(character);
        nvs_close(handle);
    }
    return value == 1 ? Mode::action : Mode::kana;
}

// Voice printing: the action mode (KoeAct) hears "印刷して" and reboots into the kana mode
// (Ralomi) with this flag, which then takes the words and the go-ahead by voice and prints.
constexpr const char *kVoicePrintKey = "voiceprint";
bool g_voice_print = false;  // this boot is a voice printing session

bool take_voice_print_flag()
{
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open(kModeNamespace, NVS_READWRITE, &handle) != ESP_OK) { return false; }
    nvs_get_u8(handle, kVoicePrintKey, &value);
    if (value) { nvs_set_u8(handle, kVoicePrintKey, 0); nvs_commit(handle); }  // once: a crash must not loop
    nvs_close(handle);
    return value != 0;
}

void pause_listening();
[[noreturn]] void reboot_into(Mode next, bool voice_print)
{
    pause_listening();
    voice_screen::mode_switching(voice_print ? "いんさつのじゅんび中…" : "ちょっとまってね");
    nvs_handle_t handle;
    esp_err_t saved = nvs_open(kModeNamespace, NVS_READWRITE, &handle);
    if (saved == ESP_OK) {
        saved = nvs_set_u8(handle, kModeKey, (uint8_t)next);
        if (saved == ESP_OK) { saved = nvs_set_u8(handle, kVoicePrintKey, voice_print ? 1 : 0); }
        if (saved == ESP_OK) { saved = nvs_set_u8(handle,kCharacterKey,voice_screen::selected_character()); }
        if (saved == ESP_OK) { saved = nvs_commit(handle); }
        nvs_close(handle);
    }
    printf("MODE_SWITCH next=%s voice_print=%d saved=%s\n", next == Mode::action ? "action" : "kana", voice_print,
           esp_err_to_name(saved));
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

[[noreturn]] void switch_mode() { reboot_into(g_mode == Mode::kana ? Mode::action : Mode::kana, false); }

// "ラベルを印刷して", "いんさつして" ... in the kana KoeAct heard; "印刷しないで" is not.
// The first mora wavers ("らべるをぴんさつして" on the board), so "んさつ" is what is looked for.
bool wants_voice_print(const std::string &kana)
{
    return kana.find("んさつ") != std::string::npos && kana.find("ないで") == std::string::npos;
}

// True once a touch has stayed down for kModeHoldMs (call every loop; a lift starts over).
struct LongTouch {
    int64_t since = 0;
    bool held()
    {
        if (M5.Touch.getCount() == 0) { since = 0; return false; }
        const int64_t now = esp_timer_get_time();
        if (since == 0) { since = now; }
        return now - since >= kModeHoldMs * 1000LL;
    }
};

// KoeAct-JP decoder (main/koeact/a2.c). Weights stay memory-mapped in the "a2" partition; the
// arena (activations, keys, values) holds kA2MaxSteps Ralomi steps: 0.5 s lead + at most
// kActionSeconds + 0.3 s tail, in 40 ms steps.
constexpr int kActionSeconds = 8;
constexpr int kLeadSamples = 8000, kTailSamples = 4800;  // KoeAct clean(): 0.5 s, 0.3 s
constexpr int kA2MaxSteps = 240;
static_assert((kLeadSamples + kActionSeconds * 16000 + kTailSamples) / 160 <= kA2MaxSteps * 4,
              "action recording must fit the decoder arena");
a2_model g_a2;
void *g_a2_arena = nullptr;
float *g_a2_chunk = nullptr;
bool g_a2_ready = false;

// Action mode listens all the time: the microphone fills a ring of kMaxSamples, one feature hop
// per request, and the serving loop cuts utterances out of it. Nothing is written to flash.
struct Listener {
    int16_t *ring = nullptr;
    std::atomic<int64_t> written{0};  // samples written since listening began (monotonic)
    std::atomic<bool> active{false}, mic_on{false}, failed{false};
} g_listen;
std::atomic<bool> g_restore_fault{false};
int16_t *g_microphone_warmup=nullptr;

void microphone_released(void *, void *data, size_t length)
{
    if (g_listen.active.load()) {
        const int64_t written = g_listen.written.load() + (int64_t)length;
        g_listen.written.store(written);
        if (g_listen.mic_on.load() &&
            !M5.Mic.record(g_listen.ring + written % kMaxSamples, ralomi::kHop, ralomi::kSampleRate)) {
            g_listen.failed = true;
        }
        return;
    }
    if (!g_product_active.load()) { return; }
    const int end = static_cast<int16_t *>(data) - g_product.raw + length;
    g_product.completed = end;
    g_product.completed_us = esp_timer_get_time();
    const int maximum = kMaxSamples;
    if (g_product.accepting.load() && end < maximum) {
        // One feature hop per request gives a 10ms stop boundary. The capture
        // task chains requests continuously even while the model is running.
        if (M5.Mic.record(g_product.raw + end, std::min(ralomi::kHop, maximum - end), ralomi::kSampleRate)) { return; }
        g_product.failed = true;
    }
    g_product.finished = true;
}

void trace_line(const char *line)
{
    if (!g_tracing) { return; }
    const size_t length = std::strlen(line);
    if (g_cycle_trace_size + length + 1 >= kCycleTraceBytes) { g_trace_overflow = true; return; }
    std::memcpy(g_cycle_trace.get() + g_cycle_trace_size, line, length);
    g_cycle_trace_size += length;
    g_cycle_trace.get()[g_cycle_trace_size++] = '\n';
}

void cycle_report(const char *format, ...)
{
    char line[kMemoryLineBytes];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0 || length >= (int)sizeof(line)) { g_trace_overflow = true; return; }
    trace_line(line);
    printf("%s\n", line);
}

void report(const char *format, ...)
{
    char line[kMemoryLineBytes];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", line);
    g_boot_report += line;
    g_boot_report += '\n';
}

// The *_min values count from boot until serving starts; from then on they count from serving start only
// (ESP-DL fills internal RAM first while loading parameters, so the load-time minimum is near zero).
void log_memory(const char *stage, bool keep = true)
{
    char line[192];
    snprintf(line,
             sizeof(line),
             "MEM %s internal_free=%u internal_largest=%u internal_min=%u psram_free=%u psram_min=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    trace_line(line);
    if (keep) {
        report("%s", line);
    } else {
        printf("%s\n", line);
        fflush(stdout);
    }
}

bool read_exact(uint8_t *buffer, size_t size)
{
    size_t received = 0;
    while (received < size) {
        int n = xStreamBufferReceive(serial_commands, buffer + received, size - received, pdMS_TO_TICKS(2000));
        if (n <= 0) {
            return false;
        }
        received += n;
    }
    return true;
}

void serial_receiver(void *)
{
    // A single USB reader owns framing. Binary payloads are forwarded by length,
    // so PCM bytes can never be mistaken for an asynchronous screen request.
    auto receive = [](uint8_t *data, size_t size) {
        size_t offset = 0;
        while (offset < size) {
            const int n = usb_serial_jtag_read_bytes(data + offset, size - offset, pdMS_TO_TICKS(2000));
            if (n <= 0) { return false; }
            offset += n;
        }
        return true;
    };
    auto forward = [](const uint8_t *data, size_t size) {
        size_t offset = 0;
        while (offset < size) { offset += xStreamBufferSend(serial_commands, data + offset, size - offset, portMAX_DELAY); }
    };
    uint8_t header[4] = {}, payload[512];
    while (true) {
        if (usb_serial_jtag_read_bytes(header, 1, portMAX_DELAY) != 1) { continue; }
        size_t length = header[0] == 'M' ? 3 : 4;
        if (!receive(header + 1, length - 1)) { continue; }
        // "MIC" is the only 3-byte command; "MODE" shares its first letter.
        if (length == 3 && memcmp(header, "MOD", 3) == 0) {
            if (!receive(header + 3, 1)) { continue; }
            length = 4;
        }
        if (length == 4 && memcmp(header, "LFRM", 4) == 0) {
            voice_screen::dump_pixels(print_hex); continue;
        }
        if (length == 4 && (!memcmp(header, "TMUT", 4) || !memcmp(header, "TLIV", 4))) {
            tts::diagnostic(header[1] == 'M'); continue;
        }
        if (length == 4 && !memcmp(header,"TFAL",4)) {
            g_restore_fault=true;
            printf("TTS_RESTORE_FAULT armed=1\n");fflush(stdout);continue;
        }
        forward(header, length);
        size_t bytes = 0;
        if (memcmp(header, "RLMA", 4) == 0) {
            uint8_t size[5]; if (!receive(size, 5)) { continue; } forward(size, 5);
            const uint32_t samples = uint32_t(size[0]) | uint32_t(size[1]) << 8 | uint32_t(size[2]) << 16 | uint32_t(size[3]) << 24;
            if (samples <= kMaxSamples) { bytes = samples * sizeof(int16_t); }
        } else if (memcmp(header, "RLM1", 4) == 0 || memcmp(header, "RLYR", 4) == 0) {
            uint8_t size[2]; if (!receive(size, 2)) { continue; } forward(size, 2);
            const int frames = size[0] | size[1] << 8;
            if (frames <= kFrames) { bytes = frames * kBins * sizeof(float); }
        } else if (memcmp(header, "RLMS", 4) == 0) { bytes = kMicSeconds * ralomi::kSampleRate * sizeof(int16_t); }
        else if (memcmp(header, "LPRS", 4) == 0 || memcmp(header, "PPRS", 4) == 0) { bytes = 1; }  // style index
        else if (memcmp(header, "LPCM", 4) == 0) {  // action mode listener feed: uint32 samples | PCM
            uint8_t size[4]; if (!receive(size, 4)) { continue; } forward(size, 4);
            bytes = (size_t(size[0]) | size_t(size[1]) << 8 | size_t(size[2]) << 16 | size_t(size[3]) << 24) * sizeof(int16_t);
        }
        else if (memcmp(header,"CTCH",4)==0) { bytes=11; }
        // Forward whatever has arrived: waiting for a whole 512-byte block dropped a short tail
        // when the host paused (it does, between the action mode's paced LPCM chunks).
        while (bytes) {
            const int n = usb_serial_jtag_read_bytes(payload, std::min(bytes, sizeof(payload)), pdMS_TO_TICKS(10000));
            if (n <= 0) { break; }
            forward(payload, n); bytes -= n;
        }
    }
}

template <typename T>
void quantize_input(dl::TensorBase *input, const float *features, int frames)
{
    T *data = input->get_element_ptr<T>();
    const float inv_scale = std::ldexp(1.0f, -input->get_exponent());
    std::fill(data, data + input->get_size(), 0);
    for (int i = 0; i < frames * kBins; ++i) {
        data[i] = dl::quantize<T>(features[i], inv_scale);
    }
}

// Appends in place: the recognizer reserves the whole utterance once, so a chunk allocates nothing
// (the action mode runs this all day).
template <typename T>
void append_argmax(dl::TensorBase *output, int steps, std::vector<int> &out)
{
    const int vocabulary = output->shape.back();
    const T *data = output->get_element_ptr<T>();
    for (int t = 0; t < steps; ++t) {
        const T *row = data + t * vocabulary;
        out.push_back((int)(std::max_element(row, row + vocabulary) - row));
    }
}

int64_t run_timed(dl::Model *model)
{
    const int64_t start = esp_timer_get_time();
    model->run();
    return esp_timer_get_time() - start;
}

void quantize_features(dl::TensorBase *input, const float *features, int frames)
{
    if (input->get_dtype() == dl::DATA_TYPE_INT8) {
        quantize_input<int8_t>(input, features, frames);
    } else {
        quantize_input<int16_t>(input, features, frames);
    }
}

// Runs either model kind. A single-input model takes up to 1500 frames at once. A chunk model
// (scripts/device/stream_espdl.py) takes log_mel_features [1, chunk, 40] plus state_<b> and returns
// next_state_<b>; feeding its chunks in order from zero state gives the same output steps as the
// 1500-frame model on the same zero-padded input, because the network is causal.
class Recognizer {
public:
    explicit Recognizer(dl::Model *model) : m_model(model)
    {
        std::map<std::string, dl::TensorBase *> &inputs = model->get_inputs();
        std::map<std::string, dl::TensorBase *> &outputs = model->get_outputs();
        m_features = inputs.at("log_mel_features");
        m_logits = outputs.at("logits");
        // KoeAct's export also returns the layer 4 and layer 8 frames the Action decoder reads.
        if (outputs.count("layer4") && outputs.count("layer8") &&
            outputs.at("layer4")->get_dtype() == dl::DATA_TYPE_INT16 &&
            outputs.at("layer8")->get_dtype() == dl::DATA_TYPE_INT16) {
            m_layer4 = outputs.at("layer4");
            m_layer8 = outputs.at("layer8");
        }
        for (auto &[name, tensor] : inputs) {
            if (name.rfind("state_", 0) != 0) {
                continue;
            }
            dl::TensorBase *next = outputs.at("next_" + name);
            // The state is copied as raw integers, which is exact only with matching quantization.
            assert(next->get_exponent() == tensor->get_exponent() && next->get_size() == tensor->get_size() &&
                   next->get_dtype() == tensor->get_dtype());
            m_states.push_back({tensor, next, std::vector<uint8_t>(tensor->get_size() * tensor->get_dtype_bytes())});
        }
        m_argmax.reserve(kFrames / kSubsampling);
    }

    dl::Model *model() const { return m_model; }
    dl::TensorBase *features_input() const { return m_features; }
    int chunk_frames() const { return m_features->shape[1]; }
    int state_count() const { return (int)m_states.size(); }
    bool has_layers() const { return m_layer4 != nullptr; }
    // Steps encoded into the decoder arena this utterance; -1 once a chunk could not be.
    int layer_steps() const { return m_layer_steps; }
    int64_t encode_us() const { return m_encode_us; }

    void reset()
    {
        for (State &state : m_states) {
            std::fill(state.saved.begin(), state.saved.end(), 0);
        }
        m_argmax.clear();
        m_run_us = 0;
        m_layer_steps = 0;
        m_encode_us = 0;
    }

    // Run one chunk of at most chunk_frames() frames (zero-padded) and keep its frames / 4 steps.
    // Outputs are read before the next chunk's inputs are written: the planner may share memory.
    void feed(const float *features, int frames)
    {
        load_inputs(features, frames);
        m_run_us += run_timed(m_model);
        const int n = frames / kSubsampling;
        if (m_logits->get_dtype() == dl::DATA_TYPE_INT8) { append_argmax<int8_t>(m_logits, n, m_argmax); }
        else { append_argmax<int16_t>(m_logits, n, m_argmax); }
        if (has_layers() && m_layer_steps >= 0) {
            // Encode this chunk right away, as KoeAct's CoreS3 firmware does, so only the greedy
            // decoding is left after the last chunk.
            if (g_a2_ready && m_layer_steps + n <= kA2MaxSteps) {
                const int64_t start = esp_timer_get_time();
                a2_put_layers(g_a2_chunk, 0, n, m_layer4->get_element_ptr<int16_t>(), m_layer4->get_exponent(),
                              m_layer8->get_element_ptr<int16_t>(), m_layer8->get_exponent());
                a2_encode(&g_a2, g_a2_chunk, m_layer_steps, n, kA2MaxSteps, g_a2_arena);
                m_encode_us += esp_timer_get_time() - start;
                m_layer_steps += n;
            } else {
                m_layer_steps = -1;
            }
        }
        for (State &state : m_states) {
            std::memcpy(state.saved.data(), state.output->get_element_ptr(), state.saved.size());
        }
    }

    // Write one chunk of features and the saved state into the model inputs.
    void load_inputs(const float *features, int frames)
    {
        quantize_features(m_features, features, frames);
        for (State &state : m_states) {
            std::memcpy(state.input->get_element_ptr(), state.saved.data(), state.saved.size());
        }
    }

    // reset() and feed() every chunk of `frames` frames.
    void run_all(const float *features, int frames)
    {
        reset();
        for (int offset = 0; offset < frames; offset += chunk_frames()) {
            feed(features + offset * kBins, std::min(chunk_frames(), frames - offset));
        }
    }

    const std::vector<int> &argmax() const { return m_argmax; }
    int64_t run_us() const { return m_run_us; }

private:
    struct State {
        dl::TensorBase *input;
        dl::TensorBase *output;
        std::vector<uint8_t> saved;
    };
    dl::Model *m_model;
    dl::TensorBase *m_features;
    dl::TensorBase *m_logits;
    dl::TensorBase *m_layer4 = nullptr, *m_layer8 = nullptr;
    int m_layer_steps = 0;
    int64_t m_encode_us = 0;
    std::vector<State> m_states;
    std::vector<int> m_argmax;
    int64_t m_run_us = 0;
};

void benchmark(Recognizer &recognizer)
{
    dl::Model *model = recognizer.model();
    for (int i = 0; i < 2; ++i) {
        model->run();
    }
    std::vector<int64_t> runs;
    for (int i = 0; i < kBenchRuns; ++i) {
        runs.push_back(run_timed(model));
    }
    std::sort(runs.begin(), runs.end());
    const int frames = recognizer.chunk_frames();
    report("BENCH frames=%d runs=%d min_us=%" PRId64 " p50_us=%" PRId64 " max_us=%" PRId64 " RTF=%.4f",
           frames,
           kBenchRuns,
           runs.front(),
           runs[runs.size() / 2],
           runs.back(),
           runs[runs.size() / 2] / (frames * 1e4));
}

struct Recognition {
    int frames = 0;
    int64_t feature_us = 0;
    int64_t run_us = 0;
    std::vector<int> argmax;
    std::string text;
};

// Greedy CTC decoding (blank 0, <unk> 1) of the recognizer's output steps.
Recognition finish(const Recognizer &recognizer, int frames)
{
    Recognition result;
    result.frames = frames;
    result.run_us = recognizer.run_us();
    result.argmax = recognizer.argmax();
    int previous = -1;
    for (int index : result.argmax) {
        if (index != previous && index > 1) {
            result.text += kVocabulary[index];
        }
        previous = index;
    }
    return result;
}

Recognition recognize(Recognizer &recognizer, const float *features, int frames)
{
    recognizer.run_all(features, frames);
    return finish(recognizer, frames);
}

// Bulk hex goes straight to the driver: the VFS drops characters once the host pauses for 50 ms,
// which corrupts long lines. Here each chunk may wait up to 500 ms; if the host still does not
// read, the rest of the line is dropped so the board never stalls on an absent host.
void print_hex(const uint8_t *bytes, size_t size)
{
    fflush(stdout);
    char chunk[512];
    bool connected = true;
    for (size_t offset = 0; offset < size && connected; offset += sizeof(chunk) / 2) {
        const size_t count = std::min(size - offset, sizeof(chunk) / 2);
        for (size_t i = 0; i < count; ++i) {
            static const char kDigits[] = "0123456789abcdef";
            chunk[2 * i] = kDigits[bytes[offset + i] >> 4];
            chunk[2 * i + 1] = kDigits[bytes[offset + i] & 15];
        }
        connected = usb_serial_jtag_write_bytes(chunk, 2 * count, pdMS_TO_TICKS(500)) == (int)(2 * count);
    }
}

void print_tensor(int index, const std::string &operation, const std::string &name, dl::TensorBase *tensor)
{
    constexpr int kProbeElements = 4096;
    if (tensor == nullptr) {
        printf("LAYER %d %s %s missing\n", index, operation.c_str(), name.c_str());
        return;
    }
    const int count = std::min(tensor->get_size(), kProbeElements);
    printf("LAYER %d %s %s %s %d %d ",
           index,
           operation.c_str(),
           name.c_str(),
           dl::dtype_to_string(tensor->get_dtype()),
           tensor->get_exponent(),
           tensor->get_size());
    print_hex(static_cast<const uint8_t *>(tensor->get_element_ptr()), count * tensor->get_dtype_bytes());
    printf("\n");
}

// Run the execution plan one module at a time and print the leading elements of every output,
// so the host can find the first operation where the board departs from the ESP-PPQ simulation.
// A chunk model probes its first chunk from zero state.
void probe_layers(Recognizer &recognizer, const float *features, int frames)
{
    dl::Model *model = recognizer.model();
    recognizer.reset();
    recognizer.load_inputs(features, std::min(frames, recognizer.chunk_frames()));
    fbs::FbsModel *fbs_model = model->get_fbs_model();
    fbs_model->load_map();
    const std::vector<std::string> operations = fbs_model->topological_sort();
    print_tensor(-1, "Input", "log_mel_features", recognizer.features_input());
    for (int i = 0; i < (int)operations.size(); ++i) {
        model->run((int)operations.size(), i);
        std::vector<std::string> inputs, outputs;
        fbs_model->get_operation_inputs_and_outputs(operations[i], inputs, outputs);
        for (const std::string &name : outputs) {
            print_tensor(i, fbs_model->get_operation_type(operations[i]), name, model->get_intermediate(name));
        }
    }
    fbs_model->clear_map();
    printf("LAYERS_END %d\n", (int)operations.size());
    fflush(stdout);
}

void print_result(const Recognition &result)
{
    printf("RESULT %d %" PRId64, result.frames, result.run_us);
    for (int index : result.argmax) {
        printf(" %d", index);
    }
    printf("\nTEXT frames=%d feature_us=%" PRId64 " run_us=%" PRId64 " %s\n",
           result.frames,
           result.feature_us,
           result.run_us,
           result.text.c_str());
    fflush(stdout);
}

// PCM in [-1, 1] (at most 15 s) -> on-board log-Mel -> recognition.
Recognition recognize_pcm(Recognizer &recognizer, const float *pcm, int samples, std::vector<float> &features)
{
    samples = std::min(samples, kMaxSamples);
    const int frames = std::min(ralomi::frame_count(samples), kFrames);
    const int64_t start = esp_timer_get_time();
    ralomi::log_mel(pcm, samples, features.data());
    const int64_t feature_us = esp_timer_get_time() - start;
    Recognition result = recognize(recognizer, features.data(), frames);
    result.feature_us = feature_us;
    return result;
}

// int16 PCM is stored in the upper half of the float buffer and widened in place: float i only
// overwrites int16 slots 2i and 2i+1, below every slot still to be read (kMaxSamples + j, j > i).
// This avoids separate 480 KB (serial) and 128 KB (microphone) int16 buffers in PSRAM.
int16_t *pcm16_area(std::vector<float> &pcm)
{
    return reinterpret_cast<int16_t *>(pcm.data()) + kMaxSamples;
}

void widen_pcm16(std::vector<float> &pcm, int samples, int first = 0)
{
    const int16_t *pcm16 = pcm16_area(pcm);
    for (int i = first; i < samples; ++i) {
        const int16_t sample = pcm16[i];
        pcm[i] = sample / 32768.0f;
    }
}

void show(const char *status, const std::string &text)
{
    voice_screen::show(status, text);
}

bool prepare_microphone(std::vector<float> &pcm)
{
    show("マイク準備中…", "まだ話さないでください");
    auto microphone = M5.Mic.config();
    const unsigned previous_gain = microphone.magnification;
    // Capture before software saturation; bounded peak gain runs on each
    // complete second, before either features or inference.
    microphone.magnification = 1;
    M5.Mic.config(microphone);
    M5.Mic.setBufferReleaseCallback(nullptr, microphone_released);
    report("MIC_GAIN software=%u previous=%u", (unsigned)microphone.magnification, previous_gain);
    const bool began = M5.Mic.begin();
    report("MIC_BEGIN ok=%d enabled=%d", began, M5.Mic.isEnabled());
    if (!began) {
        show("マイク初期化失敗", "電源を入れ直してください");
        return false;
    }
    // ES8311 needs about one second after power-up (Mic_Class.hpp). Discard it
    // before inviting speech; keep the codec running for subsequent recordings.
    int16_t *discard = pcm16_area(pcm);
    g_microphone_warmup=discard;
    const bool queued = M5.Mic.record(discard, ralomi::kSampleRate, ralomi::kSampleRate);
    report("MIC_WARMUP queued=%d samples=%d", queued, ralomi::kSampleRate);
    if (!queued) {
        M5.Mic.end();
        show("録音の準備に失敗", "電源を入れ直してください");
        return false;
    }
    while (M5.Mic.isRecording()) {
        vTaskDelay(1);
    }
    int leading_zero = 0;
    while (leading_zero < ralomi::kSampleRate && discard[leading_zero] == 0) {
        ++leading_zero;
    }
    report("MIC_WARMUP discarded=%d leading_zero=%d", ralomi::kSampleRate, leading_zero);
    if (leading_zero == ralomi::kSampleRate) {
        M5.Mic.end();
        show("マイクが無音です", "電源を入れ直してください");
        return false;
    }
    return true;
}

// Both real microphone and timed saved-PCM input use the same stream processor.
// Diagnostics leave the USB port only after the result has reached the screen.
bool recognize_microphone(Recognizer &recognizer, std::vector<float> &pcm, std::vector<float> &features,
                          int16_t *raw, bool replay = false)
{
    constexpr int kSamples = kMicSeconds * ralomi::kSampleRate;
    // Widening stays below the int16 area, so the recording can still be printed afterwards.
    static_assert(2 * kSamples <= kMaxSamples, "microphone recording must fit the lower half");
    int16_t *recorded = pcm16_area(pcm);
    voice_screen::recording();
    log_memory("recording", false);
    const int chunk_frames = recognizer.chunk_frames();
    const int chunk_samples = chunk_frames * ralomi::kHop;
    // Bound input availability by the requested 500ms tail window. Whole-second
    // input makes even one discarded leading zero defer a complete normalized
    // second, leaving an extra model run after recording ends.
    const int input_samples = ralomi::kSampleRate / 2;
    if (!replay && (!M5.Mic.isRunning() || !M5.Mic.record(raw, input_samples, ralomi::kSampleRate))) {
        printf("ERROR MIC record_begin\n");
        show("録音に失敗", "電源を入れ直してください");
        return false;
    }
    audio_gain::Stream stream;
    recognizer.reset();
    int widened = 0, computed = 0, fed = 0;
    int64_t tail_start = 0, feature_us = 0;
    TickType_t replay_tick = xTaskGetTickCount();
    for (int end = input_samples; end <= kSamples; end += input_samples) {
        const bool more = end < kSamples;
        if (replay) {
            // Exercise the same input cadence as the real microphone.
            vTaskDelayUntil(&replay_tick, pdMS_TO_TICKS(input_samples * 1000 / ralomi::kSampleRate));
        } else {
            if (more && !M5.Mic.record(raw + end, std::min(input_samples, kSamples - end), ralomi::kSampleRate)) {
                while (M5.Mic.isRecording()) { vTaskDelay(1); }
                printf("ERROR MIC record_queue\n");
                show("録音に失敗", "もう一度タップしてください");
                return false;
            }
            while (M5.Mic.isRecording() > (more ? 1 : 0)) { vTaskDelay(1); }
        }
        if (!more) { tail_start = esp_timer_get_time(); }
        const int ready = stream.append(raw, end, recorded, kSamples, chunk_samples);
        widen_pcm16(pcm, ready, widened);
        widened = ready;
        const int frames = ralomi::frame_count(ready);
        if (frames > computed) {
            const int64_t start = esp_timer_get_time();
            ralomi::log_mel(pcm.data() + computed * ralomi::kHop,
                            ready - computed * ralomi::kHop, features.data() + computed * kBins);
            feature_us += esp_timer_get_time() - start;
            computed = frames;
        }
        while (computed - fed >= chunk_frames || (!more && computed > fed)) {
            const int count = std::min(chunk_frames, computed - fed);
            recognizer.feed(features.data() + fed * kBins, count);
            fed += count;
        }
    }
    Recognition result = finish(recognizer, computed);
    result.feature_us = feature_us;
    g_last_text = result.text;
    voice_screen::result(g_last_text);
    const int64_t tail_us = esp_timer_get_time() - tail_start;
    double raw_energy = 0;
    int raw_clipped = 0;
    for (int i = 0; i < kSamples; ++i) {
        raw_energy += (double)raw[i] * raw[i];
        raw_clipped += raw[i] <= audio_gain::kLow || raw[i] >= audio_gain::kHigh;
    }
    printf("RAW_PCM %d ", kSamples);
    print_hex(reinterpret_cast<const uint8_t *>(raw), kSamples * sizeof(int16_t));
    printf("\nMIC_RAW rms_dbfs=%.1f clipped=%d\n",
           10.0 * std::log10(raw_energy / kSamples / (32768.0 * 32768.0) + 1e-12), raw_clipped);
    printf("MIC_NORMALIZE leading_zero=%d chunk_samples=%d maximum_gain=%.0f\n",
           stream.leading(), chunk_samples, audio_gain::kMaximumGain);
    double energy = 0;
    for (int i = 0; i < kSamples; ++i) {
        energy += (double)pcm[i] * pcm[i];
    }
    const double dbfs = 10.0 * std::log10(energy / kSamples + 1e-12);
    // The recording leaves the board for host-side recognition of the same audio.
    printf("PCM %d ", kSamples);
    print_hex(reinterpret_cast<const uint8_t *>(recorded), kSamples * sizeof(int16_t));
    printf("\n");
    printf("MIC seconds=%d rms_dbfs=%.1f tail_us=%" PRId64 "\n", kMicSeconds, dbfs, tail_us);
    print_result(result);
    log_memory("after_recording", false);
    return true;
}

void run_ble_cycles(Recognizer &recognizer, std::vector<float> &pcm, std::vector<float> &features, int16_t *raw)
{
    g_cycle_trace.reset(static_cast<char *>(heap_caps_malloc(kCycleTraceBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!g_cycle_trace) { printf("ERROR cycle_trace allocation\n"); return; }
    g_cycle_trace_size = 0;
    g_trace_overflow = false;
    g_tracing = true;
    cycle_report("CYCLES_BEGIN total=%d usb=%d battery_mv=%d vbus_mv=%d", kBleCycles,
                 usb_serial_jtag_is_connected(), M5.Power.getBatteryVoltage(), M5.Power.getVBUSVoltage());
    uint32_t previous_minimum = 0;
    bool previous_decline = false;
    int completed = 0;
    for (int index = 0; index < kBleCycles; ++index) {
        // A cumulative minimum can never rise; reset the local monitor so
        // successive cycles, rather than boot/model allocation, are compared.
        heap_caps_monitor_local_minimum_free_size_stop();
        heap_caps_monitor_local_minimum_free_size_start();
        log_memory("cycle_model_loaded", false);
        const bool recognized = recognize_microphone(recognizer, pcm, features, raw);
        const bool queried = recognized && !g_trace_overflow && ble_printer::query(log_memory);
        const bool rerecorded = queried && !g_trace_overflow && recognize_microphone(recognizer, pcm, features, raw);
        const uint32_t minimum = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        const bool decline = previous_minimum != 0 && minimum < previous_minimum;
        const bool continued_decline = decline && previous_decline;
        const bool passed = rerecorded && !continued_decline && !g_trace_overflow;
        cycle_report("CYCLE index=%d status=%s minimum=%u end_free=%u end_largest=%u usb=%d battery_mv=%d vbus_mv=%d",
                     index + 1, passed ? "PASS" : "FAIL", (unsigned)minimum,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     usb_serial_jtag_is_connected(), M5.Power.getBatteryVoltage(), M5.Power.getVBUSVoltage());
        if (continued_decline) { cycle_report("CYCLE_STOP reason=minimum_continues_declining"); }
        if (!passed) { break; }
        ++completed;
        previous_minimum = minimum;
        previous_decline = decline;
    }
    cycle_report("CYCLES_END completed=%d required=%d trace_overflow=%d", completed, kBleCycles, g_trace_overflow);
    g_tracing = false;
    char count[24];
    snprintf(count, sizeof(count), "%d/%d", completed, kBleCycles);
    show(completed == kBleCycles && !g_trace_overflow ? "試験完了" : "試験失敗", count);
}

bool print_cancelled()
{
    M5.update();
    return M5.BtnA.wasPressed();
}

void print_started()
{
    voice_screen::printing(true);
    printf("UI_STATE printing_data\n");
}

ble_printer::PrintResult send_label(const label_raster::Label &label, bool dump, bool interactive)
{
    const size_t bytes = 15 + 4 + 8 + label.bytes() + 3 + 4;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> job(
        static_cast<uint8_t *>(heap_caps_calloc(bytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!job) { printf("ERROR label_job_allocation\n"); return {false, 0, "label_job_allocation", -1, -1}; }
    const uint8_t enable[] = {0x10, 0xff, 0xf1, 0x02};
    const uint8_t header[] = {0x1d, 0x76, 0x30, 0, 12, 0, uint8_t(label.rows & 255), uint8_t(label.rows >> 8)};
    const uint8_t tail[] = {0x1b, 0x4a, label_raster::kTailRows, 0x10, 0xff, 0xf1, 0x45};
    std::memcpy(job.get() + 15, enable, sizeof(enable));
    std::memcpy(job.get() + 19, header, sizeof(header));
    std::memcpy(job.get() + 27, label.bits.get(), label.bytes());
    std::memcpy(job.get() + 27 + label.bytes(), tail, sizeof(tail));
    if (dump) { printf("PRINT_JOB "); print_hex(job.get(), bytes); printf("\n"); fflush(stdout); }
    log_memory("label_job", false);
    return ble_printer::print(log_memory, job.get(), bytes,
                              interactive ? print_cancelled : nullptr, interactive ? print_started : nullptr);
}

void label_diagnostic(const std::string &text, bool print)
{
    label_raster::Label label;
    std::string error;
    if (!label_raster::render(text, label, error)) { printf("ERROR label_render %s\n", error.c_str()); return; }
    log_memory("label_rendered", false);
    printf("LABEL width=96 rows=%d bytes=%u paper_mm=%.3f\n", label.rows, (unsigned)label.bytes(), label.paper_mm());
    printf("LABEL_TEXT "); print_hex(reinterpret_cast<const uint8_t *>(text.data()), text.size()); printf("\n");
    printf("RASTER "); print_hex(label.bits.get(), label.bytes()); printf("\n");
    voice_screen::label_preview(label.bits.get(), label.rows);
    if (!print) { printf("LABEL_READY\n"); fflush(stdout); return; }
    const auto result = send_label(label, true, false);
    if (!result.success) {
        show("印刷に失敗", result.sent ? "印刷されたか分からない。もう一度押すと2枚出るかも" : "もう一度タップしてください");
    }
}

enum class Recorded { confirmed, cancelled, empty, failed };

bool prepare_styles()
{
    static const char *names[] = {"しろ・ひらがな","しろ・カタカナ","わく・ひらがな","わく・カタカナ","はんてん・ひらがな","はんてん・カタカナ"};
    std::string error;
    for (int style = 0; style < label_raster::kStyles; ++style) {
        if (!label_raster::render_style(g_last_text, style, g_styles[style], error)) {
            printf("ERROR style_render style=%d reason=%s\n", style, error.c_str()); return false;
        }
    }
    g_style = 0; g_style_names = names; g_style_count = label_raster::kStyles;
    voice_screen::volume_warning(g_quiet_warning);
    voice_screen::previews(g_last_text, g_styles, g_style_names, g_style_count, g_style, true, "どれにする？", "つぎ／長おしで印刷");
    return true;
}

void show_styles(bool reset_scroll)
{
    const bool preset = g_style_count != label_raster::kStyles;
    voice_screen::previews(preset ? kPresets[g_person].kanji : g_last_text, g_styles, g_style_names, g_style_count, g_style,
                           reset_scroll, "どれにする？", "つぎ／長おしで印刷");
}

// Kanji, hiragana and katakana in each frame; plain kanji first.
bool prepare_preset(int person)
{
    static const char *names[] = {"しろ・かんじ","しろ・ひらがな","しろ・カタカナ","わく・かんじ","わく・ひらがな","わく・カタカナ",
                                  "はんてん・かんじ","はんてん・ひらがな","はんてん・カタカナ"};
    std::string error;
    for (int frame = 0; frame < 3; ++frame) {
        if (!label_raster::render_variant(kPresets[person].kanji, frame, g_styles[frame * 3], error)
            || !label_raster::render_style(kPresets[person].kana, frame * 2, g_styles[frame * 3 + 1], error)
            || !label_raster::render_style(kPresets[person].kana, frame * 2 + 1, g_styles[frame * 3 + 2], error)) {
            printf("ERROR preset_render person=%d frame=%d reason=%s\n", person, frame, error.c_str()); return false;
        }
    }
    g_person = person; g_style = 0; g_style_names = names; g_style_count = kMaxChoices;
    g_last_text = kPresets[person].kana;
    voice_screen::volume_warning(false);
    show_styles(true);
    printf("UI_STATE preset_styles person=%d\n", person); fflush(stdout);
    return true;
}

// The people list shows each preset as a plain kanji label.
bool show_people(int person)
{
    std::string error;
    for (int i = 0; i < kPresetCount; ++i) {
        g_people_names[i] = kPresets[i].name;
        if (!g_people[i].bits && !label_raster::render_variant(kPresets[i].kanji, 0, g_people[i], error)) {
            printf("ERROR people_render person=%d reason=%s\n", i, error.c_str()); return false;
        }
    }
    g_person = person;
    voice_screen::volume_warning(false);
    voice_screen::previews("", g_people, g_people_names, kPresetCount, person, person == 0, "だれにする？", "つぎ／長おしでえらぶ");
    printf("UI_STATE people person=%d\n", person); fflush(stdout);
    return true;
}

Recorded record_product(Recognizer &recognizer, std::vector<float> &pcm, std::vector<float> &features, int16_t *raw)
{
    constexpr int maximum = kMaxSamples;
    voice_screen::roles("離して認識", "");
    voice_screen::recording_progress(double(maximum) / ralomi::kSampleRate, -120);
    g_confirm_label.bits.reset(); g_last_text.clear();
    g_product.raw = raw; g_product.completed = 0; g_product.completed_us = 0;
    g_product.accepting = true; g_product.finished = false; g_product.failed = false;
    g_product_active = true;
    if (!M5.Mic.record(raw, ralomi::kHop, ralomi::kSampleRate)) { g_product_active = false; return Recorded::failed; }
    printf("UI_STATE recording\n");
    recognizer.reset();
    audio_gain::Stream gain;
    int16_t *normalized = pcm16_area(pcm);
    int consumed = 0, widened = 0, computed = 0, fed = 0, progress = 0;
    int64_t feature_us = 0;
    while (true) {
        M5.update();
        if (!M5.BtnA.isPressed()) { g_product.accepting = false; }
        const bool final = g_product.finished.load();
        const int end = g_product.completed.load();
        if (g_product.failed.load() && final) { g_product_active = false; return Recorded::failed; }
        if (end > consumed || final) {
            const int ready = gain.append(raw, end, normalized, final ? end : maximum, recognizer.chunk_frames() * ralomi::kHop);
            consumed = end;
            widen_pcm16(pcm, ready, widened); widened = ready;
            const int frames = ralomi::frame_count(ready);
            if (frames > computed) {
                const int64_t start = esp_timer_get_time();
                ralomi::log_mel(pcm.data() + computed * ralomi::kHop, ready - computed * ralomi::kHop,
                               features.data() + computed * kBins);
                feature_us += esp_timer_get_time() - start; computed = frames;
            }
            while (computed - fed >= recognizer.chunk_frames() || (final && computed > fed)) {
                const int count = std::min(recognizer.chunk_frames(), computed - fed);
                recognizer.feed(features.data() + fed * kBins, count); fed += count;
            }
            if (final) { break; }
            if (end / (ralomi::kSampleRate / 2) > progress) {
                progress = end / (ralomi::kSampleRate / 2);
                double energy = 0;
                const int begin = std::max(0, end - ralomi::kSampleRate / 2);
                for (int i = begin; i < end; ++i) { energy += double(raw[i]) * raw[i]; }
                const double dbfs = 10 * std::log10(energy / (end - begin) / (32768.0 * 32768.0) + 1e-12);
                voice_screen::recording_progress(double(maximum - end) / ralomi::kSampleRate, dbfs);
            }
        }
        vTaskDelay(1);
    }
    g_product_active = false;
    // The left button records from every screen; a quick tap only leaves the screen.
    if (consumed < ralomi::kSampleRate * 3 / 10) {
        printf("UI_RECORD cancelled=1 samples=%d\n", consumed); return Recorded::cancelled;
    }
    Recognition result = finish(recognizer, computed); result.feature_us = feature_us;
    g_last_text = result.text;
    g_quiet_warning = gain.quiet_speech(recognizer.argmax().data(), recognizer.argmax().size(), ralomi::kHop, kSubsampling);
    printf("UI_VOLUME normalized_rms_dbfs=%.8f threshold_dbfs=%.8f warning=%d\n", gain.rms_dbfs(), audio_gain::kQuietNormalizedDbfs, g_quiet_warning);
    if (g_last_text.empty()) { print_result(result); return Recorded::empty; }
    std::string error;
    if (!prepare_styles()) { return Recorded::failed; }
    int clipped = 0;
    for (int i = 0; i < consumed; ++i) { clipped += raw[i] <= audio_gain::kLow || raw[i] >= audio_gain::kHigh; }
    printf("UI_RECORD cancelled=0 samples=%d leading_zero=%d clipped=%d tail_us=%lld\n", consumed, gain.leading(), clipped,
           (long long)(esp_timer_get_time() - g_product.completed_us.load()));
    print_result(result);
    printf("UI_STATE confirmation\n"); fflush(stdout);
    return Recorded::confirmed;
}

// --- Action mode (KoeAct-JP) ------------------------------------------------------------------

bool a2_init(int chunk_steps)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "a2");
    if (part == nullptr) { report("A2 no partition"); return false; }
    const void *map = nullptr;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) {
        report("A2 mmap failed");
        return false;
    }
    uint32_t payload = 0;
    std::memcpy(&payload, static_cast<const uint8_t *>(map) + 4, 4);
    const size_t bytes = 8 + (size_t)payload;
    const int rc = bytes <= part->size ? a2_load(&g_a2, map, bytes) : -9;
    const size_t chunk_bytes = (size_t)chunk_steps * A2_LAYERS_IN * A2_DIN * sizeof(float);
    g_a2_arena = heap_caps_malloc(a2_arena_bytes(kA2MaxSteps), MALLOC_CAP_SPIRAM);
    g_a2_chunk = static_cast<float *>(heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM));
    report("A2 load=%d bytes=%u arena_bytes=%u chunk_bytes=%u", rc, (unsigned)bytes,
           (unsigned)a2_arena_bytes(kA2MaxSteps), (unsigned)chunk_bytes);
    g_a2_ready = rc == 0 && g_a2_arena != nullptr && g_a2_chunk != nullptr;
    log_memory("a2_loaded");
    return g_a2_ready;
}

struct ActionResult {
    bool decoded = false, accepted = false;
    float p_act = 0;
    std::string kana, json;
};

// KoeAct-JP on one utterance of raw int16 PCM, with the release's preprocessing (koeact.runtime
// clean): leading exact zeros (codec start-up) dropped, the whole utterance set to -23 dBFS RMS
// (peak at most 0.99), then 0.5 s of silence before and 0.3 s after. `raw` must not overlap `pcm`.
ActionResult run_koeact(Recognizer &recognizer, const int16_t *raw, int samples, std::vector<float> &pcm,
                        std::vector<float> &features)
{
    ActionResult out;
    const int64_t start = esp_timer_get_time();
    int leading = 0;
    while (leading < samples && raw[leading] == 0) { ++leading; }
    raw += leading; samples -= leading;
    samples = std::min(samples, kA2MaxSteps * kSubsampling * ralomi::kHop - kLeadSamples - kTailSamples);
    double energy = 0;
    int peak = 0;
    for (int i = 0; i < samples; ++i) { energy += double(raw[i]) * raw[i]; peak = std::max(peak, std::abs(int(raw[i]))); }
    const double rms = samples ? std::sqrt(energy / samples) / 32768.0 : 0;
    double gain = rms > 0 ? std::pow(10.0, (-23.0 - 20 * std::log10(rms)) / 20) / 32768.0 : 1 / 32768.0;
    if (peak * gain > 0.99) { gain = 0.99 / peak; }
    const int total = kLeadSamples + samples + kTailSamples;
    std::fill(pcm.begin(), pcm.begin() + kLeadSamples, 0.0f);
    for (int i = 0; i < samples; ++i) { pcm[kLeadSamples + i] = float(raw[i] * gain); }
    std::fill(pcm.begin() + kLeadSamples + samples, pcm.begin() + total, 0.0f);
    const int frames = std::min(ralomi::frame_count(total), kA2MaxSteps * kSubsampling);
    ralomi::log_mel(pcm.data(), total, features.data());
    const int64_t feature_us = esp_timer_get_time() - start;
    recognizer.run_all(features.data(), frames);
    Recognition kana = finish(recognizer, frames);
    out.kana = kana.text;
    int64_t generate_us = -1;
    a2_result result{};
    if (g_a2_ready && recognizer.layer_steps() > 0) {
        const int64_t begin = esp_timer_get_time();
        a2_generate(&g_a2, recognizer.layer_steps(), kA2MaxSteps, g_a2_arena, &result);
        generate_us = esp_timer_get_time() - begin;
        out.decoded = true;
        out.p_act = result.p_act;
        out.accepted = result.p_act >= action::kThreshold;
        out.json = action::json_text(result.ids, result.n);
    }
    printf("KOEACT samples=%d leading_zero=%d rms_dbfs=%.1f frames=%d feature_us=%" PRId64 " run_us=%" PRId64
           " encode_us=%" PRId64 " generate_us=%" PRId64 " total_us=%" PRId64 " p_act=%.6f min_prob=%.6f accepted=%d ids=",
           samples, leading, 20 * std::log10(rms + 1e-12), frames, feature_us, kana.run_us, recognizer.encode_us(),
           generate_us, esp_timer_get_time() - start, result.p_act, result.min_prob, out.accepted);
    for (int i = 0; i < result.n; ++i) { printf(i ? " %d" : "%d", result.ids[i]); }
    printf("\nKOEACT_JSON %s\nKOEACT_KANA %s\n", out.json.c_str(), out.kana.c_str());
    fflush(stdout);
    return out;
}

void show_action(const ActionResult &result)
{
    if (!result.decoded) {
        action::idle("デコーダが使えません");
        printf("ERROR koeact_decoder ready=%d\n", g_a2_ready);
        return;
    }
    const int played = action::perform(result.json, result.kana, result.accepted);
    printf("ACTION_DONE played=%d accepted=%d json=%s\n", played, result.accepted, result.json.c_str());
    fflush(stdout);
}

// --- Always listening (action mode) -------------------------------------------------------------
// Utterances are cut out of the microphone ring by energy, against a noise floor that follows the
// room, and run through run_koeact() exactly like a button recording. Every buffer is allocated
// once at start; the loop writes nothing to flash.
constexpr int kHopsPerSecond = ralomi::kSampleRate / ralomi::kHop;
constexpr int kPrerollHops = kHopsPerSecond / 4;       // speech onset kept before the trigger
constexpr int kOnsetHops = 3;                          // 30 ms over the onset level starts speech
constexpr int kEndSilenceHops = kHopsPerSecond * 6 / 10;  // 0.6 s under the hold level ends it
constexpr int kTailHops = kHopsPerSecond / 5;          // silence kept after the last voiced hop
constexpr int kMinVoicedHops = kHopsPerSecond * 15 / 100;  // shorter bursts (clicks) are dropped
constexpr double kOnsetOverNoiseDb = 12, kHoldOverNoiseDb = 7, kOnsetFloorDb = -52, kHoldFloorDb = -56;
// Speech also ends 25 dB under its own (slowly falling) peak, so a noisy room (fan, air
// conditioner) still ends an utterance even when the noise floor learnt before it is stale.
constexpr double kHoldUnderPeakDb = 25, kPeakDecayDbPerHop = 0.02;
// run_koeact() keeps at most this much of an utterance (the decoder arena, minus lead and tail).
constexpr int kSegmentSamples = kA2MaxSteps * kSubsampling * ralomi::kHop - kLeadSamples - kTailSamples;
constexpr int kListenSamples = std::min(kSegmentSamples, kActionSeconds * ralomi::kSampleRate);

struct ListenStats {
    int segments = 0, accepted = 0, rejected = 0, dropped = 0, button = 0;
    int64_t max_us = 0;
};

// Mean-removed energy of one hop in dBFS (the codec adds a small DC offset).
double hop_dbfs(const int16_t *hop)
{
    double sum = 0, square = 0;
    for (int i = 0; i < ralomi::kHop; ++i) { sum += hop[i]; square += double(hop[i]) * hop[i]; }
    const double mean = sum / ralomi::kHop;
    const double variance = std::max(0.0, square / ralomi::kHop - mean * mean);
    return 10 * std::log10(variance / (32768.0 * 32768.0) + 1e-12);
}

bool start_listening()
{
    g_listen.mic_on = true;
    if (!M5.Mic.record(g_listen.ring + g_listen.written.load() % kMaxSamples, ralomi::kHop, ralomi::kSampleRate)) {
        g_listen.mic_on = false;
        return false;
    }
    return true;
}

void pause_listening()
{
    g_listen.mic_on = false;
    while (M5.Mic.isRecording()) { vTaskDelay(1); }  // the request in flight still lands in the ring
}

// All response producers run in the recognition task. Stop the shared I2S input,
// then discard the codec's existing one-second startup period before hearing again.
bool restore_microphone(bool inject_failure)
{
    M5.Mic.end();
    const auto saved=M5.Mic.config();
    if(inject_failure) {
        auto invalid=saved;invalid.sample_rate=0;
        M5.Mic.config(invalid); // exercise the real begin() rejection, not a substituted return
    }
    bool restored=M5.Mic.begin();
    if(inject_failure) M5.Mic.config(saved);
    printf("MIC_RESTORE begin=%d injected=%d\n",restored,inject_failure);
    if(!restored || !g_microphone_warmup) return false;
    restored=M5.Mic.record(g_microphone_warmup,ralomi::kSampleRate,ralomi::kSampleRate);
    printf("MIC_RESTORE warmup=%d samples=%d\n",restored,ralomi::kSampleRate);
    while(M5.Mic.isRecording()) vTaskDelay(1);
    return restored;
}

void recover_listening()
{
    if(!g_listen.failed.exchange(false)) return;
    printf("ERROR MIC listen_chain restarting\n");
    pause_listening();
    const bool active=g_listen.active.exchange(false);
    bool restored=restore_microphone(false);
    g_listen.active=active;
    if(restored && active) restored=start_listening();
    if(!restored) g_listen.failed=true;
    printf("MIC_RECOVERY ready=%d mic_on=%d written=%lld\n",restored,g_listen.mic_on.load(),(long long)g_listen.written.load());
    fflush(stdout);
}

void speak_response(const char *caption)
{
    if(!tts::is_ready()) {
        printf("ERROR TTS not_ready caption=%s microphone_unchanged=1\n",caption);
        fflush(stdout);return;
    }
    const bool active = g_listen.active.load();
    const bool resume = g_listen.mic_on.load();
    const bool microphone = M5.Mic.isRunning();
    pause_listening();
    g_listen.active = false;
    M5.Mic.end();
    const int64_t before = g_listen.written.load();
    printf("TTS_LISTEN state=paused written=%lld resume=%d\n", (long long)before, resume);
    tts::say(caption);
    bool restored = true;
    if (microphone) {
        restored=restore_microphone(g_restore_fault.exchange(false));
    }
    const int64_t after = g_listen.written.load();
    g_listen.active = active;
    if (resume && restored) { restored = start_listening(); }
    printf("TTS_LISTEN state=restored ok=%d before=%lld after=%lld mic_on=%d\n", restored,
           (long long)before, (long long)after, g_listen.mic_on.load());
    if (!restored) { g_listen.failed=true;printf("ERROR TTS microphone_restore\n"); }
    fflush(stdout);
}

// Diagnostic "LPCM": after the header, uint32 samples | int16 PCM fed to the listening ring in
// place of the microphone, hop by hop, calling `pump` after each so the cutting and everything after
// it run as live. The host sends the next 0.5 s only after "LPCM_ACK": the USB receive buffers hold
// about 1 s and an utterance with its animation or label can take longer than that.
void feed_lpcm(const std::function<void()> &pump)
{
    uint8_t size[4];
    const uint32_t samples = read_exact(size, 4)
        ? uint32_t(size[0]) | uint32_t(size[1]) << 8 | uint32_t(size[2]) << 16 | uint32_t(size[3]) << 24 : 0;
    pause_listening();
    printf("LPCM_BEGIN samples=%u\n", (unsigned)samples);
    for (uint32_t fed = 0; fed + ralomi::kHop <= samples; fed += ralomi::kHop) {
        const int64_t at = g_listen.written.load();
        if (!read_exact(reinterpret_cast<uint8_t *>(g_listen.ring + at % kMaxSamples), ralomi::kHop * sizeof(int16_t))) {
            printf("ERROR LPCM short\n");
            break;
        }
        g_listen.written = at + ralomi::kHop;
        pump();
        if ((fed / ralomi::kHop + 1) % (kHopsPerSecond / 2) == 0) { printf("LPCM_ACK %u\n", (unsigned)(fed + ralomi::kHop)); fflush(stdout); }
    }
    uint8_t tail[2 * ralomi::kHop];  // a last partial hop is read and dropped
    if (samples % ralomi::kHop) { read_exact(tail, (samples % ralomi::kHop) * sizeof(int16_t)); }
    printf("LPCM_END\n");
    fflush(stdout);
    if (!start_listening()) { printf("ERROR MIC listen_restart\n"); }
}

// Cuts utterances out of the ring hop by hop, against a room noise floor followed quickly down
// and very slowly up (so steady noise is learnt but speech does not raise it).
struct Cutter {
    enum class Event { none, segment, dropped };
    bool speaking = false;
    int onset = 0, voiced = 0;
    double noise = -60, peak = -120;
    int64_t start = 0, last_voiced = 0, end = 0;

    void reset() { speaking = false; onset = 0; voiced = 0; }

    // One hop whose first sample is `hop`; `next` is the sample after it. On Event::segment the
    // utterance is [start, end) and `voiced` still counts its voiced hops until reset().
    Event step(double level, int64_t hop, int64_t next)
    {
        if (!speaking) {
            noise += (level < noise + 3 ? 0.05 : 0.002) * (level - noise);
            onset = level > std::max(noise + kOnsetOverNoiseDb, kOnsetFloorDb) ? onset + 1 : 0;
            if (onset >= kOnsetHops) {
                speaking = true;
                start = std::max<int64_t>(0, hop - (int64_t)(kOnsetHops + kPrerollHops) * ralomi::kHop);
                last_voiced = hop;
                voiced = onset;
                peak = level;
            }
            return Event::none;
        }
        peak = std::max(level, peak - kPeakDecayDbPerHop);
        if (level > std::max({noise + kHoldOverNoiseDb, kHoldFloorDb, peak - kHoldUnderPeakDb})) { last_voiced = hop; ++voiced; }
        const bool silent = hop - last_voiced >= (int64_t)kEndSilenceHops * ralomi::kHop;
        const bool full = next - start >= kListenSamples;
        if (!silent && !full) { return Event::none; }
        speaking = false; onset = 0;
        if (voiced < kMinVoicedHops) { voiced = 0; return Event::dropped; }
        end = full ? next : std::min(next, last_voiced + (int64_t)(kTailHops + 1) * ralomi::kHop);
        return Event::segment;
    }
};

void serve_action(Recognizer &recognizer)
{
    std::vector<float> features(kFrames * kBins);
    std::vector<float> pcm(kMaxSamples);
    auto psram = [](size_t bytes) {
        return std::unique_ptr<int16_t, decltype(&heap_caps_free)>(
            static_cast<int16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    };
    auto ring = psram(kMaxSamples * sizeof(int16_t));
    auto segment = psram(kSegmentSamples * sizeof(int16_t));
    if (!ring || !segment) { printf("ERROR MIC listen_buffer allocation\n"); vTaskDelete(nullptr); return; }
    log_memory("action_buffers");
    if (!prepare_microphone(pcm)) { printf("ERROR MIC prepare\n"); vTaskDelete(nullptr); return; }
    std::fill(ring.get(), ring.get() + kMaxSamples, 0);
    g_listen.ring = ring.get();
    g_listen.written = 0;
    g_listen.active = true;
    if (!voice_screen::start_characters()) { printf("ERROR characters_start\n"); vTaskDelete(nullptr); return; }
    if (!start_listening()) {
        printf("ERROR MIC listen_start\n");
        voice_screen::failure("録音に失敗しました");
        vTaskDelete(nullptr);
        return;
    }
    action::reset();
    printf("UI_STATE action_listening\n");
    ESP_LOGI(TAG, "READY action mode: always listening / hold A / RLMA pcm16 / LPCM / LSTA / MODE");

    enum class Phase { idle, button };
    Phase phase = Phase::idle;
    int64_t scan = 0, start = 0;  // start: the button recording's first sample
    Cutter cutter;
    bool listening_drawn = false;
    ListenStats stats;
    int64_t next_stat = esp_timer_get_time() + 60 * 1000000LL;

    // Copy [begin, end) out of the ring and run it; a VAD utterance that is not a request leaves
    // the screen as it was (the room's talk must not keep changing it).
    auto process = [&](int64_t begin, int64_t end, bool by_button) {
        const int64_t written = g_listen.written.load();
        begin = std::max(begin, written - kMaxSamples + ralomi::kHop * kHopsPerSecond);
        const int samples = (int)std::min<int64_t>(end - begin, kListenSamples);
        const int64_t started = esp_timer_get_time();
        for (int i = 0; i < samples; ++i) { segment.get()[i] = ring.get()[(begin + i) % kMaxSamples]; }
        ++stats.segments;
        stats.button += by_button;
        printf("LISTEN_SEG trigger=%s samples=%d voiced_hops=%d noise_dbfs=%.1f\n", by_button ? "button" : "vad",
               samples, cutter.voiced, cutter.noise);
        if (!by_button) { action::idle("かんがえ中…"); }
        const ActionResult result = run_koeact(recognizer, segment.get(), samples, pcm, features);
        const home::Command appliance = home::parse(result.kana);
        if (wants_voice_print(result.kana)) {
            pause_listening();
            speak_response("ラベルをいんさつするよ");
            reboot_into(Mode::kana, true);
        } else if (appliance.kind != home::Command::Kind::none) {
            ++stats.accepted;
            home::perform(appliance, result.kana);
        } else if (action::perform_extra(result.kana)) {
            ++stats.accepted;
        } else if (action::perform_talk(result.kana)) {
            ++stats.accepted;
        } else if (result.decoded && !result.accepted && !by_button) {
            ++stats.rejected;
            action::idle("話しかけてね");
            printf("LISTEN_IGNORED p_act=%.6f kana=%s\n", result.p_act, result.kana.c_str());
        } else {
            stats.accepted += result.accepted;
            stats.rejected += !result.accepted;
            show_action(result);
        }
        stats.max_us = std::max(stats.max_us, esp_timer_get_time() - started);
        // Skip what arrived while busy: an old command must not fire late.
        scan = g_listen.written.load();
        phase = Phase::idle; cutter.reset(); listening_drawn = false;
    };

    // Run the cutter over every complete hop up to `limit`.
    auto pump = [&](int64_t limit) {
        while (scan + ralomi::kHop <= limit) {
            const double level = hop_dbfs(ring.get() + scan % kMaxSamples);
            const int64_t hop = scan;
            scan += ralomi::kHop;
            if (phase == Phase::button) { continue; }
            const Cutter::Event event = cutter.step(level, hop, scan);
            if (cutter.speaking && !listening_drawn && cutter.voiced >= kMinVoicedHops) {
                action::idle("きいてるよ…");
                listening_drawn = true;
            }
            if (event == Cutter::Event::dropped) { ++stats.dropped; continue; }
            if (event == Cutter::Event::segment) { process(cutter.start, cutter.end, false); return; }
        }
    };

    bool release_barrier = false, held = false;
    int touch_x=0,touch_y=0;
    int progress = 0;
    uint8_t header[4] = {};
    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) {
            printf("BUTTON A=%d B=%d\n", M5.BtnA.wasPressed(), M5.BtnB.wasPressed());
        }
        bool touch_down=false;
        if(M5.Touch.getCount()>0) {
            const auto &point=M5.Touch.getDetail(0);
            touch_x=point.x;touch_y=point.y;touch_down=point.isPressed();
        }
        if(phase!=Phase::button && !release_barrier &&
           voice_screen::character_touch(touch_down,touch_x,touch_y,uint32_t(esp_timer_get_time()/1000))) { switch_mode(); }
        recover_listening();
        if (phase == Phase::button) {
            // Hold the left button and speak: the same ring, cut by the button instead of energy.
            const int64_t now = g_listen.written.load();
            if (!M5.BtnA.isPressed() || now - start >= kListenSamples) {
                if (now - start < ralomi::kSampleRate * 3 / 10 + kPrerollHops * ralomi::kHop) {
                    printf("UI_RECORD cancelled=1\n");
                    action::idle("話しかけてね");
                    scan = now; phase = Phase::idle; cutter.reset();
                } else {
                    action::idle("かんがえ中…");
                    process(start, now, true);
                }
                release_barrier = true;
            } else if ((now - start) / (ralomi::kSampleRate / 2) > progress) {
                progress = (now - start) / (ralomi::kSampleRate / 2);
                voice_screen::recording_progress(std::max(0.0, double(start + kListenSamples - now) / ralomi::kSampleRate),
                                                 -120, kActionSeconds);
            }
        }
        if (release_barrier && !M5.BtnA.isPressed() && !M5.BtnB.isPressed() && M5.Touch.getCount() == 0) {
            release_barrier = false;
        }
        if (!release_barrier && phase != Phase::button) {
            if (M5.BtnA.wasPressed()) {
                phase = Phase::button;
                start = std::max<int64_t>(0, g_listen.written.load() - (int64_t)kPrerollHops * ralomi::kHop);
                progress = 0;
                voice_screen::roles("離して認識", "");
                voice_screen::recording_progress(kActionSeconds, -120, kActionSeconds);
                printf("UI_STATE action_recording\n");
                continue;
            }
            if (M5.BtnB.wasPressed()) { action::reset(); printf("UI_STATE action_reset\n"); release_barrier = true; continue; }
        }
        if (held) { scan = g_listen.written.load(); }
        else { pump(g_listen.written.load()); }
        if (esp_timer_get_time() >= next_stat) {
            next_stat += 60 * 1000000LL;
            printf("LISTEN_STAT segments=%d accepted=%d rejected=%d dropped=%d button=%d max_us=%" PRId64
                   " noise_dbfs=%.1f written=%" PRId64 "\n", stats.segments, stats.accepted, stats.rejected,
                   stats.dropped, stats.button, stats.max_us, cutter.noise, g_listen.written.load());
            voice_screen::animation_stats();
            log_memory("listening", false);
        }
        if (xStreamBufferReceive(serial_commands, header, 1, pdMS_TO_TICKS(10)) != 1) { continue; }
        if(header[0]=='T' && read_exact(header+1,3) && std::memcmp(header,"TTBL",4)==0) {
            tts::diagnostic_phrases(speak_response);
            continue;
        }
        if(header[0]=='C' && read_exact(header+1,3) && std::memcmp(header,"CTCH",4)==0) {
            uint8_t packet[11];
            if(!read_exact(packet,sizeof(packet))) { printf("ERROR CHARACTER_DIAG short\n");continue; }
            auto number=[&](int offset) { return int16_t(packet[offset] | (packet[offset+1]<<8)); };
            if(voice_screen::diagnostic_touch(packet[0],number(1),number(3),number(5),number(7),uint16_t(number(9)))) switch_mode();
            continue;
        }
        if (header[0] == 'L' && read_exact(header + 1, 3)) {
            if (std::memcmp(header, "LHLD", 4) == 0 || std::memcmp(header, "LRUN", 4) == 0) {
                // Diagnostic: hold the energy cutting so room speech cannot run while a host test
                // streams PCM (the USB receive buffers overflow while an utterance is processed).
                held = header[1] == 'H';
                scan = g_listen.written.load(); phase = Phase::idle; cutter.reset();
                printf("LISTEN_HOLD %d\n", held);
            } else if (std::memcmp(header, "LSTA", 4) == 0) {
                next_stat = esp_timer_get_time();  // print the statistics on the next pass
            } else if (std::memcmp(header, "LPCM", 4) == 0) {
                scan = g_listen.written.load(); phase = Phase::idle; cutter.reset();
                feed_lpcm([&] { pump(g_listen.written.load()); });
                scan = g_listen.written.load(); phase = Phase::idle; cutter.reset();
            }
            continue;
        }
        if (header[0] == 'M' && read_exact(header + 1, 2)) {
            if (std::memcmp(header, "MOD", 3) == 0 && read_exact(header + 3, 1) && header[3] == 'E') { switch_mode(); }
            continue;
        }
        if (header[0] == 'I' && read_exact(header + 1, 3) && std::memcmp(header, "INFO", 4) == 0) {
            printf("BOOT_REPORT_BEGIN\n%sBOOT_REPORT_END\n", g_boot_report.c_str());
            log_memory("serving", false);
            continue;
        }
        if (header[0] == 'S' && read_exact(header + 1, 3) && std::memcmp(header, "SHOT", 4) == 0) {
            voice_screen::dump_pixels(print_hex);
            continue;
        }
        if (header[0] == 'R' && read_exact(header + 1, 3) && std::memcmp(header, "RLMA", 4) == 0) {
            // uint32 samples | uint8 (ignored) | samples * int16 raw PCM (16 kHz mono): one
            // utterance, as a button recording would give (at most kSegmentSamples are kept).
            uint8_t size[5];
            const bool sized = read_exact(size, 5);
            const int samples = sized ? size[0] | (size[1] << 8) | (size[2] << 16) | (size[3] << 24) : 0;
            const int kept = std::min(samples, kSegmentSamples);
            if (samples < ralomi::kWindow || samples > kMaxSamples ||
                !read_exact(reinterpret_cast<uint8_t *>(segment.get()), kept * sizeof(int16_t))) {
                printf("ERROR samples %d\n", samples);
                continue;
            }
            for (int rest = samples - kept; rest > 0;) {  // drop what does not fit, keeping the framing
                uint8_t skip[256];
                const int count = std::min<int>(rest * 2, sizeof(skip));
                if (!read_exact(skip, count)) { break; }
                rest -= count / 2;
            }
            action::idle("かんがえ中…");
            const ActionResult result = run_koeact(recognizer, segment.get(), kept, pcm, features);
            const home::Command appliance = home::parse(result.kana);
            if (wants_voice_print(result.kana)) { pause_listening(); speak_response("ラベルをいんさつするよ"); reboot_into(Mode::kana, true); }
            else if (appliance.kind != home::Command::Kind::none) { home::perform(appliance, result.kana); }
            else if (!action::perform_extra(result.kana) && !action::perform_talk(result.kana)) { show_action(result); }
            scan = g_listen.written.load(); phase = Phase::idle; cutter.reset();
        }
    }
}

// --- Voice printing (kana mode, Ralomi) ---------------------------------------------------------

// Ralomi on [begin, end) of the listening ring, prepared like the PCM it matched FP32 on: the
// utterance at -23 dBFS RMS (peak at most 0.99) with 0.5 s of silence before and 0.3 s after. The
// int16 copy goes to the upper half of `pcm` and is widened in place (pcm16_area).
std::string recognize_ring(Recognizer &recognizer, int64_t begin, int64_t end, std::vector<float> &pcm,
                           std::vector<float> &features)
{
    begin = std::max(begin, g_listen.written.load() - kMaxSamples + ralomi::kHop * kHopsPerSecond);
    const int samples = (int)std::min<int64_t>(end - begin, kListenSamples);
    double energy = 0;
    int peak = 0;
    for (int i = 0; i < samples; ++i) {
        const int value = g_listen.ring[(begin + i) % kMaxSamples];
        energy += double(value) * value;
        peak = std::max(peak, std::abs(value));
    }
    const double rms = samples ? std::sqrt(energy / samples) / 32768.0 : 0;
    double gain = rms > 0 ? std::pow(10.0, (-23.0 - 20 * std::log10(rms)) / 20) : 1;
    if (peak * gain > 0.99 * 32768) { gain = 0.99 * 32768 / peak; }
    int16_t *out = pcm16_area(pcm);
    const int total = kLeadSamples + samples + kTailSamples;
    std::fill(out, out + kLeadSamples, 0);
    for (int i = 0; i < samples; ++i) {
        out[kLeadSamples + i] = (int16_t)std::lround(g_listen.ring[(begin + i) % kMaxSamples] * gain);
    }
    std::fill(out + kLeadSamples + samples, out + total, 0);
    widen_pcm16(pcm, total);
    Recognition result = recognize_pcm(recognizer, pcm.data(), total, features);
    printf("VOICE_HEARD samples=%d rms_dbfs=%.1f run_us=%" PRId64 " text=%s\n", samples, 20 * std::log10(rms + 1e-12),
           result.run_us, result.text.c_str());
    fflush(stdout);
    return result.text;
}

// Characters in UTF-8 text (the kana are one character each, "ゅ" included).
int characters(const std::string &text)
{
    int count = 0;
    for (const unsigned char byte : text) { count += (byte & 0xC0) != 0x80; }
    return count;
}

// The go-ahead to print must be a short reply of its own ("いんさつ", "いんさつして"): with the
// room always heard, chatter that merely contains 印刷 printed a label on the board.
constexpr int kGoAheadCharacters = 6;

// ... and it must begin with the word: room speech heard as "にゅんさつ" printed a label too.
bool go_ahead(const std::string &text)
{
    if (characters(text) > kGoAheadCharacters) { return false; }
    for (const char *word : {"いんさつ", "ぴんさつ", "びんさつ", "いんさ", "おねがい"}) {
        if (text.rfind(word, 0) == 0) { return true; }
    }
    return false;
}

bool heard_any(const std::string &text, std::initializer_list<const char *> words)
{
    for (const char *word : words) { if (text.find(word) != std::string::npos) { return true; } }
    return false;
}

// Speak the words, see the label, say "いんさつ": the label prints and the board goes back to the
// action mode. "やりなおし" listens again, "やめる" (or the right button, or a minute of
// silence) goes back without printing.
void serve_voice_print(Recognizer &recognizer)
{
    std::vector<float> features(kFrames * kBins);
    std::vector<float> pcm(kMaxSamples);
    std::unique_ptr<int16_t, decltype(&heap_caps_free)> ring(
        static_cast<int16_t *>(heap_caps_malloc(kMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!ring) { printf("ERROR MIC listen_buffer allocation\n"); reboot_into(Mode::action, false); }
    if (!prepare_microphone(pcm)) { printf("ERROR MIC prepare\n"); reboot_into(Mode::action, false); }
    std::fill(ring.get(), ring.get() + kMaxSamples, 0);
    g_listen.ring = ring.get();
    g_listen.written = 0;
    g_listen.active = true;
    if (!start_listening()) { printf("ERROR MIC listen_start\n"); reboot_into(Mode::action, false); }
    log_memory("voice_print", false);

    enum class Step { words, confirm };
    Step step = Step::words;
    constexpr int64_t kIdleUs = 60 * 1000000LL;
    int64_t give_up_at = esp_timer_get_time() + kIdleUs;
    int64_t scan = 0;
    Cutter cutter;
    label_raster::Label &label = g_styles[0];
    voice_screen::voice_prompt("いんさつしたい", "ことばを話してね", "");
    speak_response("いんさつしたいことばを話してね");
    printf("UI_STATE voice_print_words\n");
    fflush(stdout);

    auto back = [&](const char *why) {
        printf("VOICE_PRINT_END reason=%s\n", why);
        pause_listening();
        reboot_into(Mode::action, false);
    };
    auto print = [&] {
        voice_screen::printing(false);
        printf("UI_STATE printing_connect\n");
        speak_response("ラベルをいんさつするよ");
        const auto result = send_label(label, false, true);
        printf("VOICE_PRINT result=%d reason=%s sent=%u battery=%d\n", result.success, result.reason,
               (unsigned)result.sent, result.battery_percent);
        fflush(stdout);
        if (result.success) {
            voice_screen::success(result.battery_percent);
            speak_response("できたよ");
            vTaskDelay(pdMS_TO_TICKS(3000));  // as the button flow: 3 seconds of "できたよ"
            back("printed");
        }
        // Voice prompts, not the button flow's failure screen: saying "いんさつ" again retries.
        if (result.sent) { voice_screen::voice_prompt("いんさつされたか", "わからないよ", "いんさつ"); }
        else { voice_screen::voice_prompt("プリンターに", "つながらないよ", "いんさつ"); }
        speak_response(result.sent ? "いんさつされたかわからないよ" : "プリンターにつながらないよ");
        step = Step::confirm;
    };
    auto handle = [&](const std::string &text) {
        give_up_at = esp_timer_get_time() + kIdleUs;
        if (step == Step::words) {
            if (text.empty()) { voice_screen::voice_prompt("ききとれなかったよ", "もういちど話してね", ""); speak_response("ききとれなかったよ。もういちど話してね"); return; }
            // The go-ahead said too early is not the label: right after the switch the board took
            // "ーさつ" as the words. ("さつき" and other words with さつ still print.)
            if (go_ahead(text) || (characters(text) <= kGoAheadCharacters && heard_any(text, {"んさつ", "ーさつ"}))) {
                voice_screen::voice_prompt("いんさつしたい", "ことばを話してね", text);
                speak_response("いんさつしたいことばを話してね");
                printf("UI_STATE voice_print_words go_ahead_too_early=%s\n", text.c_str());
                return;
            }
            std::string error;
            if (!label_raster::render_style(text, 0, label, error)) {
                printf("ERROR label_render %s\n", error.c_str());
                voice_screen::voice_prompt("いんさつできない字が", "あるよ。もういちど", text);
                speak_response("いんさつできない字があるよ。もういちど");
                return;
            }
            g_last_text = text;
            step = Step::confirm;
            voice_screen::voice_confirm(label.bits.get(), label.rows, text);
            speak_response("いんさつ、といってね");
            printf("UI_STATE voice_print_confirm text=%s rows=%d\n", text.c_str(), label.rows);
            return;
        }
        if (heard_any(text, {"やめ", "きゃんせる", "もどって"})) { back("cancelled"); }
        // "やりなおし" came out as "やりな", "やりが" and "なりなおし" on the board: a short reply
        // starting "やり" or holding "なおし" (a redo uses no paper, so it may be loose).
        if ((characters(text) <= kGoAheadCharacters && (text.rfind("やり", 0) == 0 || heard_any(text, {"なおし"}))) ||
            heard_any(text, {"もういちど", "もいちど"})) {
            step = Step::words;
            voice_screen::voice_prompt("いんさつしたい", "ことばを話してね", "");
            speak_response("いんさつしたいことばを話してね");
    printf("UI_STATE voice_print_words\n");
        } else if (go_ahead(text)) {
            print();
        } else {
            voice_screen::voice_confirm(label.bits.get(), label.rows, g_last_text);  // keep waiting for a go-ahead
        }
        fflush(stdout);
    };
    auto pump = [&] {
        const int64_t limit = g_listen.written.load();
        while (scan + ralomi::kHop <= limit) {
            const double level = hop_dbfs(ring.get() + scan % kMaxSamples);
            const int64_t hop = scan;
            scan += ralomi::kHop;
            if (cutter.step(level, hop, scan) != Cutter::Event::segment) { continue; }
            const std::string text = recognize_ring(recognizer, cutter.start, cutter.end, pcm, features);
            cutter.reset();
            handle(text);
            scan = g_listen.written.load();  // what was said meanwhile is not taken as an answer
            return;
        }
    };

    uint8_t header[4] = {};
    bool held = false;  // "LHLD": a host test feeds the words; the room is not listened to meanwhile
    while (true) {
        M5.update();
        if (M5.BtnB.wasPressed()) { back("button"); }
        recover_listening();
        if (esp_timer_get_time() > give_up_at) { back("timeout"); }
        if (held) { scan = g_listen.written.load(); }
        else { pump(); }
        if (xStreamBufferReceive(serial_commands, header, 1, pdMS_TO_TICKS(10)) != 1) { continue; }
        if (header[0] == 'L' && read_exact(header + 1, 3)) {
            if (std::memcmp(header, "LPCM", 4) == 0) {
                scan = g_listen.written.load(); cutter.reset();
                feed_lpcm(pump);
                scan = g_listen.written.load(); cutter.reset();
            } else if (std::memcmp(header, "LHLD", 4) == 0 || std::memcmp(header, "LRUN", 4) == 0) {
                held = header[1] == 'H';
                scan = g_listen.written.load(); cutter.reset();
                printf("LISTEN_HOLD %d\n", held);
            }
        } else if (header[0] == 'M' && read_exact(header + 1, 2) && std::memcmp(header, "MOD", 3) == 0 &&
                   read_exact(header + 3, 1) && header[3] == 'E') {
            back("serial");
        } else if (header[0] == 'I' && read_exact(header + 1, 3) && std::memcmp(header, "INFO", 4) == 0) {
            printf("BOOT_REPORT_BEGIN\n%sBOOT_REPORT_END\n", g_boot_report.c_str());
            log_memory("serving", false);
        }
    }
}

// Golden-vector check against the ESP-PPQ test values, memory profile and neural latency.
void diagnose(Recognizer &recognizer)
{
    dl::Model *model = recognizer.model();
    const esp_err_t test = model->test();
    report("TEST %s", test == ESP_OK ? "PASS" : "FAIL");
    model->profile_memory();
    benchmark(recognizer);
    log_memory("after_bench");
}

void serve(Recognizer &recognizer)
{
    std::vector<float> features(kFrames * kBins);
    std::vector<float> pcm(kMaxSamples);
    std::unique_ptr<int16_t, decltype(&heap_caps_free)> raw(
        static_cast<int16_t *>(heap_caps_malloc(kMaxSamples * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!raw) { printf("ERROR MIC raw_buffer allocation\n"); vTaskDelete(nullptr); return; }
    uint8_t header[4] = {};
    if (!prepare_microphone(pcm)) {
        printf("ERROR MIC prepare\n");
        vTaskDelete(nullptr);
        return;
    }
    enum class Ui { waiting, recording, people, confirmation, printing, success, print_error, record_error };
    Ui state = Ui::waiting;
    bool release_barrier = false;
    TickType_t return_at = 0;
    auto waiting = [&]() {
        state = Ui::waiting; voice_screen::waiting();
        printf("UI_STATE waiting\n");
    };
    auto record = [&]() {
        state = Ui::recording;
        const Recorded result = record_product(recognizer, pcm, features, raw.get());
        if (result == Recorded::confirmed) { state = Ui::confirmation; }
        else if (result == Recorded::failed) {
            state = Ui::record_error; voice_screen::failure("録音に失敗しました");
            printf("UI_STATE record_error\n");
        } else {
            waiting();
            if (result == Recorded::empty) { show("聞き取れませんでした", "タップして話す"); printf("UI_STATE empty\n"); }
        }
        release_barrier = true;
    };
    auto print = [&]() {
        state = Ui::printing; voice_screen::printing(false);
        printf("UI_STATE printing_connect\n");
        speak_response("ラベルをいんさつするよ");
        const auto result = send_label(g_styles[g_style], false, true);
        if (result.success) {
            state = Ui::success; voice_screen::success(result.battery_percent);
            return_at = xTaskGetTickCount() + pdMS_TO_TICKS(3000);  // Approved plan: 3 seconds.
            printf("UI_STATE success battery=%d\n", result.battery_percent);
        } else if (std::strcmp(result.reason, "cancelled_before_send") == 0 && result.sent == 0) {
            waiting(); printf("UI_PRINT cancelled_before_send=1\n");
        } else {
            state = Ui::print_error;
            std::string reason;
            if (std::strstr(result.reason, "unknown_")) {
                char code[96]; snprintf(code, sizeof(code), "不明な状態（コード %02X）", result.status); reason = code;
            } else if (std::strstr(result.reason, "connect") || std::strstr(result.reason, "not_found")) {
                reason = "プリンターにつながりません";
            } else if (std::strstr(result.reason, "battery")) { reason = "プリンター電池の応答が不明です"; }
            else { reason = "プリンターの応答がありません"; }
            if (result.sent) { reason += "\n印刷されたか分からない。もう一度押すと2枚出るかも"; }
            voice_screen::failure(reason);
            printf("UI_STATE print_error reason=%s uncertain=%d\n", result.reason, result.sent > 0);
        }
        release_barrier = true;
    };
    waiting();
    ESP_LOGI(TAG, "READY RLM1 features / RLMA pcm16 / MIC or touch");
    int scroll_origin = 0;
    bool scroll_gesture = false;
    bool blue_printed = false;
    LongTouch mode_touch;
    while (true) {
        recover_listening();
        M5.update();
        // A long touch on the waiting screen switches to the action mode (touch is unused there).
        if (mode_touch.held() && state == Ui::waiting) { switch_mode(); }
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) {
            printf("BUTTON A=%d B=%d\n", M5.BtnA.wasPressed(), M5.BtnB.wasPressed());
            if (g_battery_armed && M5.BtnA.wasPressed()) {
                if (usb_serial_jtag_is_connected()) { show("USBを外して", "Aボタンで試験"); }
                else { g_battery_armed = false; run_ble_cycles(recognizer, pcm, features, raw.get()); }
                continue;
            }
        }
        if (state == Ui::success && (int32_t)(xTaskGetTickCount() - return_at) >= 0) { waiting(); }
        if (release_barrier && !M5.BtnA.isPressed() && !M5.BtnB.isPressed() && M5.Touch.getCount() == 0) { release_barrier = false; }
        if (!release_barrier) {
            // Human request: the left button starts recording from every screen.
            if (M5.BtnA.wasPressed()) { record(); continue; }
            if (M5.BtnB.wasPressed()) {
                blue_printed = false;
                if (state == Ui::waiting) {
                    // blue_printed swallows this press's release so it doesn't also pick the next person.
                    if (show_people(0)) { state = Ui::people; }
                    blue_printed = true; continue;
                }
            }
            if ((state == Ui::confirmation || state == Ui::print_error)
                && !blue_printed && M5.BtnB.pressedFor(kPrintHoldMs)) {
                blue_printed = true; print(); continue;
            }
            if (state == Ui::people && !blue_printed && M5.BtnB.pressedFor(kPrintHoldMs)) {
                blue_printed = true;
                if (prepare_preset(g_person)) { state = Ui::confirmation; }
                release_barrier = true; continue;
            }
            if (M5.BtnB.wasReleased() && !blue_printed) {
                if (state == Ui::confirmation) {
                    g_style = (g_style + 1) % g_style_count;
                    show_styles(false);
                    printf("UI_STYLE selected=%d\n", g_style);
                } else if (state == Ui::people) {
                    show_people((g_person + 1) % kPresetCount);
                } else if (state == Ui::success) { waiting(); release_barrier = true; }
            }
        }
        if (!release_barrier && M5.Touch.getCount() > 0 && (state == Ui::confirmation || state == Ui::people)) {
            const auto &touch = M5.Touch.getDetail(0);
            if (touch.wasPressed()) { scroll_origin = voice_screen::first_row(); scroll_gesture = true; }
            if (scroll_gesture && (touch.isDragging() || touch.isFlicking())) {
                const int target = scroll_origin - touch.distanceY() / voice_screen::line_height();
                voice_screen::scroll(target - voice_screen::first_row());
            }
            // Touch only scrolls the list; it never starts recording or printing.
        }
        // Short timeout so the loop also watches the touch panel.
        if (xStreamBufferReceive(serial_commands, header, 1, pdMS_TO_TICKS(20)) != 1) {
            continue;
        }
        if (header[0] == 'M') {  // "MIC" typed in a terminal, or "MODE"
            if (read_exact(header + 1, 2) && std::memcmp(header, "MIC", 3) == 0) {
                recognize_microphone(recognizer, pcm, features, raw.get());
            } else if (std::memcmp(header, "MOD", 3) == 0 && read_exact(header + 3, 1) && header[3] == 'E') {
                switch_mode();
            }
            continue;
        }
        if (header[0] == 'B') {
            if (read_exact(header + 1, 3)) {
                if (std::memcmp(header, "BLEI", 4) == 0) { printf("BLE_PROBE %s\n", ble_printer::probe(log_memory) ? "PASS" : "FAIL"); }
                else if (std::memcmp(header, "BLEQ", 4) == 0) { ble_printer::query(log_memory); }
                else if (std::memcmp(header, "BLET", 4) == 0) { run_ble_cycles(recognizer, pcm, features, raw.get()); }
                else if (std::memcmp(header, "BARM", 4) == 0) { g_battery_armed = true; show("USBを外して", "Aボタンで試験"); printf("BATTERY_TEST ARMED\n"); }
                else if (std::memcmp(header, "BLED", 4) == 0) {
                    printf("CYCLE_TRACE_BEGIN\n"); fflush(stdout);
                    size_t sent = 0;
                    while (sent < g_cycle_trace_size) {
                        // Keep the same bounded USB chunks as print_hex; the
                        // full trace can exceed the driver's TX ring.
                        const size_t count = std::min(g_cycle_trace_size - sent, size_t{512});
                        const int written = usb_serial_jtag_write_bytes(g_cycle_trace.get() + sent, count, pdMS_TO_TICKS(500));
                        if (written <= 0) { break; }
                        sent += written;
                    }
                    printf(sent == g_cycle_trace_size ? "CYCLE_TRACE_END\n" : "ERROR cycle_trace_transfer\n");
                }
            }
            continue;
        }
        if (header[0] == 'L') {
            if (read_exact(header + 1, 3)) {
                if (std::memcmp(header, "LABL", 4) == 0) { label_diagnostic(g_last_text, false); }
                else if (std::memcmp(header, "LALL", 4) == 0) { label_diagnostic(label_raster::all_characters(), false); }
                else if (std::memcmp(header, "LMAX", 4) == 0) {
                    std::string maximum;
                    for (int i = 0; i < ralomi::frame_count(kMicSeconds * ralomi::kSampleRate) / kSubsampling; ++i) { maximum += "きゃ"; }
                    label_diagnostic(maximum, false);
                }
                else if (std::memcmp(header, "LFRM", 4) == 0) { voice_screen::dump_pixels(print_hex); }
                else if (std::memcmp(header, "LPRS", 4) == 0) {
                    // Diagnostic: the exact raster of person 0's style <next byte>.
                    uint8_t style = 0;
                    if (read_exact(&style, 1) && prepare_preset(0) && style < g_style_count) {
                        g_style = style; show_styles(false); state = Ui::confirmation;
                        printf("RASTER rows=%d ", g_styles[g_style].rows); print_hex(g_styles[g_style].bits.get(), g_styles[g_style].bytes()); printf("\n");
                    }
                }
            }
            continue;
        }
        if (header[0] == 'I') {  // "INFO": repeat the boot report and the memory since serving started
            if (read_exact(header + 1, 3) && std::memcmp(header, "INFO", 4) == 0) {
                // Model::test() is not repeated: each call leaves about 1.7 MB of PSRAM allocated.
                printf("BOOT_REPORT_BEGIN\n%sBOOT_REPORT_END\n", g_boot_report.c_str());
                log_memory("serving", false);
            }
            continue;
        }
        if (header[0] == 'S' || header[0] == 'N' || header[0] == 'P' || header[0] == 'V') {
            if (read_exact(header + 1, 3)) {
                if (std::memcmp(header, "SHOT", 4) == 0) { voice_screen::dump(print_hex); }
                else if (std::memcmp(header, "NEXT", 4) == 0) { voice_screen::scroll(1); }
                else if (std::memcmp(header, "PREV", 4) == 0) { voice_screen::scroll(-1); }
                else if (std::memcmp(header, "PRNT", 4) == 0) { label_diagnostic(g_last_text, true); }
                else if (std::memcmp(header, "VWAI", 4) == 0) { voice_screen::waiting(); }
                else if (std::memcmp(header, "VREC", 4) == 0) { voice_screen::roles("離して認識", ""); voice_screen::recording_progress(15, -120); }
                else if (std::memcmp(header, "VRES", 4) == 0) { voice_screen::result(g_last_text); }
                else if (std::memcmp(header, "VCON", 4) == 0 || std::memcmp(header, "VPRV", 4) == 0) {
                    if (prepare_styles()) { state = Ui::confirmation; }
                }
                else if (std::memcmp(header, "VPPL", 4) == 0) { if (show_people(0)) { state = Ui::people; } }
                else if (std::memcmp(header, "VPST", 4) == 0) { if (prepare_preset(0)) { state = Ui::confirmation; } }
                // Diagnostic: print person 0's style <next byte> as the buttons would.
                else if (std::memcmp(header, "PPRS", 4) == 0) {
                    uint8_t style = 0;
                    if (read_exact(&style, 1) && prepare_preset(0) && style < g_style_count) {
                        g_style = style; show_styles(false); state = Ui::confirmation; print();
                    }
                }
                else if (std::memcmp(header, "VPRI", 4) == 0) { voice_screen::printing(false); printf("VIEW_ONLY printing\n"); }
                else if (std::memcmp(header, "VOK!", 4) == 0) { voice_screen::success(-1); printf("VIEW_ONLY success\n"); }
                else if (std::memcmp(header, "VERR", 4) == 0) {
                    voice_screen::roles("戻る", "長押しで再印刷"); voice_screen::failure("プリンターにつながりません"); printf("VIEW_ONLY failure\n");
                }
                else if (std::memcmp(header, "VMAX", 4) == 0) {
                    // Explicit display fixture: the 4 s decoder emits at most
                    // floor(frame_count(64000)/4) two-character tokens.
                    // This never changes recognition output or sends a print job.
                    std::string longest;
                    const int steps = ralomi::frame_count(kMicSeconds * ralomi::kSampleRate) / kSubsampling;
                    for (int i = 0; i < steps; ++i) { longest += "きゃ"; }
                    show("最大長表示試験", longest);
                }
                printf("VIEW_READY %.4s\n", header); fflush(stdout);
            }
            continue;
        }
        if (header[0] != 'R') {
            continue;
        }
        if (!read_exact(header + 1, 3)) {
            printf("ERROR header\n");
        } else if (std::memcmp(header, "RLM1", 4) == 0 || std::memcmp(header, "RLYR", 4) == 0) {
            uint8_t size[2];
            const bool sized = read_exact(size, 2);
            const int frames = sized ? size[0] | (size[1] << 8) : 0;
            if (frames < kSubsampling || frames > kFrames ||
                !read_exact(reinterpret_cast<uint8_t *>(features.data()), frames * kBins * sizeof(float))) {
                printf("ERROR frames %d\n", frames);
                continue;
            }
            if (header[2] == 'M') {  // RLM1
                print_result(recognize(recognizer, features.data(), frames));
            } else {
                probe_layers(recognizer, features.data(), frames);
            }
        } else if (std::memcmp(header, "RLMS", 4) == 0) {
            // Explicit raw-PCM diagnostic, paced over four seconds; no speaker output.
            if (!read_exact(reinterpret_cast<uint8_t *>(raw.get()), kMicSeconds * ralomi::kSampleRate * sizeof(int16_t))) {
                printf("ERROR stream_pcm\n");
                continue;
            }
            recognize_microphone(recognizer, pcm, features, raw.get(), true);
        } else if (std::memcmp(header, "RLMA", 4) == 0) {
            // uint32 samples | uint8 dump_features | samples * int16 PCM (16 kHz mono)
            uint8_t size[5];
            const bool sized = read_exact(size, 5);
            const int samples = sized ? size[0] | (size[1] << 8) | (size[2] << 16) | (size[3] << 24) : 0;
            if (samples < ralomi::kWindow + (kSubsampling - 1) * ralomi::kHop || samples > kMaxSamples ||
                !read_exact(reinterpret_cast<uint8_t *>(pcm16_area(pcm)), samples * sizeof(int16_t))) {
                printf("ERROR samples %d\n", samples);
                continue;
            }
            widen_pcm16(pcm, samples);
            Recognition result = recognize_pcm(recognizer, pcm.data(), samples, features);
            if (size[4]) {
                printf("FEATURES %d ", result.frames * kBins);
                print_hex(reinterpret_cast<const uint8_t *>(features.data()), result.frames * kBins * sizeof(float));
                printf("\n");
            }
            print_result(result);
            g_last_text = result.text;
            voice_screen::result(g_last_text);
        } else {
            printf("ERROR header\n");
        }
    }
}

void recognition_task(void *)
{
    log_memory("boot");
    if (!tts::begin()) { report("ERROR TTS init"); }
    home::set_speaker(speak_response);
    action::set_speaker(speak_response);
    const bool action_mode = g_mode == Mode::action;
    report("MODE %s partition=%s", action_mode ? "action" : "kana", action_mode ? "koeact" : "model");
    if (action_mode) { voice_screen::mode_switching("じゅんび中…"); }
    else if (g_voice_print) { voice_screen::mode_switching("いんさつのじゅんび中…"); }
    const int64_t load_start = esp_timer_get_time();
    auto *model = new dl::Model(action_mode ? "koeact" : "model", fbs::MODEL_LOCATION_IN_FLASH_PARTITION);
    report("LOAD us=%" PRId64, esp_timer_get_time() - load_start);
    log_memory("model_loaded");
    static Recognizer recognizer(model);
    dl::TensorBase *input = recognizer.features_input();
    dl::TensorBase *output = model->get_outputs().at("logits");
    report("IO input=%s exp=%d output=%s exp=%d chunk_frames=%d states=%d layers=%d",
           dl::dtype_to_string(input->get_dtype()),
           input->get_exponent(),
           dl::dtype_to_string(output->get_dtype()),
           output->get_exponent(),
           recognizer.chunk_frames(),
           recognizer.state_count(),
           recognizer.has_layers());
    if (action_mode) {
        // Model::test() is skipped here: its test values stay allocated and PSRAM is short.
        model->profile_memory();
        benchmark(recognizer);
        if (!recognizer.has_layers() || !a2_init(recognizer.chunk_frames() / kSubsampling)) {
            report("ERROR koeact layers=%d decoder=%d", recognizer.has_layers(), g_a2_ready);
        }
        heap_caps_monitor_local_minimum_free_size_start();
        serve_action(recognizer);
        return;
    }
    if (g_voice_print) {
        // Straight to listening: the golden-vector test and benchmark would add seconds to the
        // switch the person is waiting for.
        heap_caps_monitor_local_minimum_free_size_start();
        serve_voice_print(recognizer);
        return;
    }
    diagnose(recognizer);
    heap_caps_monitor_local_minimum_free_size_start();
    serve(recognizer);
}

}  // namespace

extern "C" void app_main(void)
{
    report("BOOT reset_reason=%d boot_id=%08" PRIx32, (int)esp_reset_reason(), esp_random());
    auto m5 = M5.config();
    M5.begin(m5);
    M5.Speaker.end();  // CoreS3 shares I2S between the speaker and the ES7210 microphone
    M5.Display.setTextWrap(true);
    if (!voice_screen::begin()) {
        ESP_LOGE(TAG, "ERROR screen_buffer allocation");
        return;
    }
    report("BOARD id=%d stopwatch=%d display=%dx%d touch=%d mic_enabled=%d buttons=A(GPIO2),B(GPIO1)",
           (int)M5.getBoard(), M5.getBoard() == m5::board_t::board_M5StopWatch,
           M5.Display.width(), M5.Display.height(), M5.Touch.isEnabled(), M5.Mic.isEnabled());
    ralomi::features_init();
    // NVS holds the mode. BLE also calls nvs_flash_init later, which then returns ESP_OK.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    g_mode = nvs == ESP_OK ? load_mode() : Mode::kana;
    g_voice_print = nvs == ESP_OK && take_voice_print_flag() && g_mode == Mode::kana;
    report("NVS init=%s voice_print=%d", esp_err_to_name(nvs), g_voice_print);
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = 16384;
    config.tx_buffer_size = 8192;  // print_hex writes 512-character chunks; the default ring is 256 bytes
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();
    auto *serial_storage = static_cast<uint8_t *>(heap_caps_malloc(config.rx_buffer_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!serial_storage) { ESP_LOGE(TAG, "ERROR serial_queue_allocation"); return; }
    serial_commands = xStreamBufferCreateStatic(config.rx_buffer_size + 1, 1, serial_storage, &serial_buffer_storage);
    if (!serial_commands || xTaskCreatePinnedToCore(serial_receiver, "screen_serial", 8192, nullptr, 4, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "ERROR serial_receiver_task"); return;
    }
    xTaskCreatePinnedToCore(recognition_task, "ralomi", 32768, nullptr, 5, nullptr, 0);
}
