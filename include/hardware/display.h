#pragma once

#include "hardware/lgfx_config.hpp"

extern LGFX tft;

void displayInit();

/** Copy an RGB565 sprite to the panel in strips, pausing between strips so the
 *  RGB DMA isn't starved of PSRAM bandwidth (avoids picture jitter). */
void displayPushSpriteGently(LGFX_Sprite& sprite, int x, int y);

/** fillScreen() in strips with pauses; use for large PSRAM sprites. */
void displayFillGently(lgfx::LovyanGFX& gfx, uint32_t color);
