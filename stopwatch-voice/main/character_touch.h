#pragma once
#include <cstdint>
#include <cstdlib>
namespace characters {
// M5Unified 0.2.27 Touch_Class defaults; mode hold retains the existing product duration.
constexpr int kTouchMovePixels=8;
constexpr uint32_t kTouchClickMs=500,kCharacterModeHoldMs=1500;
enum class TouchKind { none,tap,double_tap,swipe_left,swipe_right,mode_hold };
struct TouchEvent { TouchKind kind=TouchKind::none; int x=0,y=0; };
class TouchGesture {
    bool pressed=false,moved=false,consumed=false,last_tap=false,double_candidate=false;
    int start_x=0,start_y=0,tap_x=0,tap_y=0;
    uint32_t start_ms=0,tap_ms=0;
public:
    void clear() { *this=TouchGesture{}; }
    TouchEvent update(bool down,int x,int y,uint32_t ms) {
        TouchEvent event{TouchKind::none,x,y};
        if(down) {
            if(!pressed) {
                pressed=true;moved=consumed=false;start_x=x;start_y=y;start_ms=ms;
                double_candidate=last_tap && ms-tap_ms<=kTouchClickMs
                    && std::abs(x-tap_x)<=((kTouchMovePixels+1)<<2)
                    && std::abs(y-tap_y)<=((kTouchMovePixels+1)<<2);
            }
            moved=moved || std::abs(x-start_x)>kTouchMovePixels || std::abs(y-start_y)>kTouchMovePixels;
            if(!consumed && !moved && ms-start_ms>=kCharacterModeHoldMs) {
                consumed=true;last_tap=false;event.kind=TouchKind::mode_hold;
            }
            return event;
        }
        if(!pressed) return event;
        pressed=false;
        const uint32_t duration=ms-start_ms;
        const int dx=x-start_x,dy=y-start_y;
        if(consumed || duration>=kCharacterModeHoldMs) { last_tap=false;return event; }
        if(moved || std::abs(dx)>kTouchMovePixels || std::abs(dy)>kTouchMovePixels) {
            last_tap=false;
            if(std::abs(dx)>std::abs(dy) && std::abs(dx)>kTouchMovePixels)
                event.kind=dx<0?TouchKind::swipe_left:TouchKind::swipe_right;
            return event;
        }
        if(duration>=kTouchClickMs) { last_tap=false;return event; }
        event.kind=double_candidate?TouchKind::double_tap:TouchKind::tap;
        last_tap=!double_candidate;tap_ms=ms;tap_x=x;tap_y=y;
        return event;
    }
};
struct View {
    bool group=false;
    int selected=0,previous=0,direction=0;
    unsigned revision=0;
    void select(int index,int slide=0) { previous=selected;selected=index;group=false;direction=slide;++revision; }
    void swipe(int direction_,int count) { select((selected+direction_+count)%count,direction_); }
    void gather() { group=true;direction=0;++revision; }
};
}
