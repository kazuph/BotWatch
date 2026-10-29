#pragma once
#include <algorithm>
#include <cmath>

namespace tts_level {
// TALK-BRIEF: -1 dBFS. Keep normalization outside the unmodified sanoTTS core.
constexpr float kTargetPeak=0.891250938f; // pow(10, -1/20)
inline float gain(float absolute_peak) { return absolute_peak>0?kTargetPeak/absolute_peak:1; }
inline float limit(float scaled) { return std::clamp(scaled,-kTargetPeak,kTargetPeak); }
// Pinned M5Unified: stereo interpolation, squared master/channel controls and >>8 output.
inline double maximum_i2s_gain(unsigned magnification,bool stereo)
{
    return double(magnification)*(stereo?2:1)*std::pow(255.0,4)/std::pow(2.0,36);
}
// ES8311 DAC_VOLUME: 0xBF = 0 dB, 0.5 dB/step. Choose the largest non-clipping step.
inline unsigned dac_volume(unsigned magnification,bool stereo)
{
    const double headroom=1/(maximum_i2s_gain(magnification,stereo)*kTargetPeak);
    return std::clamp(0xbf+int(std::floor(40*std::log10(headroom))),0,255);
}
}
