#include "pitch_detector.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
int main(){
 if(!audio_memory::arena.reserve(audio_spectrum::Workspace::requiredBytes(tuner::PitchDetector::frameSize)))return 9;

 tuner::Tracker tracker;assert(tracker.begin());std::vector<int16_t> window;double phase=0;int samples=0;
 auto feed=[&](size_t count,bool sweep,bool silence=false){
  for(size_t i=0;i<count;++i,++samples){double cents=sweep?(samples-3072)*2.*tuner::PitchDetector::stableCents/tuner::PitchDetector::hopSize:0;phase+=2*M_PI*440*std::pow(2.,cents/1200)/16000;window.push_back(silence?0:std::lround(6000*std::sin(phase)));}
  if(window.size()>tuner::PitchDetector::frameSize)window.erase(window.begin(),window.end()-tuner::PitchDetector::frameSize);
  return tracker.update(window,440);
 };
 feed(2048,false);feed(512,false);assert(feed(512,false).valid);
 bool expired=false;for(int hop=0;hop<32;++hop){auto shown=feed(512,true);assert(tracker.latestAnalysis().valid);if(tracker.displayAgeSamples()>=tuner::PitchDetector::holdSamples){assert(!shown.valid);expired=true;}}
 assert(expired);bool restored=false;for(int i=0;i<7;++i){auto shown=feed(512,false);restored|=shown.valid&&shown.midi==69;}assert(restored);
 for(int i=0;i<4;++i)feed(512,false);
 bool cleared=false;for(int i=0;i<20;++i){auto shown=feed(512,false,true);if(!shown.valid)cleared=true;}assert(cleared);
 printf("PASS valid swept PCM expires at display age 0.5s, steady tone restores, silence still clears\n");
}
