#include "ear_game.h"
#include "audio_spectrum.h"
#include <cmath>
#include <cstdio>
#include <set>
int main(){
 if(!audio_memory::arena.reserve(audio_spectrum::Workspace::requiredBytes(2048)))return 9;
int cases=0;
for(auto mode:{ear_game::Mode::Note,ear_game::Mode::Scale,ear_game::Mode::Chord})for(unsigned seed=0;seed<100;++seed){
 auto q=ear_game::makeQuestion(mode,seed);if(std::set<std::string>(q.choices.begin(),q.choices.end()).size()!=3 || q.correctChoice<0 || q.correctChoice>2)return 1;
 if(mode==ear_game::Mode::Note && (q.sounds.size()!=1 || q.sounds[0].size()!=1))return 2;
 if(mode==ear_game::Mode::Scale && (q.sounds.size()!=8 || std::set<std::string>(q.choices.begin(),q.choices.end())!=std::set<std::string>{"ちょうおんかい","しぜんてきたんおんかい","わせいてきたんおんかい"}))return 3;
 if(mode==ear_game::Mode::Chord && (q.sounds.size()!=1 || q.sounds[0].size()!=3 || q.answerDetail!=q.choices[q.correctChoice]))return 4;
 ear_game::ToneStream stream;stream.start(q.sounds);auto frame=stream.render();
 if(frame.size()!=ear_game::ToneStream::sampleRate*ear_game::ToneStream::frameDurationMs/1000)return 5;
 if(mode==ear_game::Mode::Note){audio_memory::arena.rewind();audio_spectrum::Workspace fft;if(!fft.begin(frame.size()))return 8;auto p=fft.power(frame);auto i=std::max_element(p.begin()+1,p.end())-p.begin();double hz=audio_spectrum::peak(p,i,frame.size(),ear_game::ToneStream::sampleRate).hz;double expected=440*std::pow(2.,(q.sounds[0][0]+12*ear_game::ToneStream::playbackOctaveShift-69)/12.);if(std::abs(1200*std::log2(hz/expected))>2)return 6;}
 stream.stop();auto silent=stream.render();if(std::any_of(silent.begin(),silent.end(),[](auto x){return x!=0;}))return 7;
 ++cases;
}
printf("PASS ear question/choices/audio-frequency/stop cases=%d\n",cases);
}
