/**
 * Plane Radar — WiFi setup, then radar UI on the Sunton ESP32-8048S050
 * (5" 800x480 RGB panel + GT911 touch): 480x480 scope plus a touch info panel.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/display_schedule.h"
#include "services/portal_security.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/side_panel.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_redraw_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeChanged() {
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void onRangeTap() {
  ui::radar::rangeNext();
  onRangeChanged();
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    if (services::display_schedule::isOff()) {
      services::display_schedule::wakeTemporarily();
      return;
    }
    onRangeTap();
  }
}

/** Act on a completed tap (finger lifted) at screen coordinate x, y. */
void onTouchTap(int x, int y) {
  // While the schedule has the display off, a tap only wakes it.
  if (services::display_schedule::isOff()) {
    services::display_schedule::wakeTemporarily();
    return;
  }
  if (!g_radar_visible) {
    return;
  }
  switch (ui::sidePanelHitTest(x, y)) {
    case ui::PanelButton::kZoomIn:
      if (ui::radar::rangeZoomIn()) {
        onRangeChanged();
      }
      return;
    case ui::PanelButton::kZoomOut:
      if (ui::radar::rangeZoomOut()) {
        onRangeChanged();
      }
      return;
    case ui::PanelButton::kRunways:
      ui::radar::toggleRunways();
      ui::radarDisplayDraw();
      return;
    case ui::PanelButton::kLoginInfo:
      ui::sidePanelToggleLoginCard();
      ui::sidePanelDraw(true);
      return;
    case ui::PanelButton::kNone:
      break;
  }
  // Anywhere on the radar scope: cycle range, same as a BOOT tap.
  if (x < config::kPanelX) {
    onRangeTap();
  }
}

/** Poll the GT911; fire onTouchTap() on release of a short press. */
void handleTouch() {
  static unsigned long s_last_poll_ms = 0;
  static bool s_down = false;
  static unsigned long s_down_ms = 0;
  static int32_t s_down_x = 0;
  static int32_t s_down_y = 0;
  constexpr unsigned long kMaxTapMs = 1500;

  const unsigned long now = millis();
  if (now - s_last_poll_ms < config::kTouchPollMs) {
    return;
  }
  s_last_poll_ms = now;

  int32_t x = 0;
  int32_t y = 0;
  const bool touching = tft.getTouch(&x, &y) > 0;
  if (touching && !s_down) {
    s_down = true;
    s_down_ms = now;
    s_down_x = x;
    s_down_y = y;
  } else if (!touching && s_down) {
    s_down = false;
    if (now - s_down_ms <= kMaxTapMs) {
      onTouchTap(static_cast<int>(s_down_x), static_cast<int>(s_down_y));
    }
  }
}

// ADS-B fetch runs on its own task: the HTTPS request blocks for ~1-2 s, and
// keeping it off the main loop lets the radar keep redrawing (dead-reckoned) at
// 2 Hz throughout. The task publishes into the shared aircraft buffer under a
// lock; the render loop reads a snapshot.
void adsbFetchTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && !wifiOtaInProgress() &&
        !services::display_schedule::isOff()) {
      services::adsb::fetchUpdate(services::location::lat(),
                                  services::location::lon(),
                                  ui::radar::fetchRadiusKm());
    }
    vTaskDelay(pdMS_TO_TICKS(config::kAdsbFetchIntervalMs));
  }
}

}  // namespace

/** Keep a freshly flashed (OTA) image in "pending verify" instead of letting
 *  the Arduino core accept it at boot: wifi_setup marks it valid once the web
 *  portal is reachable again, and a crash or reset before that rolls back to
 *  the previous image. */
extern "C" bool verifyRollbackLater() { return true; }

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar (ESP32-8048S050)");
  Serial.printf("PSRAM: %u bytes free\n", static_cast<unsigned>(ESP.getFreePsram()));

  bootButtonInit();
  // Before Wi-Fi or the display start: a first-boot password is drawn from the
  // hardware RNG with its standalone entropy source switched on.
  services::portal_security::init();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::display_schedule::init();
  services::adsb::init();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }

  // Start the background ADS-B fetch (pinned to core 0, away from the render
  // loop on core 1). It checks Wi-Fi state each cycle.
  xTaskCreatePinnedToCore(adsbFetchTask, "adsb", 20480, nullptr, 1, nullptr, 0);
}

void loop() {
  handleBootButton();
  handleTouch();
  wifiLoop();

  // Scheduled display-off turns the backlight off; Wi-Fi upkeep below still runs.
  const bool display_off = services::display_schedule::update();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (display_off) {
      // Skip drawing while dark; clearing the flag forces a full redraw on wake.
      g_radar_visible = false;
    } else if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_redraw_ms >= config::kRadarRedrawIntervalMs) {
      // Redraw at 2 Hz with dead-reckoned positions; the ADS-B fetch runs on
      // its own task (adsbFetchTask).
      g_last_redraw_ms = millis();
      ui::radarDisplayRefreshAircraft();
    }
  }

  delay(10);
}
