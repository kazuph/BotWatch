#include "character_touch.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
using namespace characters;
int main()
{
    using K=TouchKind;
    TouchGesture t;
    assert(t.update(true,233,233,0).kind==K::none);
    assert(t.update(false,233,233,40).kind==K::tap);
    assert(t.update(true,233,233,80).kind==K::none);
    assert(t.update(false,233,233,120).kind==K::double_tap);
    t.clear();t.update(true,233,233,0);
    assert(t.update(true,233,233,kCharacterModeHoldMs-1).kind==K::none);
    assert(t.update(true,233,233,kCharacterModeHoldMs).kind==K::mode_hold);
    assert(t.update(true,233,233,kCharacterModeHoldMs+1).kind==K::none);
    assert(t.update(false,233,233,kCharacterModeHoldMs+2).kind==K::none);
    for(int direction : {-1,1}) {
        t.clear();t.update(true,233,233,0);t.update(true,233+direction*100,233,100);
        assert(t.update(false,233+direction*100,233,200).kind==(direction<0?K::swipe_left:K::swipe_right));
        assert(t.update(false,233+direction*100,233,300).kind==K::none);
    }
    t.clear();t.update(true,233,233,0);t.update(true,233,333,100);
    assert(t.update(false,233,333,200).kind==K::none);
    // Crossing the flick boundary cancels long hold and cannot later become a tap.
    t.clear();t.update(true,233,233,0);t.update(true,233+kTouchMovePixels+1,233,10);
    assert(t.update(true,233,233,kCharacterModeHoldMs).kind==K::none);
    assert(t.update(false,233,233,kCharacterModeHoldMs+1).kind==K::none);
    t.clear();t.update(true,233,233,0);
    assert(t.update(false,233,233,kTouchClickMs).kind==K::none);
    // A long second press must not be interpreted as a double tap.
    t.clear();t.update(true,233,233,0);t.update(false,233,233,30);t.update(true,233,233,60);
    assert(t.update(true,233,233,60+kCharacterModeHoldMs).kind==K::mode_hold);
    assert(t.update(false,233,233,61+kCharacterModeHoldMs).kind==K::none);
    // Millisecond rollover preserves the 1500ms contract.
    t.clear();uint32_t base=UINT32_MAX-100;t.update(true,233,233,base);
    assert(t.update(true,233,233,base+kCharacterModeHoldMs).kind==K::mode_hold);
    for(int count=3;count<=6;++count) {
        View cycle;
        for(int i=1;i<=count;++i) { cycle.swipe(1,count);assert(cycle.selected==i%count); }
        cycle.swipe(-1,count);assert(cycle.selected==count-1);
        cycle.gather();assert(cycle.group);cycle.select(0);assert(!cycle.group);
    }
    View view;
    t.clear();t.update(true,356,220,0);
    assert(t.update(false,356,220,40).kind==K::tap);
    t.clear(); // a group tap changed the view; do not carry it into the single view
    t.update(true,356,220,80);
    assert(t.update(false,356,220,120).kind==K::tap);
    assert(!view.group && view.selected==0);
    for(int i=1;i<=6;++i) { view.swipe(1,6);assert(view.selected==i%6 && !view.group); }
    view.swipe(-1,6);assert(view.selected==5);
    view.gather();assert(view.group);
    view.select(2);assert(!view.group && view.selected==2);
    std::puts("PASS tap double_tap swipe_left swipe_right vertical_drag hold_boundary hold_after_drag rollover six_character_cycle group_to_single");
}
