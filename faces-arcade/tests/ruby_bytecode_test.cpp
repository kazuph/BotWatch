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
    auto nested = [](size_t count) {
        std::vector<uint8_t> bytes(20 + 12 + count * 20 + 8, 0);
        const char* header = "RITE0400";
        for (int i=0;i<8;++i) bytes[i]=header[i];
        u32(bytes,8,bytes.size());
        bytes[20]='I'; bytes[21]='R'; bytes[22]='E'; bytes[23]='P';
        u32(bytes,24,12 + count * 20);
        for (size_t i=0;i<count;++i) {
            size_t at=32+i*20; u32(bytes,at,20);
            bytes[at+7]=1; // one register
            bytes[at+9]=i+1<count ? 1 : 0; // one child except at the leaf
        }
        size_t end=32+count*20; bytes[end]='E'; bytes[end+1]='N'; bytes[end+2]='D';
        u32(bytes,end+4,8);
        return bytes;
    };
    auto maxDepth=nested(64); assert(rubyValidBytecode(maxDepth.data(),maxDepth.size()));
    test(nested(65),"nested IREP depth");
    puts("valid catch and all truncated prefixes checked");
}
