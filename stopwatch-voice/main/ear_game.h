// SPDX-License-Identifier: MIT
// Copyright (c) 2026 M5Stack Technology CO LTD
// See licenses/M5StopWatch-MIT.txt.
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <span>

namespace ear_game {

enum class Mode { Note, Scale, Chord };

struct Question {
    Mode mode = Mode::Note;
    std::array<std::string, 3> choices;
    int correctChoice = 0;
    std::vector<std::vector<int>> sounds;
    std::string answerDetail;
};

Question makeQuestion(Mode mode, uint32_t seed);
std::vector<int16_t> renderQuestionAudio(const std::vector<std::vector<int>>& sounds);

class ToneStream {
public:
    static constexpr int sampleRate = 44100;
    static constexpr int frameDurationMs = 20;
    static constexpr int framesPerBeat = 25;
    static constexpr int playbackOctaveShift = 3;

    void start(const std::vector<std::vector<int>>& sounds);
    void stop();
    bool active() const;
    bool render(std::span<int16_t> output);
    std::vector<int16_t> render();

private:
    mutable std::mutex _mutex;
    std::vector<std::vector<int>> _sounds;
    std::array<float, 4> _phases{};
    int _frameInBeat = 0;
    std::size_t _soundIndex = 0;
    bool _active = false;
};

}  // namespace ear_game
