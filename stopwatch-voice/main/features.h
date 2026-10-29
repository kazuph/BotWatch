// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ayousanz
//
// Causal 40-bin log-Mel frontend matching the released features.py (ja-reading-v0 model):
// 16 kHz, 400-sample periodic Hann window, 160-sample hop, 512-point FFT power spectrum,
// HTK triangular mel filterbank 20 Hz..8 kHz, natural log with a 1e-10 floor, no normalization.
#pragma once

#include <cstdint>

namespace ralomi {

constexpr int kSampleRate = 16000;
constexpr int kWindow = 400;
constexpr int kHop = 160;
constexpr int kFft = 512;
constexpr int kMelBins = 40;

// Number of complete frames for `samples` input samples (no padding or centering).
int frame_count(int samples);

// Initialise the window, filterbank and FFT tables once.
void features_init();

// Write frame_count(samples) * kMelBins features for float PCM in [-1, 1].
void log_mel(const float *pcm, int samples, float *features);

}  // namespace ralomi
