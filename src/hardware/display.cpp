#include "hardware/display.h"

#include <algorithm>

#include "config.h"
#include "hardware/display_font.h"

LGFX tft;

namespace {
void stripPause() { vTaskDelay(pdMS_TO_TICKS(config::kLcdStripPauseMs)); }
}  // namespace

void displayPushSpriteGently(LGFX_Sprite& sprite, int x, int y) {
  const int w = sprite.width();
  const int h = sprite.height();
  const auto* buf = static_cast<const lgfx::swap565_t*>(sprite.getBuffer());
  if (buf == nullptr || sprite.getColorDepth() != lgfx::color_depth_t::rgb565_2Byte) {
    sprite.pushSprite(&tft, x, y);
    return;
  }
  for (int row = 0; row < h; row += config::kLcdStripRows) {
    const int n = std::min(config::kLcdStripRows, h - row);
    tft.pushImage(x, y + row, w, n, buf + static_cast<size_t>(row) * w);
    stripPause();
  }
}

void displayFillGently(lgfx::LovyanGFX& gfx, uint32_t color) {
  const int w = gfx.width();
  const int h = gfx.height();
  for (int row = 0; row < h; row += config::kLcdStripRows) {
    gfx.fillRect(0, row, w, std::min(config::kLcdStripRows, h - row), color);
    stripPause();
  }
}

void displayInit() {
  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.setTextWrap(false);
  displayFontInit();
}
