#include "ruby_bytecode.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
static void u32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at]=x>>24; v[at+1]=x>>16; v[at+2]=x>>8; v[at+3]=x;
}
int main(int argc,char** argv) {
    assert(argc == 2);
    std::ifstream in(argv[1],std::ios::binary);
    std::vector<uint8_t> good((std::istreambuf_iterator<char>(in)),{});
    assert(rubyValidBytecode(good.data(),good.size()));
    auto test = [&](std::vector<uint8_t> v, const char* name) {
        assert(!rubyValidBytecode(v.data(),v.size())); printf("rejected %s\n",name);
    };
    auto v=good; v.resize(28); u32(v,8,v.size()); v[20]='J';v[21]='U';v[22]='N';v[23]='K';u32(v,24,0);test(v,"zero section");
    v=good;v.resize(20);u32(v,8,v.size());test(v,"header only");
    v=good;u32(v,24,0xffffffff);test(v,"oversized section");
    v=good;v.resize(44);u32(v,8,v.size());u32(v,24,24);test(v,"truncated IREP");
    for (size_t n=0;n<good.size();++n) {
        v.assign(good.begin(),good.begin()+n);
        if (n>=12) u32(v,8,n);
        assert(!rubyValidBytecode(v.data(),v.size()));
    }
    puts("valid catch and all truncated prefixes checked");
}
