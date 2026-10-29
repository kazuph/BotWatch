#pragma once
#include <M5Unified.h>
#include "voice_screen.h"
#include "character_touch.h"
namespace characters {
// Allocate/map once, before listening starts. Failure is explicit; no stand-in sprites.
bool begin();
int count();
struct DrawTiming { int64_t background_us, character_us; };
DrawTiming draw(M5Canvas &canvas, const voice_screen::Avatar &avatar,const View &view,bool reset_background);
int hit(int x,int y);
void stats();
}
