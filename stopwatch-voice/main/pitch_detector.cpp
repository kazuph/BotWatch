// SPDX-License-Identifier: MIT
// Copyright (c) 2026 M5Stack Technology CO LTD
// Adapted for BotWatch; see licenses/M5StopWatch-MIT.txt.
#include "pitch_detector.h"
#include "audio_spectrum.h"
#ifdef ESP_PLATFORM
#include "dsps_dotprod.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace tuner {
namespace {




}  // namespace

float midiFrequency(int midi, float calibrationHz)
{
    return calibrationHz * std::pow(2.0f, static_cast<float>(midi - 69) / 12.0f);
}

std::string_view katakanaNoteName(int pitchClass)
{
    static constexpr std::array<std::string_view, 12> names = {
        "ド", "レフラット", "レ", "ミフラット", "ミ", "ファ",
        "ソフラット", "ソ", "ラフラット", "ラ", "シフラット", "シ",
    };
    const int normalized = ((pitchClass % 12) + 12) % 12;
    return names[static_cast<std::size_t>(normalized)];
}

PitchResult PitchDetector::analyze(std::span<const int16_t> samples, float calibrationHz) const
{
    PitchResult result;
    if (!workspace.ready() || samples.size() != frameSize || calibrationHz < calibrationMinHz || calibrationHz > calibrationMaxHz) {
        return result;
    }

    const float lowestFrequencyHz=midiFrequency(33,calibrationHz);
    const float highestFrequencyHz=midiFrequency(108,calibrationHz);
    double mean=0,energy=0;
    for(auto sample:samples)mean+=sample;
    mean/=samples.size();
    for(auto sample:samples)energy+=(sample-mean)*(sample-mean);
    if(energy/samples.size() < 32768.0*32768.0*std::pow(10.0,silenceDbfs/10.0)) return result;
    const std::size_t minTau = static_cast<std::size_t>(std::ceil(sampleRate / highestFrequencyHz));
    const std::size_t maxTau = std::min(frameSize / 2, static_cast<std::size_t>(sampleRate / lowestFrequencyHz));
    const std::size_t analysisWindow = frameSize - maxTau;
    // A1 is A4 / 8; the longest allowed period is at the minimum calibration.
    constexpr size_t tauCount=sampleRate*8/calibrationMinHz+1;
    struct YinScratch {float centered[frameSize];double prefix[frameSize+1];float difference[tauCount],normalized[tauCount];};
    static_assert(sizeof(YinScratch)<=frameSize*audio_spectrum::padding*sizeof(std::complex<float>));
    auto &work=*std::construct_at(static_cast<YinScratch*>(workspace.scratchData()));
    auto *difference=work.difference,*normalized=work.normalized,*centered=work.centered;
    auto *prefix=work.prefix;
    for(size_t i=0;i<frameSize;++i){centered[i]=samples[i]-mean;prefix[i+1]=prefix[i]+double(centered[i])*centered[i];}
    for (std::size_t tau = 1; tau <= maxTau; ++tau) {
        float dot=0;
#ifdef ESP_PLATFORM
        dsps_dotprod_f32(centered,centered+tau,&dot,analysisWindow);
#else
        for(size_t i=0;i<analysisWindow;++i)dot+=centered[i]*centered[i+tau];
#endif
        difference[tau]=std::max(0.0,double(prefix[analysisWindow]+prefix[tau+analysisWindow]-prefix[tau])-2*dot);
    }

    float runningSum = 0.0f;
    for (std::size_t tau = 1; tau <= maxTau; ++tau) {
        runningSum += difference[tau];
        normalized[tau] = runningSum > 0.0f ? difference[tau] * static_cast<float>(tau) / runningSum : 1.0f;
    }

    std::size_t bestTau = 0;
    for (std::size_t tau = minTau; tau <= maxTau; ++tau) {
        if (normalized[tau] >= yinThreshold) {
            continue;
        }
        while (tau + 1 <= maxTau && normalized[tau + 1] < normalized[tau]) {
            ++tau;
        }
        bestTau = tau;
        break;
    }
    if (bestTau == 0) {
        return result;
    }

    const float confidence=1.0f-normalized[bestTau];
    const auto spectrum=workspace.power(samples);
    // FFT no longer needs its complex array; the persistent power bins stay
    // separate while candidate peaks occupy the same checked PSRAM scratch.
    auto *peakStorage=static_cast<audio_spectrum::Peak*>(workspace.scratchData());
    static_assert(sizeof(audio_spectrum::Peak)/2<=sizeof(std::complex<float>));
    std::uninitialized_value_construct_n(peakStorage,(workspace.bins()-1)/2);
    const double floor=32768*std::pow(10.0,silenceDbfs/20.0)*std::sqrt(2.0);
    size_t peakCount=0;
    const double spectralFloor=std::pow(floor*audio_spectrum::a0*samples.size()/2,2);
    for(size_t i=1;i+1<spectrum.size();++i) {
        if(spectrum[i]<spectralFloor || spectrum[i]<=spectrum[i-1] || spectrum[i]<=spectrum[i+1])continue;
        auto peak=audio_spectrum::peak(spectrum,i,samples.size(),sampleRate);
        if(peak.amplitude>=floor)peakStorage[peakCount++]=peak;
    }
    const std::span<const audio_spectrum::Peak> peaks(peakStorage,peakCount);
    double bestScore=0,frequency=0;
    const double boundary=std::pow(2.0,2.0/1200);
    for(const auto &base:peaks) {
        if(base.hz<lowestFrequencyHz/boundary || base.hz>highestFrequencyHz*boundary)continue;
        double score=0,strongest=0,refined=base.hz;
        // Match the fundamental and its second/third harmonics, including
        // their aliases above Nyquist. A real fundamental line is required.
        for(int harmonic=1;harmonic<=3;++harmonic) {
            const double unfolded=harmonic*base.hz;
            const double folded=std::abs(std::remainder(unfolded,double(sampleRate)));
            const auto nearest=std::min_element(peaks.begin(),peaks.end(),[&](const auto &x,const auto &y){return std::abs(x.hz-folded)<std::abs(y.hz-folded);});
            if(nearest==peaks.end())continue;
            const double measured=unfolded+(std::remainder(unfolded,double(sampleRate))>=0?1:-1)*(nearest->hz-folded);
            const double corrected=measured/harmonic;
            if(corrected<=0 || std::abs(1200*std::log2(corrected/base.hz))>stableCents)continue;
            score+=nearest->amplitude*nearest->amplitude;
            if(nearest->amplitude>strongest){strongest=nearest->amplitude;refined=corrected;}
        }
        if(score>bestScore){bestScore=score;frequency=refined;}
    }
    if(bestScore==0)return result;
    const int midi = static_cast<int>(std::lround(69.0f + 12.0f * std::log2(frequency / calibrationHz)));
    const float target = midiFrequency(midi, calibrationHz);
    result.valid = std::isfinite(frequency) && frequency >= lowestFrequencyHz*std::pow(2.0f,-2.0f/1200) && frequency <= highestFrequencyHz*std::pow(2.0f,2.0f/1200);
    result.frequencyHz = frequency;
    result.cents = 1200.0f * std::log2(frequency / target);
    result.confidence = confidence;
    result.midi = midi;
    result.octave = midi / 12 - 1;
    result.pitchClass = ((midi % 12) + 12) % 12;
    return result;
}

PitchResult Tracker::update(std::span<const int16_t> samples,float calibrationHz) {
    auto current=detector.analyze(samples,calibrationHz);lastAnalysis=current;
    samplesSinceUpdate=std::min(samplesSinceUpdate+int(PitchDetector::hopSize),PitchDetector::holdSamples);
    if(!current.valid) {
        historyCount=0;
        if(samplesSinceUpdate>=PitchDetector::holdSamples)shown={};
        return shown;
    }
    if(historyCount && std::abs(1200*std::log2(current.frequencyHz/history[historyCount-1].frequencyHz))>PitchDetector::stableCents)historyCount=0;
    if(historyCount==history.size()){std::move(history.begin()+1,history.end(),history.begin());--historyCount;}
    history[historyCount++]=current;
    if(historyCount==PitchDetector::stableFrames) {
        float low=current.frequencyHz,high=low;
        for(auto &p:history){low=std::min(low,p.frequencyHz);high=std::max(high,p.frequencyHz);}
        if(1200*std::log2(high/low)<=PitchDetector::stableCents) {
            auto sorted=history;std::sort(sorted.begin(),sorted.end(),[](const auto &a,const auto &b){return a.frequencyHz<b.frequencyHz;});
            shown=sorted[PitchDetector::stableFrames/2];samplesSinceUpdate=0;
        }
    }
    if(samplesSinceUpdate>=PitchDetector::holdSamples)shown={};
    return shown;
}

}  // namespace tuner
