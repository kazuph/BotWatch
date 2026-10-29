#pragma once
#include <algorithm>
#include <cmath>

namespace tts_level {
// TALK-BRIEF: -1 dBFS. Keep normalization outside the unmodified sanoTTS core.
constexpr float kTargetPeak=0.891250938f; // pow(10, -1/20)
inline float gain(float absolute_peak) { return absolute_peak>0?kTargetPeak/absolute_peak:1; }
inline float limit(float scaled) { return std::clamp(scaled,-kTargetPeak,kTargetPeak); }

// The small StopWatch speaker barely reproduces low frequencies, and peak
// normalization alone leaves speech far below full scale on average. Remove the
// lows, add makeup gain and hold the peak with an instant-attack limiter.
constexpr float kLoudnessBoostDb=15.0f;
constexpr float kHighPassHz=250.0f;
constexpr float kReleaseMs=40.0f;
class Loudness {
    float b0=1,b1=0,b2=0,a1=0,a2=0,x1=0,x2=0,y1=0,y2=0,envelope=0,release=0,boost=1;
public:
    explicit Loudness(float sample_rate) {
        // RBJ cookbook second-order high-pass, Q = 1/sqrt(2).
        const double w=2*M_PI*kHighPassHz/sample_rate,alpha=std::sin(w)/(2*M_SQRT1_2),c=std::cos(w),a0=1+alpha;
        b0=float((1+c)/2/a0);b1=float(-(1+c)/a0);b2=b0;a1=float(-2*c/a0);a2=float((1-alpha)/a0);
        release=float(std::exp(-1.0/(kReleaseMs*1e-3*sample_rate)));
        boost=float(std::pow(10.0,kLoudnessBoostDb/20.0));
    }
    float process(float x) {
        const float y=b0*x+b1*x1+b2*x2-a1*y1-a2*y2;
        x2=x1;x1=x;y2=y1;y1=y;
        const float boosted=y*boost,level=std::abs(boosted);
        envelope=level>envelope?level:envelope*release+level*(1-release);
        return limit(envelope>kTargetPeak?boosted*kTargetPeak/envelope:boosted);
    }
};
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
