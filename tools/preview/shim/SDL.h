// Preview shim for <SDL.h>: LovyanGFX's desktop platform only needs these four
// timing calls, plus a few type names, for an offscreen sprite. No window is ever created, so the real
// SDL library is not required.
#pragma once
#define SDL_h_

#include <chrono>
#include <cstdint>
#include <thread>

using Uint32 = uint32_t;
using Uint64 = uint64_t;

// Opaque types named by Panel_sdl.hpp's declarations (its .inl is never built).
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_mutex;
using SDL_Keymod = int;
using SDL_KeyCode = int;
constexpr SDL_KeyCode SDLK_UNKNOWN = 0;

inline Uint64 SDL_GetPerformanceCounter() {
  return static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
}
inline Uint64 SDL_GetPerformanceFrequency() { return 1000000000ull; }
inline Uint32 SDL_GetTicks() {
  return static_cast<Uint32>(SDL_GetPerformanceCounter() / 1000000ull);
}
inline void SDL_Delay(Uint32 ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
