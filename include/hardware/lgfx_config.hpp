#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

#include "config.h"

/**
 * LovyanGFX device for the Sunton ESP32-8048S050: 800x480 16-bit parallel RGB
 * panel driven by the ESP32-S3 LCD_CAM peripheral (frame buffer in PSRAM),
 * PWM backlight on GPIO2 and a GT911 capacitive touch controller on I2C.
 * All pin values and timings come from config.h.
 */
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_RGB _bus;
  lgfx::Panel_RGB _panel;
  lgfx::Light_PWM _light;
  lgfx::Touch_GT911 _touch;

 public:
  LGFX() {
    {
      auto cfg = _panel.config();
      cfg.memory_width = config::kDisplayWidth;
      cfg.memory_height = config::kDisplayHeight;
      cfg.panel_width = config::kDisplayWidth;
      cfg.panel_height = config::kDisplayHeight;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel.config(cfg);
    }
    {
      auto cfg = _panel.config_detail();
      cfg.use_psram = 1;  // 800x480x2 = 750 KB frame buffer: PSRAM only
      _panel.config_detail(cfg);
    }
    {
      auto cfg = _bus.config();
      cfg.panel = &_panel;
      for (int i = 0; i < 16; ++i) {
        cfg.pin_data[i] = static_cast<int8_t>(config::kLcdPinData[i]);
      }
      cfg.pin_henable = static_cast<int8_t>(config::kLcdPinDe);
      cfg.pin_vsync = static_cast<int8_t>(config::kLcdPinVsync);
      cfg.pin_hsync = static_cast<int8_t>(config::kLcdPinHsync);
      cfg.pin_pclk = static_cast<int8_t>(config::kLcdPinPclk);
      cfg.freq_write = config::kLcdPclkHz;

      cfg.hsync_polarity = 0;
      cfg.hsync_pulse_width = config::kLcdHsyncPulseWidth;
      cfg.hsync_back_porch = config::kLcdHsyncBackPorch;
      cfg.hsync_front_porch = config::kLcdHsyncFrontPorch;
      cfg.vsync_polarity = 0;
      cfg.vsync_pulse_width = config::kLcdVsyncPulseWidth;
      cfg.vsync_back_porch = config::kLcdVsyncBackPorch;
      cfg.vsync_front_porch = config::kLcdVsyncFrontPorch;
      // Reference driver: flags.pclk_active_neg = true
      cfg.pclk_active_neg = 1;
      cfg.de_idle_high = 0;
      cfg.pclk_idle_high = 0;
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);

    {
      auto cfg = _light.config();
      cfg.pin_bl = static_cast<int16_t>(config::kLcdPinBacklight);
      cfg.freq = 4000;  // reference driver: 4 kHz LEDC
      cfg.pwm_channel = 7;
      _light.config(cfg);
    }
    _panel.light(&_light);

    {
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = config::kDisplayWidth - 1;
      cfg.y_min = 0;
      cfg.y_max = config::kDisplayHeight - 1;
      cfg.pin_int = config::kTouchPinInt;
      cfg.pin_rst = static_cast<int16_t>(config::kTouchPinRst);
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 1;  // I2C_NUM_1
      cfg.pin_sda = static_cast<int16_t>(config::kTouchPinSda);
      cfg.pin_scl = static_cast<int16_t>(config::kTouchPinScl);
      cfg.i2c_addr = config::kTouchI2cAddr;
      cfg.freq = 400000;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};
