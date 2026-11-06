#include "vision.h"
#include "human_face_detect.hpp"
#include "imagenet_cls.hpp"
#include "dl_module_softmax.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstdio>
#include <new>

namespace vision {
namespace {
HumanFaceDetect* detector = nullptr;
ImageNetCls* classifier = nullptr;
Mode active = Mode::off;
void log_memory(const char* stage, int64_t elapsed_us) {
    printf("VISION %s us=%lld internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u\n",
           stage, static_cast<long long>(elapsed_us),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
}
void stop() {
    delete detector; detector = nullptr;
    delete classifier; classifier = nullptr;
    active = Mode::off;
}
void set_mode(Mode mode) {
    if (active == mode) return;
    stop();
    active = mode;
}
bool faces(const uint8_t* pixels, uint16_t width, uint16_t height, std::vector<Box>& out) {
    if (active != Mode::faces || !pixels) return false;
    if (!detector) detector = new(std::nothrow) HumanFaceDetect();
    if (!detector) return false;
    dl::image::img_t img = {const_cast<uint8_t*>(pixels), width, height, dl::image::DL_IMAGE_PIX_TYPE_RGB565BE};
    int64_t start = esp_timer_get_time();
    const auto& results = detector->run(img);
    out.clear();
    for (const auto& result : results) {
        if (result.box.size() >= 4)
            out.push_back({result.box[0], result.box[1], result.box[2], result.box[3], result.score});
    }
    log_memory("face", esp_timer_get_time() - start);
    return true;
}
bool classify(const uint8_t* pixels, uint16_t width, uint16_t height, std::vector<Prediction>& out) {
    if (active != Mode::classify || !pixels) return false;
    if (!classifier) classifier = new(std::nothrow) ImageNetCls();
    if (!classifier) return false;
    dl::image::img_t img = {const_cast<uint8_t*>(pixels), width, height, dl::image::DL_IMAGE_PIX_TYPE_RGB565BE};
    int64_t start = esp_timer_get_time();
    classifier->run(img);
    dl::TensorBase* logits = classifier->get_raw_model()->get_output("");
    if (!logits || logits->get_size() != 1000) return false;
    dl::TensorBase probabilities(logits->shape, nullptr, 0, dl::DATA_TYPE_FLOAT);
    dl::module::Softmax softmax(nullptr, -1, dl::MODULE_NON_INPLACE, dl::QUANT_TYPE_SYMM_8BIT);
    softmax.run(logits, &probabilities);
    float* values = static_cast<float*>(probabilities.data);
    out.clear();
    for (int i = 0; i < logits->get_size(); ++i) out.push_back({i, values[i]});
    std::partial_sort(out.begin(), out.begin() + 3, out.end(),
                      [](const Prediction& a, const Prediction& b) { return a.probability > b.probability; });
    out.resize(3);
    log_memory("classification", esp_timer_get_time() - start);
    return true;
}
}
