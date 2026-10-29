// Compare direct writes with the pinned library's real palette conversion.
// Include main and managed_components/m5stack__m5gfx/src when compiling.
#include "character_blit.h"
#include "character_motion.h"
#include <lgfx/v1/misc/pixelcopy.hpp>
#include <cassert>
#include <cstdio>
#include <vector>
#include <fstream>
#include <iterator>

int main()
{
    constexpr int width=468,height=468;
    const float cases[][5]={{233,233,0,.78f,.79f},{233,233,-2.8f,.9f,.7f},
        {-120,233,4.6f,.78f,.78f},{600,233,-4.6f,.78f,.78f},
        {233,-120,45,1,1},{233,600,-45,1,1},{-700,-700,0,1,1},
        {700,700,0,1,1},{110,220,0,1.015f,.985f},{356,220,2.3f,1,1},
        {233,233,0,float(characters::kSingleScale),float(characters::kSingleScale)},
        {233,160,4.6f,float(characters::kSingleScale*1.2),float(characters::kSingleScale*.75)},
        {-120,233,-4.6f,float(characters::kSingleScale),float(characters::kSingleScale)}};
    lgfx::bgr888_t colors[256];uint16_t packed[256];
    for(int i=0;i<256;++i) {
        colors[i].set(i,i*7,i*13);
        packed[i]=lgfx::swap565_t(colors[i].R8(),colors[i].G8(),colors[i].B8()).raw;
    }
    uint64_t compared=0;
    for(int side:{96,272}) {
        std::vector<uint8_t> source(side*side);
        for(int i=0;i<side*side;++i) source[i]=(i*31+i/side)%256;
        for(const auto &c:cases) {
            const auto transform=characters::indexed_transform(side,width,height,c[0],c[1],c[2],c[3],c[4],{0,0,side,side});
            std::vector<uint16_t> actual(width*height+2,0x1234);
            characters::blit_indexed(source.data(),side,packed,actual.data()+1,width,transform);
            assert(actual.front()==0x1234 && actual.back()==0x1234);
            lgfx::pixelcopy_t pc;
            pc.src_data=source.data();pc.palette=colors;pc.src_bitwidth=side;
            pc.src_bits=8;pc.src_mask=255;pc.transp=0;pc.src_x32_add=pc.src_y32_add=0;
            const auto *m=transform.inverse;
            for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
                uint16_t expected=0x1234;
                const int64_t sx=int64_t(m[2])+int64_t(m[0])*x+int64_t(m[1])*y+((m[0]+m[1])>>1);
                const int64_t sy=int64_t(m[5])+int64_t(m[3])*x+int64_t(m[4])*y+((m[3]+m[4])>>1);
                if((sx>>16)>=0 && (sx>>16)<side && (sy>>16)>=0 && (sy>>16)<side) {
                    pc.src_x32=sx;pc.src_y32=sy;
                    lgfx::pixelcopy_t::copy_palette_affine<lgfx::swap565_t,lgfx::bgr888_t>(&expected,0,1,&pc);
                }
                assert(actual[1+y*width+x]==expected);++compared;
            }
        }
    }
    std::ifstream file("stopwatch-voice/assets/pets/sprites.bin",std::ios::binary);
    std::vector<uint8_t> atlas((std::istreambuf_iterator<char>(file)),{});
    assert(atlas.size()==1557616);
    uint64_t pet_compared=0;
    const int counts[]={6,8,8,4,5,8,6,6,6,8,8,8,8};
    for(int id=0;id<3;++id) for(int row=0;row<13;++row) for(int frame=0;frame<counts[row];++frame) {
        uint16_t palette[16];
        for(int i=0;i<16;++i) {
            const int at=16+id*32+i*2;const unsigned value=atlas[at]|(atlas[at+1]<<8);
            palette[i]=__builtin_bswap16(uint16_t(value));assert(i==0 || value!=0xf81f);
        }
        const auto *source=atlas.data()+112+id*13*8*96*104/2+(row*8+frame)*96*104/2;
        const auto bounds=characters::packed_bounds(source,96,104);
        for(const auto &c:cases) {
            const auto t=characters::indexed_transform(144,width,height,c[0],c[1],c[2],c[3],c[4],
                {24+bounds.left,10+bounds.top,24+bounds.right,10+bounds.bottom});
            std::vector<uint16_t> actual(width*height+2,0x1234);
            characters::blit_packed(source,96,104,24,10,palette,actual.data()+1,width,t);
            assert(actual.front()==0x1234 && actual.back()==0x1234);
            const auto *m=t.inverse;
            for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
                const int64_t sx=int64_t(m[2])+int64_t(m[0])*x+int64_t(m[1])*y+((m[0]+m[1])>>1);
                const int64_t sy=int64_t(m[5])+int64_t(m[3])*x+int64_t(m[4])*y+((m[3]+m[4])>>1);
                const int px=(sx>>16)-24,py=(sy>>16)-10;
                uint16_t expected=0x1234;
                if(px>=0 && px<96 && py>=0 && py<104) {
                    const int position=py*96+px,index=(source[position/2]>>(position%2?0:4))&15;
                    if(index) expected=palette[index];
                }
                assert(actual[1+y*width+x]==expected);++pet_compared;
            }
        }
    }
    std::printf("PASS indexed_blit_pixels=%llu packed_pet_pixels=%llu offscreen_guards=1 transparency=1 library_palette=1\n",(unsigned long long)compared,(unsigned long long)pet_compared);
}
