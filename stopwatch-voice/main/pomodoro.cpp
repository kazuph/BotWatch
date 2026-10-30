/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "pomodoro.h"

using namespace model;

void Pomodoro::toggle(uint32_t nowMs)
{
    if (_state == State::Running) {
        update(nowMs);
        _state = State::Paused;
        return;
    }

    _last_update_ms = nowMs;
    _state = State::Running;
}

void Pomodoro::reset()
{
    _phase = Phase::Focus;
    _state = State::Stopped;
    _remaining_ms = focusDurationSeconds * millisecondsPerSecond;
    _last_update_ms = 0;
}

bool Pomodoro::update(uint32_t nowMs)
{
    if (_state != State::Running) {
        return false;
    }

    uint32_t elapsed = nowMs - _last_update_ms;
    _last_update_ms = nowMs;
    bool phase_changed = false;

    while (elapsed >= _remaining_ms) {
        elapsed -= _remaining_ms;
        advancePhase();
        phase_changed = true;
    }

    _remaining_ms -= elapsed;
    return phase_changed;
}

uint32_t Pomodoro::remainingSeconds() const
{
    return (_remaining_ms + millisecondsPerSecond - 1) / millisecondsPerSecond;
}

uint32_t Pomodoro::phaseDurationMs() const
{
    const uint32_t seconds = _phase == Phase::Focus ? focusDurationSeconds : breakDurationSeconds;
    return seconds * millisecondsPerSecond;
}

void Pomodoro::advancePhase()
{
    _phase = _phase == Phase::Focus ? Phase::Break : Phase::Focus;
    _remaining_ms = phaseDurationMs();
}
