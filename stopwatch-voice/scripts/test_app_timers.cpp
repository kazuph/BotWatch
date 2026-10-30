#include "app_stopwatch.h"
#include "pomodoro.h"
#include <cassert>
#include <cstdio>
int main(){apps::Stopwatch sw;assert(sw.elapsed(0)==0);sw.toggle(100);assert(sw.elapsed(1100)==1000);sw.left(1100);assert(sw.laps().size()==1 && sw.laps()[0]==1000);sw.toggle(2100);assert(sw.elapsed(9999)==2000);sw.toggle(10000);assert(sw.elapsed(11000)==3000);sw.toggle(11000);sw.left(12000);assert(sw.elapsed(20000)==0 && sw.laps().empty());
model::Pomodoro p;assert(p.remainingSeconds()==1500);p.toggle(0);assert(!p.update(1499000) && p.remainingSeconds()==1);assert(p.update(1500000) && p.remainingSeconds()==300 && p.phase()==model::Pomodoro::Phase::Break);p.toggle(1501000);assert(p.remainingSeconds()==299);assert(!p.update(1800000));p.toggle(2000000);assert(p.update(2299000) && p.phase()==model::Pomodoro::Phase::Focus && p.remainingSeconds()==1500);p.reset();p.toggle(0xfffffff0);assert(!p.update(0x3d8) && p.remainingSeconds()==1499);printf("PASS stopwatch pause/lap/reentry/reset and pomodoro focus/break/pause/rollover\n");}
