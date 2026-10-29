#pragma once
#include <algorithm>
#include <cmath>

// Timelines and coordinates ported from character-plan/build2.py.
namespace characters {
enum class Move { rest, jump, play };
struct Pose { double x, y, sx = 1, sy = 1, rotation = 0; int row = 0; };
constexpr double kHomes[6][2]={{110,220},{233,205},{356,220},{150,330},{233,345},{316,330}};
constexpr int kTileSide=144;
constexpr double kSingleScale=(390.0-150.0)/104.0;
inline Pose pose(int index, double now, double since_action, Move move, int mood, double since_mood,bool single=false)
{
    Pose p{kHomes[index][0], kHomes[index][1]};
    if(single) { p.x=233;p.y=233; }
    const double breathe = std::sin(now * 2.2 + p.x);
    p.y += breathe * 3; p.sy += breathe * .015; p.sx -= breathe * .015;
    if (mood == 1) {
        const double b = std::abs(std::sin(since_mood * 7)) * std::exp(-since_mood * 1.2);
        p.y -= b * 18; p.sy += b * .08; p.row = 3;
    } else if (mood == 2) { p.y += 10 * (1 - std::pow(1 - std::min(1.0, since_mood), 3)); p.sy -= .06; p.sx += .04; p.rotation = -.05; p.row = 5; }
    else if (mood == 3) { p.x += std::sin(since_mood * 40) * 5 * std::exp(-since_mood * 1.5); p.sy += .04 * std::exp(-since_mood); p.row = 10; }
    if (move == Move::jump) {
        const double t = since_action - index * .08;
        if (t >= 0 && t < .9) {
            if (t < .12) { p.sy -= .25 * t / .12; p.sx += .2 * t / .12; }
            else if (t < .72) { const double s = (t - .12) / .6; p.y -= std::sin(s * M_PI) * 70; p.sy += .18 * std::cos(s * M_PI); p.sx -= .1 * std::cos(s * M_PI); }
            else { const double s = (t - .72) / .18; p.sy -= .2 * std::sin(s * M_PI); p.sx += .16 * std::sin(s * M_PI); }
            p.row = 4;
        }
    } else if (move == Move::play) {
        const double t = since_action - index * .25;
        const int direction = index % 2 ? 1 : -1;
        const double off_left=single?-kTileSide*kSingleScale/2-1:-100;
        const double off_right=single?466+kTileSide*kSingleScale/2+1:560;
        if (t >= 0 && t < 4.4) {
            if (t < 1.4) { const double s = t / 1.4, a = s < .5 ? 4*s*s*s : 1-std::pow(-2*s+2,3)/2; p.x += ((direction > 0 ? off_right : off_left) - p.x) * a; }
            else if (t < 3) { p.x = -1000; }
            else { const double a = 1 - std::pow(1 - (t - 3) / 1.4, 3); p.x = (direction > 0 ? off_left : off_right) * (1-a) + p.x * a; }
            p.y -= std::abs(std::sin(t * 12)) * 8;
            p.rotation = .08 * (t < 1.4 ? direction : -direction);
            p.row = (t < 1.4 ? direction : -direction) > 0 ? 1 : 2;
        }
    }
    return p;
}
inline double follow(double current, double target, double dt) { return current + (target-current) * (1-std::exp(-dt*10)); }
}
