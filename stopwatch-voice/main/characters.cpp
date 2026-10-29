#include "characters.h"
#include "character_cloud.h"
#include "character_blit.h"
#include "pet_config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

namespace characters {
namespace {
// 4-bit indexed sprites: 3 pets * 13 rows * 8 frames * 96x104 / 2 < 2MB.
constexpr int kWidth = 96, kHeight = 104, kRows = 13, kColumns = 8;
constexpr int kFrameBytes = kWidth*kHeight/2, kPetBytes = kRows*kColumns*kFrameBytes;
constexpr int kHeaderBytes = 16 + kPetCount*16*2;
constexpr uint16_t kClear = 0xf81f;
M5Canvas body_cache,group_cache[2];
// Larger than every breathing/jump deformation: cached bodies are only reduced, never enlarged.
constexpr double kBodyScale=kSingleScale*(1+.015+.08+.18);
constexpr int kBodySide=4*((int(2*46*kBodyScale)+2+3)/4);
constexpr int kGroupSide=4*((2*46+2+3)/4);
static_assert(2*kGroupSide*kGroupSide<=kBodySide*kBodySide);
int cached_body=-1;
bool cached_dark=true;
const uint8_t *atlas = nullptr;
PackedBounds *pet_bounds=nullptr;
esp_partition_mmap_handle_t mapping;
double gx[6]{}, gy[6]{}, facing[6]{};
double previous = 0, action_time = 0, mood_time = 0;
unsigned movement = 0;
voice_screen::Expression last_mood = voice_screen::Expression::neutral;
uint64_t frames = 0;
int64_t first_frame = 0, draw_us = 0;
unsigned view_revision=0;
double slide_offset=0;
Pose last_pose[6]{};
bool last_group=false;
struct Bounds { int left,top,right,bottom; };
Bounds previous_bounds[6]{};
float star_phase[60];

uint16_t rgb(unsigned c) { return ((c>>19)&31)<<11 | ((c>>10)&63)<<5 | ((c>>3)&31); }
uint16_t read16(const uint8_t *p) { return p[0] | (p[1]<<8); }
void ellipse(M5Canvas &g, double x, double y, double rx, double ry, unsigned c)
{ g.fillEllipse(std::lround(x),std::lround(y),std::lround(rx),std::max(1L,std::lround(ry)),rgb(c)); }

void scene(M5Canvas &g, int kind, double now,bool reset)
{
    if (kind == 0) {
        auto *pixels=static_cast<uint16_t *>(g.getBuffer());
        const int width=g.width(),height=g.height();
        if(reset) std::memset(pixels,0,width*height*sizeof(uint16_t));
        else for(const auto &b:previous_bounds) for(int y=b.top;y<b.bottom;++y)
            std::memset(pixels+y*width+b.left,0,(b.right-b.left)*sizeof(uint16_t));
        for (int i=0;i<60;++i) {
            const int b=std::lround(255*(.25f+.75f*std::abs(std::sin(float(now*1.3)+star_phase[i]))));
            g.fillRect(i*97%466,i*211%466,2,2,rgb((b<<16)|(b<<8)|b));
        }
    } else if (kind == 1) {
        for(int y=0;y<280;++y) {
            const double a=y/300.0;
            const unsigned r=95+96*a, gr=179+51*a, b=255;
            g.drawFastHLine(0,y,466,rgb((r<<16)|(gr<<8)|b));
        }
        for(int i=0;i<3;++i) {
            const int speed[]={14,9,20};
            const double x=std::fmod(now*speed[i]+i*190,620)-80, y=50+i*30;
            ellipse(g,x,y,22,22,0xffffff); ellipse(g,x+22,y-8,26,26,0xffffff); ellipse(g,x+46,y,20,20,0xffffff);
        }
        g.fillRect(0,280,466,186,rgb(0x6dbb4c));
        for(int x=0;x<466;x+=12) { const double s=std::sin(now*2+x*.05)*5; g.drawWideLine(x,300,x+s/2,290,3,rgb(0x3f8f2c)); g.drawWideLine(x+s/2,290,x+s,280,3,rgb(0x3f8f2c)); }
    } else if (kind == 2) {
        g.fillSprite(rgb(0xf2e2c6)); g.fillRect(0,300,466,166,rgb(0xb07d4f));
        g.fillRect(150,50,166,120,rgb(0x9fd4ff)); g.fillRect(150+int(std::fmod(now*30,148)),50,18,120,rgb(0xc1e3ff));
        for(int i=0;i<6;++i) g.drawRect(150-i,50-i,166+2*i,120+2*i,TFT_WHITE);
        for(int side : {-1,1}) {
            const int x=side==1?124:316, k=std::lround(std::sin(now*1.4)*6);
            g.fillTriangle(x,40,x+30*side,40,x+(30+k)*side,190,rgb(0xe06c6c));
            g.fillTriangle(x,40,x+(30+k)*side,190,x+k*side,190,rgb(0xe06c6c));
        }
        ellipse(g,380,200,22,22,0xffffff);
        g.drawWideLine(380,200,380+16*std::sin(now),200-16*std::cos(now),3,rgb(0x333333));
    } else {
        for(int y=0;y<200;++y) { const double a=y/220.0; const unsigned r=255-96*a, gr=207+13*a, b=115+140*a; g.drawFastHLine(0,y,466,rgb((r<<16)|(gr<<8)|b)); }
        ellipse(g,360,70,26+std::sin(now*2)*2,26+std::sin(now*2)*2,0xfff4b0);
        g.fillRect(0,200,466,84,rgb(0x2f9bd6));
        for(int i=0;i<3;++i) for(int x=0;x<466;x+=8) g.drawWideLine(x,225+i*18+std::sin(x*.04+now*2+i)*4,x+8,225+i*18+std::sin((x+8)*.04+now*2+i)*4,4,rgb(0xd5f2ff));
        g.fillRect(0,284,466,182,rgb(0xf1d99b));
        for(int x=0;x<466;++x) { const int y=280+std::lround(std::sin(x*.03+now*1.5)*4); g.drawFastVLine(x,y,284-y,rgb(0xf1d99b)); }
    }
}

struct CorporatePainter {
    M5Canvas &target;
    float sx,sy,c,s,cx,cy;
    CorporatePainter(M5Canvas &target_,float scale,float stretch_x=1,float stretch_y=1,
                     float rotation=0,float center_x=72,float center_y=72):target(target_),
        sx(scale*stretch_x),sy(scale*stretch_y),c(std::cos(rotation)),s(std::sin(rotation)),cx(center_x),cy(center_y) {}
    struct Point { float x,y; };
    Point point(float x,float y) const { x=(x-72)*sx;y=(y-72)*sy;return {cx+c*x-s*y,cy+s*x+c*y}; }
    template<class Sample> void field(float left,float top,float right,float bottom,Sample sample) {
        const Point corners[]={point(72+left,72+top),point(72+right,72+top),point(72+right,72+bottom),point(72+left,72+bottom)};
        float min_x=corners[0].x,max_x=min_x,min_y=corners[0].y,max_y=min_y;
        for(const auto &p:corners) { min_x=std::min(min_x,p.x);max_x=std::max(max_x,p.x);min_y=std::min(min_y,p.y);max_y=std::max(max_y,p.y); }
        const int x0=std::max(0,int(std::floor(min_x))),x1=std::min(int(target.width())-1,int(std::ceil(max_x)));
        const int y0=std::max(0,int(std::floor(min_y))),y1=std::min(int(target.height())-1,int(std::ceil(max_y)));
        const float step_x=c/sx,step_y=-s/sy;
        for(int y=y0;y<=y1;++y) {
            int run=x0;uint16_t previous=kClear;
            const float dx=x0-cx,dy=y-cy;
            float local_x=(c*dx+s*dy)/sx,local_y=(-s*dx+c*dy)/sy;
            for(int x=x0;x<=x1;++x) {
                const uint16_t color=sample(local_x,local_y);
                local_x+=step_x;local_y+=step_y;
                if(x>x0 && color!=previous) { if(previous!=kClear) target.drawFastHLine(run,y,x-run,previous);run=x; }
                previous=color;
            }
            if(x0<=x1 && previous!=kClear) target.drawFastHLine(run,y,x1-run+1,previous);
        }
    }
    void rect(float x,float y,float w,float h,uint16_t color) {
        const auto a=point(x,y),b=point(x+w,y),d=point(x,y+h),e=point(x+w,y+h);
        target.fillTriangle(a.x,a.y,b.x,b.y,e.x,e.y,color);target.fillTriangle(a.x,a.y,e.x,e.y,d.x,d.y,color);
    }
    void line(float x1,float y1,float x2,float y2,float width,uint16_t color) {
        const auto a=point(x1,y1),b=point(x2,y2);
        const float dx=x2-x1,dy=y2-y1,length=std::hypot(dx,dy);
        const float normal=length?std::hypot(dy*sx,dx*sy)/length:std::sqrt(sx*sy);
        target.drawWideLine(a.x,a.y,b.x,b.y,width*normal,color);
    }
    void ellipse(float x,float y,float rx,float ry,unsigned color) {
        const uint16_t ink=rgb(color);
        field(x-72-rx,y-72-ry,x-72+rx,y-72+ry,[&](float px,float py) {
            const float dx=(px-(x-72))/rx,dy=(py-(y-72))/ry;
            return dx*dx+dy*dy<=1?ink:kClear;
        });
    }
};

void clawd(CorporatePainter &paint,double x, double y, double back, double open, int mood, bool running, double now)
{
    constexpr int P=7,Q=14;
    const int cells[3][9]={{0,5,14,15,15,15,13,10,0},{4,13,15,15,15,15,15,14,8},{0,0,8,8,0,4,4,0,0}};
    const uint16_t color=rgb(mood==3?0xe8603a:0xd97757);
    for(int r=0;r<3;++r) for(int c=0;c<9;++c) for(int q=0;q<4;++q) if(cells[r][c]&(8>>q)) {
        int py=30+(r*2+(q>>1))*Q;
        if(r==2 && running && int(now*10)%2 && (c+q)%2) py-=P;
        paint.rect(9+(c*2+(q&1))*P,py,P,Q,color);
    }
    // Fill the original missing quadrants when showing the back.
    for(int cx : {5,12}) {
        const int ex=9+cx*P;
        paint.rect(ex,44,P,Q,color);
        if(back<.5) {
            const int h=mood==1?3:std::max(1,int(Q*open*(mood==2?.7:1)));
            paint.rect(ex+std::lround(x*2)*P,44+std::lround(y)*P+Q-h,P,h,rgb(0x111111));
            if(mood==3) paint.line(ex-2,40+(cx<8?0:4),ex+P+2,40+(cx<8?4:0),3,rgb(0x111111));
        }
    }
}

void grok(CorporatePainter &paint,double x, double y, double back, double open, int mood, bool dark)
{
    if(back>.98) return;
    const uint16_t eye=dark?TFT_BLACK:TFT_WHITE;
    for(int side : {-1,1}) {
        const double ex=74+x*12+side*10, ey=62+y*10;
        double tilt=mood==3?side*.55:mood==2?-side*.45:-.35;
        double h=15*open*(mood==2?.8:1)*(1-back);
        if(mood==1) {
            paint.line(ex-5,ey+2,ex,ey-2,4,eye); paint.line(ex,ey-2,ex+5,ey+2,4,eye);
        } else paint.line(ex-std::sin(tilt)*h/2,ey-std::cos(tilt)*h/2,ex+std::sin(tilt)*h/2,ey+std::cos(tilt)*h/2,6.5,eye);
    }
}

void felipe(CorporatePainter &paint,double x, double y, double back, double open, int mood)
{
    paint.ellipse(68+x*3,38,26,9,0x1b1b1b); paint.rect(66+x*3,26,4,6,rgb(0x1b1b1b));
    if(back>=.5) return;
    for(int side : {-1,1}) {
        const double ex=72+side*10+x*8,ey=74+y*6+(mood==2?6:0);
        if(mood==1) { paint.line(ex-4,ey+2,ex,ey-2,2,rgb(0x0b1a3a)); paint.line(ex,ey-2,ex+4,ey+2,2,rgb(0x0b1a3a)); }
        else paint.ellipse(ex,ey,4,5*open+.5,0x0b1a3a);
        if(mood==3) paint.line(ex-5,ey-8-side*2,ex+5,ey-8+side*2,2,rgb(0x0b1a3a));
    }
}

void body_palette(M5Canvas &cache,int kind,bool dark)
{
    cache.setPaletteColor(0,uint32_t(0xff00ff));
    for(unsigned index=1;index<256;++index) {
        const float t=(index-1)/254.0f;
        if(kind==0) {
            const uint8_t b=dark?207+48*(1-t):58*(1-t);
            cache.setPaletteColor(index,b,b,b);
        } else cache.setPaletteColor(index,uint8_t(143+(47-143)*t),uint8_t(184+(111-184)*t),uint8_t(255+(224-255)*t));
    }
}
void fill_body(M5Canvas &cache,int kind,float scale)
{
        const int side=cache.width();
        auto *pixels=static_cast<uint8_t *>(cache.getBuffer());
        for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
            const float px=(x-side/2.0f)/scale,py=(y-side/2.0f)/scale;
            unsigned index=0;
            if(kind==0) {
                const float radius=std::sqrt(px*px+py*py);
                if(radius<=42) index=1+std::min(254L,std::lround(radius*254/42));
            } else if(cloud_contains(px,py)) {
                const float t=std::clamp((std::sqrt((px+16)*(px+16)+(py+20)*(py+20))-6)/64,0.0f,1.0f);
                index=1+std::min(254L,std::lround(t*254));
            }
            pixels[y*side+x]=index;
        }
}
bool prepare_bodies()
{
    body_cache.setPsram(true);body_cache.setColorDepth(lgfx::color_depth_t::palette_8bit);
    if(!body_cache.createSprite(kBodySide,kBodySide)) return false;
    body_cache.setPivot(kBodySide/2.0,kBodySide/2.0);
    // Group views borrow disjoint slices of the single-view storage. Layout
    // changes repaint in place without allocations while listening.
    auto *pixels=static_cast<uint8_t *>(body_cache.getBuffer());
    for(int kind=0;kind<2;++kind) {
        auto &cache=group_cache[kind];cache.setColorDepth(lgfx::color_depth_t::palette_8bit);
        if(!cache.createPalette()) return false;
        cache.setBuffer(pixels+kind*kGroupSide*kGroupSide,kGroupSide,kGroupSide);
        cache.setPivot(kGroupSide/2.0,kGroupSide/2.0);
        body_palette(cache,kind,true);
    }
    printf("CHARACTER_CACHE body_side=%d body_bytes=%d scale=%.3f shared=1\n",kBodySide,kBodySide*kBodySide,kBodyScale);
    return true;
}
M5Canvas &body_for(int kind,bool group,bool dark)
{
    if(group) {
        if(cached_body!=2) {
            for(int i=0;i<2;++i) fill_body(group_cache[i],i,1);
            body_palette(group_cache[0],0,dark);cached_body=2;
        } else if(cached_dark!=dark) body_palette(group_cache[0],0,dark);
        cached_dark=dark;
        return group_cache[kind];
    }
    if(cached_body!=kind) { fill_body(body_cache,kind,kBodyScale);body_palette(body_cache,kind,dark);cached_body=kind; }
    else if(kind==0 && cached_dark!=dark) body_palette(body_cache,kind,dark);
    cached_dark=dark;
    return body_cache;
}

SourceRect pet(M5Canvas &canvas,int id,int row,double now,const Pose &p,double scale,double rotation)
{
    constexpr int counts[13]={6,8,8,4,5,8,6,6,6,8,8,8,8};
    const int frame=int(now*8)%counts[row];
    const uint8_t *palette=atlas+16+id*32;
    const uint8_t *pixels=atlas+kHeaderBytes+id*kPetBytes+(row*kColumns+frame)*kFrameBytes;
    uint16_t colors[16];
    for(int i=0;i<16;++i) colors[i]=__builtin_bswap16(read16(palette+i*2));
    const auto b=pet_bounds[(id*kRows+row)*kColumns+frame];
    const SourceRect region{24+b.left,10+b.top,24+b.right,10+b.bottom};
    const auto transform=indexed_transform(kTileSide,canvas.width(),canvas.height(),p.x,p.y,
        rotation,p.sx*scale,p.sy*scale,region);
    blit_packed(pixels,kWidth,kHeight,24,10,colors,static_cast<uint16_t *>(canvas.getBuffer()),canvas.width(),transform);
    return region;
}
}

int count() { return 3+kPetCount; }

bool begin()
{
    for(int i=0;i<60;++i) star_phase[i]=std::fmod(i*.37,1)*9;
    if(kPetCount) {
        const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"sprites");
        if(!p || p->address!=0xe00000 || p->size!=0x200000) { printf("ERROR sprites_partition\n"); return false; }
        const void *mapped=nullptr;
        if(esp_partition_mmap(p,0,kHeaderBytes+kPetCount*kPetBytes,ESP_PARTITION_MMAP_DATA,&mapped,&mapping)!=ESP_OK) return false;
        atlas=static_cast<const uint8_t *>(mapped);
        if(std::memcmp(atlas,"SWPET001",8) || read16(atlas+8)!=96 || read16(atlas+10)!=104 || read16(atlas+12)!=13 || read16(atlas+14)!=8) { printf("ERROR sprites_format\n"); return false; }
        const size_t bounds_bytes=kPetCount*kRows*kColumns*sizeof(PackedBounds);
        pet_bounds=static_cast<PackedBounds *>(heap_caps_malloc(bounds_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!pet_bounds) return false;
        for(int frame=0;frame<kPetCount*kRows*kColumns;++frame)
            pet_bounds[frame]=packed_bounds(atlas+kHeaderBytes+frame*kFrameBytes,kWidth,kHeight);
        printf("CHARACTER_CACHE pet_bounds_bytes=%u\n",unsigned(bounds_bytes));
    }
    if(!prepare_bodies()) { printf("ERROR character_body_cache\n");return false; }
    printf("CHARACTERS_READY sprites_bytes=%d\n",kHeaderBytes+kPetCount*kPetBytes);
    return true;
}

DrawTiming draw(M5Canvas &canvas, const voice_screen::Avatar &a,const View &view,bool reset_background)
{
    const int64_t start=esp_timer_get_time();
    const double now=start/1e6,dt=previous?std::min(.05,now-previous):0;
    previous=now;
    if(a.movement!=movement) { movement=a.movement; action_time=now; }
    if(a.expression!=last_mood) { last_mood=a.expression; mood_time=now; }
    const int mood=a.expression==voice_screen::Expression::happy?1:a.expression==voice_screen::Expression::sad?2:a.expression==voice_screen::Expression::angry?3:0;
    const int64_t background_start=esp_timer_get_time();
    scene(canvas,a.background,now,reset_background);
    std::fill(std::begin(previous_bounds),std::end(previous_bounds),Bounds{});
    const int64_t character_start=esp_timer_get_time();
    if(view.revision!=view_revision) { view_revision=view.revision;slide_offset=view.direction*466; }
    slide_offset=follow(slide_offset,0,dt);
    last_group=view.group;
    for(int i=0;i<count();++i) { gx[i]=follow(gx[i],a.eye_x,dt); gy[i]=follow(gy[i],a.eye_y,dt); facing[i]=follow(facing[i],a.back?1:0,dt); }
    auto draw_one=[&](int i,bool outgoing) {
        Pose p=pose(i,now,now-action_time,a.move,mood,now-mood_time,!view.group);
        if(!view.group) {
            p.x+=slide_offset-(outgoing?view.direction*466:0);
        }
        p.x+=a.body_x;p.y+=a.body_y;
        last_pose[i]=p;
        if(p.x< -200 || p.x>700) return;
        const double blink=std::fmod(now+i*.37,3.7);
        const double open=a.eyes_closed || a.expression==voice_screen::Expression::sleepy?0:blink<.14?std::abs(blink-.07)/.07:1;
        const double scale=view.group?(i<3?1:.85):kSingleScale;
        const double rotation_rad=p.rotation+(i==2?std::sin(now*1.6)*.04:0);
        const double rotation=rotation_rad*180/M_PI;
        SourceRect pet_region{};
        if(i<3) {
          CorporatePainter paint(canvas,scale,p.sx,p.sy,rotation_rad,p.x,p.y);
        if(i==1 || i==2) {
            const double size=scale;
            auto &body=body_for(i-1,view.group,a.background==0);
            const double cache_scale=view.group?1:kBodyScale;
            uint16_t colors[256];
            const auto *palette=body.getPalette();
            for(int index=0;index<256;++index) colors[index]=lgfx::swap565_t(palette[index].R8(),palette[index].G8(),palette[index].B8()).raw;
            const auto transform=indexed_transform(body.width(),canvas.width(),canvas.height(),p.x,p.y,
                rotation,
                size*p.sx/cache_scale,size*p.sy/cache_scale,{0,0,int(body.width()),int(body.height())});
            blit_indexed(static_cast<const uint8_t *>(body.getBuffer()),body.width(),colors,
                static_cast<uint16_t *>(canvas.getBuffer()),canvas.width(),transform);
        }
        if(i==0) clawd(paint,gx[i],gy[i],facing[i],open,mood,p.row==1||p.row==2,now);
        else if(i==1) grok(paint,gx[i],gy[i],facing[i],open,mood,a.background==0);
        else if(i==2) felipe(paint,gx[i],gy[i],facing[i],open,mood);
        } else {
            int row=p.row;
            if(row!=1 && row!=2 && row!=4) { if(facing[i]>.5) row=9; else if(mood==3) row=10; else if(gy[i]<-.5) row=11; else if(gy[i]>.5) row=12; else if(std::abs(gx[i])>.5) row=gx[i]>0?1:2; }
            pet_region=pet(canvas,i-3,row,now,p,scale,rotation);
        }
        // Bounds of the approved source primitives / packed pet cell within its tile.
        const float left=i==0?-63:i==1?-42:i==2?-46:pet_region.left-kTileSide/2;
        const float right=i<3?-left:pet_region.right-kTileSide/2;
        const float top=i==0?-42:i==1?-42:i==2?-46:pet_region.top-kTileSide/2;
        const float bottom=i<3?42:pet_region.bottom-kTileSide/2;
        const float c=std::cos(float(rotation*M_PI/180)),s=std::sin(float(rotation*M_PI/180));
        float min_x=canvas.width(),max_x=0,min_y=canvas.height(),max_y=0;
        for(float x:{left,right}) for(float y:{top,bottom}) {
            const float sx=x*p.sx*scale,sy=y*p.sy*scale;
            const float px=p.x+c*sx-s*sy,py=p.y+s*sx+c*sy;
            min_x=std::min(min_x,px);max_x=std::max(max_x,px);min_y=std::min(min_y,py);max_y=std::max(max_y,py);
        }
        // Cover both source sampling and destination edge rounding (one pixel each).
        previous_bounds[i]={std::clamp(int(std::floor(min_x))-2,0,int(canvas.width())),std::clamp(int(std::floor(min_y))-2,0,int(canvas.height())),
            std::clamp(int(std::ceil(max_x))+2,0,int(canvas.width())),std::clamp(int(std::ceil(max_y))+2,0,int(canvas.height()))};
    };
    if(view.group) for(int i=0;i<count();++i) draw_one(i,false);
    else {
        if(view.direction && std::abs(slide_offset)>=1) draw_one(view.previous,true);
        draw_one(view.selected,false);
    }
    ++frames; if(!first_frame) first_frame=start;
    const int64_t completed=esp_timer_get_time();
    draw_us+=completed-start;
    return {character_start-background_start,completed-character_start};
}

int hit(int x,int y)
{
    if(!last_group) return -1;
    for(int i=count()-1;i>=0;--i) {
        const Pose &p=last_pose[i];
        const double scale=i<3?1:.85;
        const double rx=(i==0?63:i<3?46:48)*p.sx*scale;
        const double top=(i<3?-46:-62)*p.sy*scale,bottom=42*p.sy*scale;
        if(x>=p.x-rx && x<=p.x+rx && y>=p.y+top && y<=p.y+bottom) return i;
    }
    return -1;
}

void stats()
{
    const int64_t elapsed=first_frame?esp_timer_get_time()-first_frame:0;
    printf("CHARACTER_STAT frames=%llu elapsed_us=%lld fps=%.3f draw_mean_us=%.1f slide_px=%.3f\n",(unsigned long long)frames,(long long)elapsed,elapsed?frames*1e6/elapsed:0,frames?double(draw_us)/frames:0,slide_offset);
}
}
