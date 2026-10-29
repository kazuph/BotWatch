// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ayousanz

#include "features.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dsps_fft2r.h"

namespace ralomi {
namespace {

constexpr int kBinsHalf = kFft / 2 + 1;
float g_window[kWindow];
// Sparse triangular filters: first non-zero FFT bin and its weights per mel bin.
int g_filter_start[kMelBins];
int g_filter_length[kMelBins];
float g_filter_weight[kMelBins][kBinsHalf];
alignas(16) float g_fft[kFft * 2];
bool g_ready = false;

double hz_to_mel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double mel_to_hz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

}  // namespace

int frame_count(int samples) { return samples < kWindow ? 0 : (samples - kWindow) / kHop + 1; }

void features_init()
{
    if (g_ready) {
        return;
    }
    for (int n = 0; n < kWindow; ++n) {
        // torch.hann_window(400, periodic=True), computed in double then rounded to float
        g_window[n] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * n / kWindow));
    }
    // build_mel_filterbank: float64 mel edges and FFT frequencies, cast to float32 at the end
    const double low = hz_to_mel(20.0);
    const double high = hz_to_mel(kSampleRate / 2.0);
    double edges[kMelBins + 2];
    for (int i = 0; i < kMelBins + 2; ++i) {
        edges[i] = mel_to_hz(low + (high - low) * i / (kMelBins + 1));
    }
    for (int m = 0; m < kMelBins; ++m) {
        g_filter_start[m] = -1;
        g_filter_length[m] = 0;
        for (int k = 0; k < kBinsHalf; ++k) {
            const double frequency = (kSampleRate / 2.0) * k / (kBinsHalf - 1);
            const double rising = (frequency - edges[m]) / (edges[m + 1] - edges[m]);
            const double falling = (edges[m + 2] - frequency) / (edges[m + 2] - edges[m + 1]);
            const double weight = std::max(0.0, std::min(rising, falling));
            if (weight > 0.0) {
                if (g_filter_start[m] < 0) {
                    g_filter_start[m] = k;
                }
                g_filter_weight[m][g_filter_length[m]++] = static_cast<float>(weight);
            }
        }
    }
    dsps_fft2r_init_fc32(nullptr, kFft);
    g_ready = true;
}

void log_mel(const float *pcm, int samples, float *features)
{
    const int frames = frame_count(samples);
    float power[kBinsHalf];
    for (int f = 0; f < frames; ++f) {
        const float *frame = pcm + f * kHop;
        for (int n = 0; n < kFft; ++n) {
            g_fft[2 * n] = n < kWindow ? frame[n] * g_window[n] : 0.0f;
            g_fft[2 * n + 1] = 0.0f;
        }
        dsps_fft2r_fc32(g_fft, kFft);
        dsps_bit_rev_fc32(g_fft, kFft);
        for (int k = 0; k < kBinsHalf; ++k) {
            power[k] = g_fft[2 * k] * g_fft[2 * k] + g_fft[2 * k + 1] * g_fft[2 * k + 1];
        }
        float *out = features + f * kMelBins;
        for (int m = 0; m < kMelBins; ++m) {
            float energy = 0.0f;
            const float *weights = g_filter_weight[m];
            const float *bins = power + g_filter_start[m];
            for (int i = 0; i < g_filter_length[m]; ++i) {
                energy += bins[i] * weights[i];
            }
            out[m] = std::log(std::max(energy, 1e-10f));
        }
    }
}

}  // namespace ralomi
