// LovyanGFX's desktop platform hooks (millis, heap, GPIO no-ops) and the frame
// buffer panel base, without its SDL window panel, which an offscreen preview
// does not need. Mirrors the relevant part of lgfx_v1_platforms.cpp.
#define LGFX_V1_IMPLEMENTATION
#include <SDL.h>
#include <lgfx/v1/panel/Panel_FrameBufferBase.inl>
#include <lgfx/v1/platforms/sdl/common.inl>
