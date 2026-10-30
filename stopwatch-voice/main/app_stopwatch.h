// SPDX-License-Identifier: MIT
// Copyright (c) 2026 M5Stack Technology CO LTD
// Adapted from app_stopwatch/model/stopwatch.cpp; time is an explicit input.
#pragma once
#include <cstdint>
#include <vector>
namespace apps {
class Stopwatch {
    uint32_t started=0,accumulated=0;
    bool running_=false;
    std::vector<uint32_t> laps_;
public:
    bool running() const {return running_;}
    void toggle(uint32_t now){if(running_)accumulated+=now-started;else started=now;running_=!running_;}
    uint32_t elapsed(uint32_t now)const{return accumulated+(running_?now-started:0);}
    void left(uint32_t now){if(running_)laps_.push_back(elapsed(now));else{accumulated=0;laps_.clear();}}
    const std::vector<uint32_t>& laps()const{return laps_;}
};
}
