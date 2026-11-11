#include "ui/side_panel.h"

#include <Arduino.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdio>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui {
namespace {

// --- Layout (panel-local coordinates; panel is kPanelWidth x kDisplayHeight) ---
constexpr int kW = config::kPanelWidth;
constexpr int kH = config::kDisplayHeight;
constexpr int kPadX = 14;

constexpr int kTitleY = 10;
constexpr int kStatusY = 44;
constexpr int kDividerY = 74;

constexpr int kListTopY = 84;
constexpr int kRowH = 52;
constexpr int kMaxRows = 6;  // 84 + 6*52 = 396

constexpr int kButtonTopY = 404;
constexpr int kButtonH = 66;
constexpr int kButtonGap = 8;
constexpr int kButtonCount = 3;
constexpr int kButtonW = (kW - kButtonGap * (kButtonCount + 1)) / kButtonCount;
constexpr int kButtonRadius = 10;

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;
constexpr float kKmPerMile = 1.609344f;

LGFX_Sprite s_panel(&tft);
bool s_panel_ready = false;

struct NearItem {
  size_t index;
  float dist_km;
  float bearing_deg;
};

bool ensurePanelSprite() {
  if (s_panel_ready) {
    return true;
  }
  s_panel.setPsram(true);
  s_panel.setColorDepth(lgfx::color_depth_t::rgb565_2Byte);
  if (!s_panel.createSprite(kW, kH)) {
    Serial.println("panel: sprite alloc failed");
    return false;
  }
  s_panel.setTextWrap(false);
  s_panel_ready = true;
  return true;
}

void offsetKm(float lat, float lon, float* dist_km, float* bearing_deg) {
  const float center_lat_rad =
      static_cast<float>(services::location::lat()) * kDegToRad;
  const float dx = static_cast<float>(lon - services::location::lon()) *
                   kKmPerDeg * cosf(center_lat_rad);
  const float dy = static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf(dx * dx + dy * dy);
  float b = atan2f(dx, dy) / kDegToRad;  // 0 = N, 90 = E
  if (b < 0.0f) {
    b += 360.0f;
  }
  *bearing_deg = b;
}

const char* compassPoint(float bearing_deg) {
  static const char* const kPoints[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  const int idx = static_cast<int>(lroundf(bearing_deg / 45.0f)) % 8;
  return kPoints[idx];
}

void formatDistance(char* buf, size_t len, float dist_km) {
  const bool miles = radar::useMiles();
  const float d = miles ? dist_km / kKmPerMile : dist_km;
  if (d < 10.0f) {
    snprintf(buf, len, "%.1f%s", d, miles ? "mi" : "km");
  } else {
    snprintf(buf, len, "%.0f%s", d, miles ? "mi" : "km");
  }
}

/** Partial selection sort: first `k` entries become the nearest, in order. */
size_t pickNearest(NearItem* items, size_t n, size_t k) {
  const size_t limit = n < k ? n : k;
  for (size_t i = 0; i < limit; ++i) {
    size_t best = i;
    for (size_t j = i + 1; j < n; ++j) {
      if (items[j].dist_km < items[best].dist_km) {
        best = j;
      }
    }
    if (best != i) {
      const NearItem tmp = items[i];
      items[i] = items[best];
      items[best] = tmp;
    }
  }
  return limit;
}

int buttonX(int i) { return kButtonGap + i * (kButtonW + kButtonGap); }

void drawButton(int i, const char* label, bool active) {
  const int x = buttonX(i);
  const uint16_t fill = active ? radar::kColorGrid : radar::kColorBackground;
  s_panel.fillRoundRect(x, kButtonTopY, kButtonW, kButtonH, kButtonRadius, fill);
  s_panel.drawRoundRect(x, kButtonTopY, kButtonW, kButtonH, kButtonRadius,
                        radar::kColorGrid);
  s_panel.drawRoundRect(x + 1, kButtonTopY + 1, kButtonW - 2, kButtonH - 2,
                        kButtonRadius - 1, radar::kColorGrid);
  s_panel.setTextDatum(textdatum_t::middle_center);
  s_panel.setTextColor(radar::kColorLabel, fill);
  s_panel.drawString(label, x + kButtonW / 2, kButtonTopY + kButtonH / 2);
}

void drawHeader(size_t total, size_t in_range) {
  s_panel.setTextDatum(textdatum_t::top_left);
  s_panel.setFont(&fonts::FreeSansBold12pt7b);
  s_panel.setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_panel.drawString("PLANE RADAR", kPadX, kTitleY);

  char range_label[12];
  radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  char ring[24];
  snprintf(ring, sizeof(ring), "Ring %s", range_label);
  char count[32];
  snprintf(count, sizeof(count), "%u in view / %u", static_cast<unsigned>(in_range),
           static_cast<unsigned>(total));
  s_panel.setFont(&fonts::FreeSans9pt7b);
  s_panel.setTextColor(radar::kColorRunwayLabel, radar::kColorBackground);
  s_panel.drawString(ring, kPadX, kStatusY);
  s_panel.setTextDatum(textdatum_t::top_right);
  s_panel.drawString(count, kW - kPadX, kStatusY);
  s_panel.setTextDatum(textdatum_t::top_left);

  s_panel.drawFastHLine(kPadX, kDividerY, kW - kPadX * 2, radar::kColorGrid);
}

void drawRow(int row, const services::adsb::Aircraft& plane, const NearItem& item) {
  const int y = kListTopY + row * kRowH;

  // Line 1: callsign (left), distance + compass point (right)
  s_panel.setFont(&fonts::FreeSansBold12pt7b);
  s_panel.setTextDatum(textdatum_t::top_left);
  s_panel.setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_panel.drawString(plane.callsign[0] != '\0' ? plane.callsign : "------", kPadX, y);

  char dist[24];
  char dist_val[12];
  formatDistance(dist_val, sizeof(dist_val), item.dist_km);
  snprintf(dist, sizeof(dist), "%s %s", dist_val, compassPoint(item.bearing_deg));
  s_panel.setTextDatum(textdatum_t::top_right);
  s_panel.setTextColor(radar::kColorRunwayLabel, radar::kColorBackground);
  s_panel.drawString(dist, kW - kPadX, y);

  // Line 2: type (left, yellow) and altitude (right, blue)
  s_panel.setFont(&fonts::FreeSans9pt7b);
  s_panel.setTextDatum(textdatum_t::top_left);
  s_panel.setTextColor(radar::kColorTagType, radar::kColorBackground);
  s_panel.drawString(plane.type[0] != '\0' ? plane.type : "?", kPadX, y + 27);
  s_panel.setTextDatum(textdatum_t::top_right);
  s_panel.setTextColor(radar::kColorTagAltitude, radar::kColorBackground);
  s_panel.drawString(plane.alt, kW - kPadX, y + 27);
}

}  // namespace

void sidePanelDraw(bool force) {
  static unsigned long s_last_draw_ms = 0;
  const unsigned long now = millis();
  if (!force && s_last_draw_ms != 0 &&
      now - s_last_draw_ms < config::kSidePanelRedrawIntervalMs) {
    return;
  }
  if (!ensurePanelSprite()) {
    return;
  }
  s_last_draw_ms = now;

  static services::adsb::Aircraft planes[services::adsb::kMaxAircraft];
  static NearItem items[services::adsb::kMaxAircraft];
  unsigned long base_ms = 0;
  const size_t n = services::adsb::snapshotAircraft(
      planes, services::adsb::kMaxAircraft, &base_ms);

  const float outer_km = radar::rangeCurrent().outer_km;
  size_t in_range = 0;
  for (size_t i = 0; i < n; ++i) {
    items[i].index = i;
    offsetKm(planes[i].lat, planes[i].lon, &items[i].dist_km, &items[i].bearing_deg);
    if (items[i].dist_km <= outer_km) {
      ++in_range;
    }
  }
  const size_t shown = pickNearest(items, n, kMaxRows);

  displayFillGently(s_panel, radar::kColorBackground);
  // Separator against the radar scope
  s_panel.drawFastVLine(0, 0, kH, radar::kColorGrid);
  s_panel.drawFastVLine(1, 0, kH, radar::kColorGrid);

  drawHeader(n, in_range);

  if (shown == 0) {
    s_panel.setFont(&fonts::FreeSans9pt7b);
    s_panel.setTextDatum(textdatum_t::middle_center);
    s_panel.setTextColor(radar::kColorRunwayLabel, radar::kColorBackground);
    s_panel.drawString(base_ms == 0 ? "Waiting for ADS-B data..." : "No aircraft nearby",
                       kW / 2, kListTopY + (kMaxRows * kRowH) / 2);
  }
  for (size_t r = 0; r < shown; ++r) {
    drawRow(static_cast<int>(r), planes[items[r].index], items[r]);
  }

  s_panel.setFont(&fonts::FreeSansBold18pt7b);
  drawButton(0, "-", false);
  drawButton(1, "+", false);
  s_panel.setFont(&fonts::FreeSansBold12pt7b);
  drawButton(2, "RWY", radar::showRunways());

  displayPushSpriteGently(s_panel, config::kPanelX, 0);
}

PanelButton sidePanelHitTest(int x, int y) {
  const int lx = x - config::kPanelX;
  if (lx < 0 || lx >= kW || y < kButtonTopY - kButtonGap || y >= kH) {
    return PanelButton::kNone;
  }
  // Generous hit boxes: split the button strip into equal thirds.
  const int third = kW / kButtonCount;
  const int idx = lx / third;
  switch (idx) {
    case 0:
      return PanelButton::kZoomOut;
    case 1:
      return PanelButton::kZoomIn;
    default:
      return PanelButton::kRunways;
  }
}

}  // namespace ui
