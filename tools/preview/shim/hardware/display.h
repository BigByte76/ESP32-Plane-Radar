// Preview shim for hardware/display.h: `tft` is an 800x480 RGB565 sprite in
// host memory instead of the RGB panel, so the firmware's own drawing code can
// render on a desktop and the frame can be saved as a PNG.
#pragma once

#include <Arduino.h>  // the real header pulls this in via lgfx_config.hpp
#include <LovyanGFX.hpp>

#include <cstdint>

class LGFX : public lgfx::LGFX_Sprite {
 public:
  bool init();
  void setBrightness(uint8_t) {}
  int getTouch(int32_t*, int32_t*) { return 0; }
};

extern LGFX tft;

void displayInit();
void displayPushSpriteGently(LGFX_Sprite& sprite, int x, int y);
void displayFillGently(lgfx::LovyanGFX& gfx, uint32_t color);
