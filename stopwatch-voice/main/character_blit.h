#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace characters {
// Pixel centres and 16.16 inverse coordinates follow the pinned LovyanGFX
// LGFXBase::push_image_rotate_zoom / make_invert_affine32 (BSD-licensed).
struct IndexedTransform {
    int32_t inverse[6];
    int left,top,right,bottom;
};
struct SourceRect { int left,top,right,bottom; };
struct PackedBounds { uint8_t left,top,right,bottom; };
inline PackedBounds packed_bounds(const uint8_t *pixels,int width,int height)
{
    int left=width,top=height,right=0,bottom=0;
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        const int position=y*width+x;
        if((pixels[position/2]>>(position%2?0:4))&15) {
            left=std::min(left,x);top=std::min(top,y);right=std::max(right,x+1);bottom=std::max(bottom,y+1);
        }
    }
    return {uint8_t(left),uint8_t(top),uint8_t(right),uint8_t(bottom)};
}
inline IndexedTransform indexed_transform(int side,int width,int height,float x,float y,
                                          float angle,float zoom_x,float zoom_y,SourceRect region)
{
    constexpr float fixed=1<<16;
    const float rad=std::fmod(angle,360.0f)*float(M_PI/180);
    const float c=std::cos(rad),s=std::sin(rad),pivot=side/2.0f+.5f;
    const float a=c*zoom_x,b=-s*zoom_y,d=s*zoom_x,e=c*zoom_y;
    const float tx=x+.5f-pivot*a-pivot*b,ty=y+.5f-pivot*d-pivot*e;
    const float determinant=fixed/(a*e-b*d);
    IndexedTransform t{{int32_t(std::round(determinant*e)),int32_t(std::round(-determinant*b)),
        int32_t(std::round(determinant*(b*ty-tx*e))),int32_t(std::round(-determinant*d)),
        int32_t(std::round(determinant*a)),int32_t(std::round(determinant*(tx*d-a*ty)))},0,0,0,0};
    const float xs[]={tx+a*region.left+b*region.top,tx+a*region.right+b*region.top,
        tx+a*region.left+b*region.bottom,tx+a*region.right+b*region.bottom};
    const float ys[]={ty+d*region.left+e*region.top,ty+d*region.right+e*region.top,
        ty+d*region.left+e*region.bottom,ty+d*region.right+e*region.bottom};
    t.left=std::max(0,int(std::floor(*std::min_element(xs,xs+4)))-1);
    t.right=std::min(width,int(std::ceil(*std::max_element(xs,xs+4)))+1);
    t.top=std::max(0,int(std::floor(*std::min_element(ys,ys+4)))-1);
    t.bottom=std::min(height,int(std::ceil(*std::max_element(ys,ys+4)))+1);
    return t;
}
template<class Paint>
inline void blit_samples(uint16_t *destination,int width,const IndexedTransform &t,Paint paint)
{
    const auto *m=t.inverse;
    const int32_t half_x=(m[0]+m[1])>>1,half_y=(m[3]+m[4])>>1;
    for(int y=t.top;y<t.bottom;++y) {
        int32_t sx=m[2]+m[0]*t.left+m[1]*y+half_x;
        int32_t sy=m[5]+m[3]*t.left+m[4]*y+half_y;
        auto *out=destination+y*width;
        for(int x=t.left;x<t.right;++x,sx+=m[0],sy+=m[3]) {
            const unsigned ix=uint32_t(sx)>>16,iy=uint32_t(sy)>>16;
            paint(out[x],ix,iy);
        }
    }
}
inline void blit_indexed(const uint8_t *source,int side,const uint16_t *palette,
                         uint16_t *destination,int width,const IndexedTransform &t)
{
    blit_samples(destination,width,t,[&](uint16_t &pixel,unsigned x,unsigned y) {
        if(x<unsigned(side) && y<unsigned(side)) {
            const unsigned index=source[y*side+x];
            if(index) pixel=palette[index];
        }
    });
}
inline void blit_packed(const uint8_t *source,int source_width,int source_height,int origin_x,int origin_y,
                        const uint16_t *palette,uint16_t *destination,int width,const IndexedTransform &t)
{
    blit_samples(destination,width,t,[&](uint16_t &pixel,unsigned x,unsigned y) {
        x-=origin_x;y-=origin_y;
        if(x<unsigned(source_width) && y<unsigned(source_height)) {
            const unsigned position=y*source_width+x;
            const unsigned index=(source[position/2]>>(position%2?0:4))&15;
            if(index) pixel=palette[index];
        }
    });
}
}
