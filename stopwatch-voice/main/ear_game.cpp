// SPDX-License-Identifier: MIT
// Copyright (c) 2026 M5Stack Technology CO LTD
// See licenses/M5StopWatch-MIT.txt.
#include "ear_game.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace ear_game {
namespace {

constexpr std::array<const char*, 12> noteNames = {
    "ド", "レフラット", "レ", "ミフラット", "ミ", "ファ",
    "ソフラット", "ソ", "ラフラット", "ラ", "シフラット", "シ",
};
constexpr std::array<int, 7> cMajorPitchClasses = {0, 2, 4, 5, 7, 9, 11};
constexpr std::array<std::array<int, 8>, 3> cScales = {
    std::array<int, 8>{60, 62, 64, 65, 67, 69, 71, 72},
    std::array<int, 8>{60, 62, 63, 65, 67, 68, 70, 72},
    std::array<int, 8>{60, 62, 63, 65, 67, 68, 71, 72},
};
constexpr std::array<const char*, 3> scaleNames = {"ちょうおんかい", "しぜんてきたんおんかい", "わせいてきたんおんかい"};

std::string noteLabel(int midi)
{
    return noteNames[static_cast<std::size_t>(((midi % 12) + 12) % 12)];
}

float midiFrequency(int midi)
{
    return 440.0f * std::pow(2.0f, static_cast<float>(midi - 69) / 12.0f);
}

template <typename Generator>
std::array<int, 3> distinctChoices(int correct, int range, Generator& generator)
{
    std::array<int, 3> values = {correct, correct, correct};
    std::uniform_int_distribution<int> distribution(0, range - 1);
    for (std::size_t i = 1; i < values.size(); ++i) {
        do {
            values[i] = distribution(generator);
        } while (std::find(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(i), values[i]) !=
                 values.begin() + static_cast<std::ptrdiff_t>(i));
    }
    std::shuffle(values.begin(), values.end(), generator);
    return values;
}

std::array<int, 3> chordNotes(int degree)
{
    return {60 + cMajorPitchClasses[degree],
            60 + cMajorPitchClasses[(degree + 2) % 7] + (degree + 2 >= 7 ? 12 : 0),
            60 + cMajorPitchClasses[(degree + 4) % 7] + (degree + 4 >= 7 ? 12 : 0)};
}

}  // namespace

Question makeQuestion(Mode mode, uint32_t seed)
{
    std::mt19937 generator(seed);
    Question question;
    question.mode = mode;

    if (mode == Mode::Note) {
        std::uniform_int_distribution<int> distribution(0, 11);
        const int pitchClass = distribution(generator);
        const auto options = distinctChoices(pitchClass, 12, generator);
        for (std::size_t i = 0; i < options.size(); ++i) {
            question.choices[i] = noteLabel(60 + options[i]);
            if (options[i] == pitchClass) {
                question.correctChoice = static_cast<int>(i);
            }
        }
        question.sounds = {{60 + pitchClass}};
        question.answerDetail = noteLabel(60 + pitchClass);
        return question;
    }

    if (mode == Mode::Scale) {
        std::uniform_int_distribution<int> distribution(0, 2);
        const int scale = distribution(generator);
        const auto options = distinctChoices(scale, 3, generator);
        for (std::size_t i = 0; i < options.size(); ++i) {
            question.choices[i] = scaleNames[options[i]];
            if (options[i] == scale) {
                question.correctChoice = static_cast<int>(i);
            }
        }
        for (const int midi : cScales[scale]) {
            question.sounds.push_back({midi});
            question.answerDetail += (question.answerDetail.empty() ? "" : " ") + noteLabel(midi);
        }
        return question;
    }

    std::uniform_int_distribution<int> distribution(0, 6);
    const int chord = distribution(generator);
    const auto options = distinctChoices(chord, 7, generator);
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto notes = chordNotes(options[i]);
        question.choices[i] = noteLabel(notes[0]) + " " + noteLabel(notes[1]) + " " + noteLabel(notes[2]);
        if (options[i] == chord) {
            question.correctChoice = static_cast<int>(i);
        }
    }
    const auto notes = chordNotes(chord);
    question.sounds = {{notes[0], notes[1], notes[2]}};
    question.answerDetail = question.choices[question.correctChoice];
    return question;
}

std::vector<int16_t> renderQuestionAudio(const std::vector<std::vector<int>>& sounds)
{
    if (sounds.empty()) {
        return {};
    }

    constexpr std::size_t samplesPerFrame = ToneStream::sampleRate * ToneStream::frameDurationMs / 1000;
    const std::size_t frameCount = sounds.size() * ToneStream::framesPerBeat;
    std::vector<int16_t> audio;
    audio.reserve(frameCount * samplesPerFrame);

    ToneStream stream;
    stream.start(sounds);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        auto rendered = stream.render();
        audio.insert(audio.end(), rendered.begin(), rendered.end());
    }

    const std::size_t fadeSamples = std::min(samplesPerFrame, audio.size());
    for (std::size_t i = 0; i < fadeSamples; ++i) {
        const std::size_t index = audio.size() - fadeSamples + i;
        const float gain = static_cast<float>(fadeSamples - i - 1) / static_cast<float>(fadeSamples);
        audio[index] = static_cast<int16_t>(static_cast<float>(audio[index]) * gain);
    }
    return audio;
}

void ToneStream::start(const std::vector<std::vector<int>>& sounds)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _sounds = sounds;
    _phases.fill(0.0f);
    _frameInBeat = 0;
    _soundIndex = 0;
    _active = !_sounds.empty();
}

void ToneStream::stop()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _active = false;
    std::vector<std::vector<int>>().swap(_sounds);
}

bool ToneStream::active() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _active;
}

bool ToneStream::render(std::span<int16_t> output)
{
    std::lock_guard<std::mutex> lock(_mutex);
    constexpr int samplesPerFrame = sampleRate * frameDurationMs / 1000;
    constexpr float amplitude = static_cast<float>(std::numeric_limits<int16_t>::max()) / 2.0f;
    if(output.size()!=samplesPerFrame)return false;
    std::fill(output.begin(),output.end(),0);
    if (!_active || _sounds.empty()) {
        return true;
    }

    const auto& chord = _sounds[_soundIndex];
    const float voiceGain = chord.empty() ? 0.0f : 1.0f / static_cast<float>(chord.size());
    std::array<float, 4> angleSteps = {};
    for (std::size_t voice = 0; voice < chord.size() && voice < angleSteps.size(); ++voice) {
        angleSteps[voice] = 2.0f * static_cast<float>(M_PI) *
                            midiFrequency(chord[voice] + playbackOctaveShift * 12) / sampleRate;
    }
    for (int sample = 0; sample < samplesPerFrame; ++sample) {
        float mixed = 0.0f;
        for (std::size_t voice = 0; voice < chord.size() && voice < _phases.size(); ++voice) {
            mixed += sinf(_phases[voice]) * voiceGain;
            _phases[voice] += angleSteps[voice];
            if (_phases[voice] >= 2.0f * static_cast<float>(M_PI)) {
                _phases[voice] -= 2.0f * static_cast<float>(M_PI);
            }
        }
        output[sample] = static_cast<int16_t>(amplitude * mixed);
    }

    if (++_frameInBeat == framesPerBeat) {
        _frameInBeat = 0;
        _soundIndex = (_soundIndex + 1) % _sounds.size();
    }
    return true;
}

std::vector<int16_t> ToneStream::render() {
    std::vector<int16_t> output(sampleRate*frameDurationMs/1000);
    render(output);return output;
}

}  // namespace ear_game
