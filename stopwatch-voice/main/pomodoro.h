/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

namespace model {

class Pomodoro {
public:
    enum class Phase : uint8_t { Focus, Break };
    enum class State : uint8_t { Stopped, Running, Paused };

    static constexpr uint32_t focusDurationSeconds = 25 * 60;
    static constexpr uint32_t breakDurationSeconds = 5 * 60;

    void toggle(uint32_t nowMs);
    void reset();
    bool update(uint32_t nowMs);

    Phase phase() const
    {
        return _phase;
    }
    State state() const
    {
        return _state;
    }
    uint32_t remainingSeconds() const;

private:
    static constexpr uint32_t millisecondsPerSecond = 1000;

    Phase _phase = Phase::Focus;
    State _state = State::Stopped;
    uint32_t _remaining_ms = focusDurationSeconds * millisecondsPerSecond;
    uint32_t _last_update_ms = 0;

    uint32_t phaseDurationMs() const;
    void advancePhase();
};

}  // namespace model
