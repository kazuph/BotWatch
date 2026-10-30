#include "tts.h"
#include "tts_text.h"
#include "tts_level.h"
#include "tts_phrases.h"
#include <M5Unified.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "features.h"
extern "C" {
#include "saanotts.h"
#include "saanotts_stream.h"
#include "saan_pcm.h"
#include "g2p.h"
}

namespace tts {
static_assert([] {
    for(const auto &phrase:kTtsPhrases) if(!(phrase.gain>0)) return false;
    return true;
}(),"Generate all phrase gains with scripts/generate_tts_levels.py before firmware build");
namespace {
saan_weights weights;
saan_arena arena;
saan_stream stream;
char *kana;
int32_t *ids;
int32_t ids_capacity;
int16_t *chunks;
// M5Unified Speaker_Class.hpp requires three buffers without a release callback.
constexpr size_t kPlaybackBuffers = 3;
size_t kana_bytes;
// Serial-only test preference survives the existing voice-print mode reboot.
RTC_NOINIT_ATTR bool saved_muted;
std::atomic<bool> muted{false};
bool ready = false;
esp_partition_mmap_handle_t mapping;
void memory(const char *stage) {
    printf("TTS_MEM stage=%s internal=%u psram=%u largest_internal=%u\n", stage,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
}
bool is_ready() { return ready; }
bool is_muted() { return muted.load(); }
void diagnostic(bool value) { saved_muted = value; muted.store(value); if(value)M5.Speaker.stop(); printf("TTS_DIAGNOSTIC muted=%d pcm=%d\n", value, value); fflush(stdout); }
void diagnostic_phrases(void (*speak)(const char *)) {
    if(!muted.load()) { printf("ERROR TTS_TABLE mute_required\n");return; }
    unsigned count=0;
    for(const auto &phrase:kTtsPhrases) {
        if(!muted.load()) { printf("ERROR TTS_TABLE mute_required\n");return; }
        speak(phrase.caption);
        ++count;
    }
    printf("TTS_TABLE_DONE phrases=%u\n",count);fflush(stdout);
}
void restore_diagnostic() {
    if (esp_reset_reason() != ESP_RST_SW) { saved_muted = false; }
    muted.store(saved_muted);
}
bool begin() {
    restore_diagnostic();
    memory("before_init");
    const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "tts");
    const void *blob = nullptr;
    if (!partition || esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &blob, &mapping) != ESP_OK ||
        saan_weights_open(&weights, blob, partition->size) != SAAN_OK) {
        printf("ERROR TTS weights\n"); return false;
    }
    kana_bytes = kana_capacity();
    ids_capacity = saan_g2p_capacity(kana_bytes);
    auto allocate = [](size_t n) { return heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); };
    kana = static_cast<char *>(allocate(kana_bytes));
    ids = static_cast<int32_t *>(allocate(ids_capacity * sizeof(int32_t)));
    const size_t arena_bytes = saan_stream_arena_peak(ids_capacity);
    void *workspace = allocate(arena_bytes);
    chunks = static_cast<int16_t *>(allocate(kPlaybackBuffers * SAAN_CHUNK * SAAN_HOP * sizeof(int16_t)));
    ready = kana && ids && workspace && chunks;
    if (ready) { saan_arena_init(&arena, workspace, arena_bytes); }
    printf("TTS_INIT ready=%d arena=%u ids_capacity=%d kana_bytes=%u weights=%u\n", ready,
           (unsigned)arena_bytes, (int)ids_capacity, (unsigned)kana_bytes, (unsigned)weights.size);
    memory("initialized");
    return ready;
}
bool say(const char *caption) {
    const bool silent = muted.load();
    float gain=0;
    if (!ready || !intermediate(caption, kana, kana_bytes,&gain) || !(gain>0)) {
        printf("ERROR TTS unsupported caption=%s\n", caption); return false;
    }
    int32_t count = 0;
    saan_g2p_info info;
    if (saan_g2p(kana, std::strlen(kana), ids, ids_capacity, &count, &info) != SAAN_G2P_OK) {
        printf("ERROR TTS g2p caption=%s\n", caption); return false;
    }
    printf("TTS_BEGIN caption=%s kana=%s ids=%d muted=%d\n", caption, kana, (int)count, silent);
    fflush(stdout);
    saan_arena_reset(&arena);
    saan_pcm_reset();
    // Keep the preceding interval visible before starting an allocation-tracked
    // minimum for synthesis through the end of speaker playback (not sampling).
    printf("TTS_RAM_PREVIOUS internal_min=%u psram_min=%u\n",
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    const bool ram_monitor = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
    const int64_t begin_us = esp_timer_get_time();
    saan_status status = saan_stream_init(&stream, &weights, &arena, ids, count, SAAN_S_V);
    int64_t synth_us = esp_timer_get_time() - begin_us;
    double raw_energy=0;float raw_absolute_peak=0;
    unsigned limited_samples=0;
    const auto speaker=M5.Speaker.config();
    const unsigned dac=tts_level::dac_volume(speaker.magnification,speaker.stereo);
    printf("TTS_NORMALIZE gain=%.9f target_dbfs=-1 master=%u magnification=%u stereo=%d dac_target=%u\n",
        gain,M5.Speaker.getVolume(),speaker.magnification,speaker.stereo,dac);
    bool audio_ok = true;
    if (!silent && status == SAAN_OK) {
        audio_ok = M5.Speaker.begin();
        if(audio_ok && M5.getBoard()==m5::board_t::board_M5StopWatch) {
            // Same ES8311 address/register/bus rate as the pinned StopWatch callback.
            audio_ok=M5.In_I2C.writeRegister8(0x18,0x32,dac,100000);
            const auto observed=M5.In_I2C.readRegister8(0x18,0x32,100000);
            printf("TTS_DAC target=%u observed=%u\n",dac,observed);
            audio_ok=audio_ok && observed==dac;
        }
    }
    int seq = 0, underruns = 0, first_samples = 0;
    tts_level::Loudness loudness(SAAN_SR);
    while (status == SAAN_OK) {
        int32_t n = 0;
        const float *pcm = nullptr;
        const int64_t pull_start = esp_timer_get_time();
        status = saan_stream_pull_ptr(&stream, &pcm, &n);
        n *= SAAN_HOP;
        synth_us += esp_timer_get_time() - pull_start;
        if (status != SAAN_OK || n == 0) { break; }
        int16_t *chunk = chunks + (seq % kPlaybackBuffers) * SAAN_CHUNK * SAAN_HOP;
        for (int i = 0; i < n; ++i) {
            raw_energy+=double(pcm[i])*pcm[i];
            raw_absolute_peak=std::max(raw_absolute_peak,std::abs(pcm[i]));
            const float scaled=pcm[i]*gain,limited=loudness.process(scaled);
            limited_samples+=limited!=scaled;
            chunk[i]=saan_f32_to_i16(limited);
        }
        if (silent) {
            printf("TTS_PCM seq=%d n=%d ", seq, (int)n);
            for (int i = 0; i < n; ++i) { const uint16_t x = chunk[i]; printf("%02x%02x", x & 255, x >> 8); }
            printf("\n"); fflush(stdout);
        } else if (audio_ok) {
            // Fill both speaker queue slots before starting. The three existing
            // buffers then let synthesis run ahead without a startup gap.
            if (seq == 0) { first_samples = n; }
            else {
                if (seq == 1) { audio_ok = M5.Speaker.playRaw(chunks, first_samples, SAAN_SR, false, 1, 0); }
                else if (!M5.Speaker.isPlaying()) { ++underruns; }
                if (audio_ok) { audio_ok = M5.Speaker.playRaw(chunk, n, SAAN_SR, false, 1, 0); }
            }
        }
        ++seq;
        vTaskDelay(1);
    }
    if (!silent) {
        if (seq == 1 && audio_ok) { audio_ok = M5.Speaker.playRaw(chunks, first_samples, SAAN_SR, false, 1, 0); }
        while (M5.Speaker.isPlaying()) { vTaskDelay(1); }
        M5.Speaker.end();
    }
    const uint32_t samples = saan_pcm_samples();
    const double raw_rms=samples?32767*std::sqrt(raw_energy/samples):0;
    const double rms = samples ? std::sqrt(double(saan_pcm_sqsum()) / samples) : 0;
    printf("TTS_END status=%d audio_ok=%d underruns=%d samples=%u rate=%d rms=%.6f synth_us=%lld elapsed_us=%lld rtf=%.6f arena_peak=%u clips=%u fnv=%016llx ram_monitor=%d psram_min=%u internal_min=%u peak=%d gain=%.6f raw_rms=%.6f raw_peak=%.6f limiter_hits=%u\n",
           status, audio_ok, underruns, (unsigned)samples, SAAN_SR, rms, (long long)synth_us,
           (long long)(esp_timer_get_time() - begin_us), samples ? synth_us * (SAAN_SR / 1e6) / samples : 0,
           (unsigned)arena.peak, (unsigned)saan_pcm_clip_count(), (unsigned long long)saan_pcm_checksum(),
           ram_monitor, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           int(saan_pcm_absmax()),gain,raw_rms,32767*raw_absolute_peak,limited_samples);
    memory("after_speech"); fflush(stdout);
    return status == SAAN_OK && audio_ok && samples > 0;
}
}
