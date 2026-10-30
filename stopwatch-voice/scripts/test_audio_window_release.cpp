#include "audio_spectrum.h"
#include <vector>
#include <cassert>
#include <cstdio>
#include <limits>
int main() {
 if(!audio_memory::arena.reserve(audio_spectrum::Workspace::requiredBytes(2048)))return 9;

 std::vector<int16_t> samples(2048);for(size_t i=0;i<samples.size();++i)samples[i]=4000*std::sin(2*3.141592653589793*440*i/16000);
 audio_spectrum::Workspace workspace;assert(workspace.bytes()==0);assert(workspace.begin(samples.size()));
 const auto first=workspace.power(samples);std::vector<float> before(first.begin(),first.end());auto *scratch=workspace.scratchData();
 for(int i=0;i<100;++i){const auto next=workspace.power(samples);assert(std::equal(before.begin(),before.end(),next.begin()));assert(workspace.scratchData()==scratch);}
 workspace.end();assert(workspace.bytes()==0);audio_memory::arena.rewind();assert(workspace.begin(samples.size()));const auto after=workspace.power(samples);assert(std::equal(before.begin(),before.end(),after.begin()));workspace.end();assert(workspace.bytes()==0);
 audio_memory::arena.rewind();audio_memory::Buffer<std::byte> oversized;assert(!oversized.allocate(audio_memory::arena.capacity()+1));assert(audio_memory::arena.used()==0);assert(audio_memory::arena.capacity()==audio_spectrum::Workspace::requiredBytes(2048));
 printf("PASS shared arena retained, views reused/released exactly, overflow request rejected\n");
}
