#pragma once

#include <cstdint>
#include <array>
#include <span>
#include "audio_spectrum.h"
#include <string_view>


namespace tuner {

constexpr int calibrationMinHz = 410;
constexpr int calibrationMaxHz = 480;
constexpr int calibrationDefaultHz = 440;

struct PitchResult {
    bool valid = false;
    float frequencyHz = 0.0f;
    float cents = 0.0f;
    float confidence = 0.0f;
    int midi = 69;
    int octave = 4;
    int pitchClass = 9;
};

class PitchDetector {
    mutable audio_spectrum::Workspace workspace;
public:
    static constexpr uint32_t sampleRate = 16000;
    static constexpr std::size_t frameSize = 2048;

    static constexpr size_t hopSize=512;
    static constexpr float yinThreshold=0.15f;
    static constexpr float silenceDbfs=-52; // parent's approved initial kOnsetFloorDb
    static constexpr int stableFrames=3;
    static constexpr float stableCents=10;
    static constexpr int holdSamples=sampleRate/2;
    bool begin() noexcept {
        return workspace.begin(frameSize);
    }
    void end() noexcept {workspace.end();}
    size_t bytes() const noexcept {return workspace.bytes();}
    PitchResult analyze(std::span<const int16_t> samples, float calibrationHz) const;
};

class Tracker {
    PitchDetector detector;
    std::array<PitchResult,PitchDetector::stableFrames> history{};
    size_t historyCount=0;
    PitchResult shown,lastAnalysis;
    int samplesSinceUpdate=0;
public:
    bool begin() noexcept {reset();return detector.begin();}
    void end() noexcept {reset();detector.end();}
    size_t bytes() const noexcept {return detector.bytes();}
    PitchResult update(std::span<const int16_t> samples,float calibrationHz);
    const PitchResult &latestAnalysis() const {return lastAnalysis;}
    int displayAgeSamples() const {return samplesSinceUpdate;}
    void reset() { historyCount=0;shown={};lastAnalysis={};samplesSinceUpdate=0; }
};
float midiFrequency(int midi, float calibrationHz);
std::string_view katakanaNoteName(int pitchClass);

}  // namespace tuner
