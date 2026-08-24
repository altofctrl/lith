// Stub SDL for the lith screen simulator.
//
// LovyanGFX picks its PC platform layer by __has_include(<SDL2/SDL.h>), and
// then guards the whole useful half of sdl/common.hpp behind SDL_h_. We want
// that half — heap_alloc, FileWrapper, the platform typedefs — but no window,
// no event loop and no SDL dependency, because nothing here draws to a screen;
// the sprite buffer is read straight out of memory and written as a PNG.
//
// So: define SDL_h_, and supply the four clock/sleep calls sdl/common.cpp
// makes. Real time is irrelevant to the simulator, which renders at a virtual
// millis() the harness sets, so these are allowed to be trivial.
#pragma once

#include <stdint.h>

#define SDL_h_

static inline uint32_t SDL_GetTicks(void) { return 0; }
static inline uint64_t SDL_GetPerformanceCounter(void) { return 0; }
static inline uint64_t SDL_GetPerformanceFrequency(void) { return 1000000; }
static inline void     SDL_Delay(uint32_t) {}
