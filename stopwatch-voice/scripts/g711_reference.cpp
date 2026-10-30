#include "radio/transport/g711.h"
#include <array>
#include <cstdio>
int main(){
 std::array<int16_t,320> input{};if(std::fread(input.data(),sizeof(int16_t),input.size(),stdin)!=input.size()){std::fprintf(stderr,"expected one 16kHz/20ms frame\n");return 1;}
 auto encoded=transceiver::g711::encodeFrame<16000>(input);std::array<int16_t,882> output{};
 transceiver::g711::decodeFrameInto<44100,882>(encoded,output.data());
 return std::fwrite(output.data(),sizeof(int16_t),output.size(),stdout)==output.size()?0:1;
}
