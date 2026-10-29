#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace characters {
// Felipe's five circles and radial light from the agreed character-plan/build2.py.
constexpr float kCloudLobes[5][3]={{-22,6,24},{0,-8,28},{22,6,24},{-12,22,20},{12,22,20}};
inline bool cloud_contains(float x,float y) {
    for(const auto &l:kCloudLobes) if((x-l[0])*(x-l[0])+(y-l[1])*(y-l[1])<=l[2]*l[2]) return true;
    return false;
}
inline uint32_t cloud_color(float x,float y) {
    const float t=std::clamp((std::sqrt((x+16)*(x+16)+(y+20)*(y+20))-6)/64,0.0f,1.0f);
    return (uint32_t(143+(47-143)*t)<<16)|(uint32_t(184+(111-184)*t)<<8)|uint32_t(255+(224-255)*t);
}
}
