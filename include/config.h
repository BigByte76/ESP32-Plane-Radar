#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
/** Setup AP closes after this long without portal activity; the saved network
 *  is then retried and the AP reopens if that fails (see portalUntilConnected). */
constexpr unsigned long kWifiPortalTimeoutSec = 600;
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-8048S050: GPIO0 "BOOT" key on the back, active LOW) ---
// Note: GPIO0 is a strapping pin — holding it while powering on enters the ROM
// download mode, so press it only after the screen has lit up.
constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

// --- Display: Sunton ESP32-8048S050, 5" 800x480 16-bit parallel RGB panel ---
// Pins and timings from the ESP-IDF reference driver (mr-sven/esp32-8048S050C).
constexpr gpio_num_t kLcdPinHsync = GPIO_NUM_39;
constexpr gpio_num_t kLcdPinVsync = GPIO_NUM_41;
constexpr gpio_num_t kLcdPinDe = GPIO_NUM_40;
constexpr gpio_num_t kLcdPinPclk = GPIO_NUM_42;
constexpr gpio_num_t kLcdPinBacklight = GPIO_NUM_2;
/** RGB565 data lines in bus order: B0..B4, G0..G5, R0..R4. */
constexpr gpio_num_t kLcdPinData[16] = {
    GPIO_NUM_8,  GPIO_NUM_3,  GPIO_NUM_46, GPIO_NUM_9,  GPIO_NUM_1,   // B0-B4
    GPIO_NUM_5,  GPIO_NUM_6,  GPIO_NUM_7,  GPIO_NUM_15, GPIO_NUM_16,  // G0-G4
    GPIO_NUM_4,                                                       // G5
    GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21, GPIO_NUM_14,  // R0-R4
};
/** Pixel clock. 14 MHz is the reference driver's safe value with the frame
 *  buffer in PSRAM. 12 MHz gives a white screen on this panel (too slow). */
constexpr uint32_t kLcdPclkHz = 14000000;
/** PSRAM bandwidth: the panel DMA streams the frame buffer from PSRAM nonstop,
 *  so long full-speed CPU copies starve it and the picture jitters. Bulk
 *  copies/fills are done in strips of this many rows with a short pause
 *  between strips so the DMA can catch up. */
constexpr int kLcdStripRows = 12;
constexpr uint32_t kLcdStripPauseMs = 1;
/** Side panel (nearest-aircraft list) refresh interval; button presses and
 *  range changes redraw it immediately. */
constexpr unsigned long kSidePanelRedrawIntervalMs = 1000;
constexpr uint8_t kLcdHsyncPulseWidth = 7;
constexpr uint8_t kLcdHsyncBackPorch = 40;
constexpr uint8_t kLcdHsyncFrontPorch = 40;
constexpr uint8_t kLcdVsyncPulseWidth = 7;
constexpr uint8_t kLcdVsyncBackPorch = 10;
constexpr uint8_t kLcdVsyncFrontPorch = 10;

constexpr int kDisplayWidth = 800;
constexpr int kDisplayHeight = 480;
/** RGB panel is wired in RGB order (the GC9A01 build needed a BGR swap). */
constexpr bool kDisplayRgbOrder = false;

// --- Touch: GT911 capacitive controller on I2C ---
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_19;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_20;
constexpr gpio_num_t kTouchPinRst = GPIO_NUM_38;
/** INT is tied to GND on this board (not routed to a GPIO), so address is 0x5D. */
constexpr int kTouchPinInt = -1;
constexpr uint8_t kTouchI2cAddr = 0x5D;
/** Poll interval for the touch controller (ms). */
constexpr unsigned long kTouchPollMs = 30;

// --- Screen layout (landscape 800x480) ---
/** Square radar scope on the left; info / touch panel on the right. */
constexpr int kRadarOriginX = 0;
constexpr int kRadarOriginY = 0;
constexpr int kPanelX = 480;
constexpr int kPanelWidth = kDisplayWidth - kPanelX;  // 320

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 5000;
/** Redraw cadence; aircraft are dead-reckoned along their track/speed so motion
 *  is smooth. 2 Hz = 500 ms (4 Hz caused visible PSRAM jitter); each frame recomposes a 480x480 radar sprite and
 *  the 320x480 side panel in PSRAM and copies them into the panel frame buffer. */
constexpr unsigned long kRadarRedrawIntervalMs = 500;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- Display on/off schedule (set in the web portal) ---
/** NTP servers for local time; the schedule is ignored until time has synced. */
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.google.com";
/** Default POSIX TZ string (portal field "Time zone"). */
constexpr char kDefaultTimezone[] = "UTC0";
/** Default schedule window when first enabled (local time, minutes after midnight). */
constexpr uint16_t kDefaultDisplayOffMin = 23 * 60;
constexpr uint16_t kDefaultDisplayOnMin = 7 * 60;
/** A touch or BOOT tap while the display is scheduled off wakes it this long (ms). */
constexpr unsigned long kDisplayWakeMs = 60000;
/** Backlight PWM level (0-255) while the display is on. */
constexpr uint8_t kDisplayBrightness = 255;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
