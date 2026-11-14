// Host stub for hardware/display.h: records backlight changes only.
#pragma once

#include <cstdint>

struct FakeTft {
  int brightness = -1;     // last value passed to setBrightness, -1 = never
  int brightness_calls = 0;
  void setBrightness(uint8_t b) {
    brightness = b;
    ++brightness_calls;
  }
};

extern FakeTft tft;
