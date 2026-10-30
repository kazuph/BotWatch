#include "pitch_detector.h"
#include <vector>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
int main(){
 if(!audio_memory::arena.reserve(audio_spectrum::Workspace::requiredBytes(tuner::PitchDetector::frameSize)))return 9;

 tuner::PitchDetector d;assert(d.begin());int failed=0;std::mt19937 rng(42);std::uniform_int_distribution<int> noise(-10000,10000);
 for(double reference:{440.,442.}) for(double f:{440.,442.,261.625565,184.997211,932.327523,440*std::pow(2.,20/1200.),440*std::pow(2.,-30/1200.)}) for(int harmonics:{0,1}){
  std::vector<int16_t> s(d.frameSize);for(size_t i=0;i<s.size();++i){double phase=2*M_PI*f*i/d.sampleRate;s[i]=1000+5000*std::sin(phase)+(harmonics?7000*std::sin(2*phase)+3000*std::sin(3*phase):0);}
  auto p=d.analyze(s,reference);double err=1200*std::log2(p.frequencyHz/f);int midi=std::lround(69+12*std::log2(f/reference));bool ok=p.valid && std::abs(err)<=2 && p.midi==midi && p.octave==midi/12-1;failed+=!ok;printf("ref=%.0f f=%.6f harmonic=%d measured=%.6f err=%.3f midi=%d octave=%d pass=%d\n",reference,f,harmonics,p.frequencyHz,err,p.midi,p.octave,ok);
 }
 for(int kind=0;kind<3;++kind)for(int trial=0;trial<100;++trial){std::vector<int16_t>s(d.frameSize);for(auto &v:s)v=kind==0?0:kind==1?1000:noise(rng);auto p=d.analyze(s,440);failed+=p.valid;}
 printf("invalid-input silence/DC/noise cases=300 failed_total=%d\n",failed);return failed?1:0;
}
