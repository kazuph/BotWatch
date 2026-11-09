#pragma once

// Called by the menu on the main task; it does not draw or execute Ruby.
bool rubyMenuBegin();
void rubyMenuTick(bool mayStart);
bool rubyMenuProgramReady();
void rubyMenuStop();
