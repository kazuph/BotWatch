#include "pitch_detector.h"
#include <vector>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
int main(){
 if(!audio_memory::arena.reserve(audio_spectrum::Workspace::requiredBytes(tuner::PitchDetector::frameSize)))return 9;
tuner::PitchDetector d;assert(d.begin());int failed=0;double worst=0;std::mt19937 rng(42);
for(float ref:{410.f,440.f,442.f,480.f})for(int midi:{33,36,54,60,69,82,108})for(int type=0;type<5;++type){
 double f=tuner::midiFrequency(midi,ref);std::vector<int16_t>s(d.frameSize);
 for(size_t i=0;i<s.size();++i){double phase=2*M_PI*f*i/d.sampleRate,decay=type==3?std::exp(-double(i)/d.frameSize):1;s[i]=1000+decay*((type==2?300.:4000.)*std::sin(phase)+(type==1||type==2?9000*std::sin(2*phase)+3000*std::sin(3*phase):0));if(type==4)s[i]=std::sin(phase);}
 auto p=d.analyze(s,ref);double err=p.valid?1200*std::log2(p.frequencyHz/f):0;bool ok=type==4?!p.valid:p.valid&&p.midi==midi&&std::abs(err)<=2;failed+=!ok;if(p.valid)worst=std::max(worst,std::abs(err));printf("ref=%.0f midi=%d type=%d valid=%d got=%d cents_error=%.5f pass=%d\n",ref,midi,type,p.valid,p.midi,err,ok);
}
d.end();audio_memory::arena.rewind();tuner::Tracker t;assert(t.begin());std::vector<int16_t>s(d.frameSize);for(size_t i=0;i<s.size();++i)s[i]=4000*std::sin(2*M_PI*440*i/d.sampleRate);
for(int i=1;i<=3;++i){auto p=t.update(s,440);failed+=p.valid!=(i==3);printf("stable_frame=%d valid=%d\n",i,p.valid);}
std::fill(s.begin(),s.end(),0);for(int i=1;i<=16;++i){auto p=t.update(s,440);failed+=p.valid!=(i<16);}
printf("worst_cents=%.6f failed=%d\n",worst,failed);return failed?1:0;}
