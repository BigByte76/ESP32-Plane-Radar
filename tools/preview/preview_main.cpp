// Renders the firmware's real screens (radar, side panel, status screens) on a
// desktop and saves them as PNGs. Run through tools/preview/render.sh.
//
// The UI code is compiled unchanged; only hardware/display.h (tft becomes an
// in-memory sprite) and <WiFi.h> are shimmed, and the ADS-B list is fed through
// the real parser via the host test stubs' fake HTTP client.
#include <Arduino.h>
#include <HTTPClient.h>
#include <zlib.h>

#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/portal_security.h"
#include "services/radar_location.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/side_panel.h"
#include "ui/status_screens.h"

// ---- Display shim implementation ----

LGFX tft;

bool LGFX::init() {
  setColorDepth(lgfx::color_depth_t::rgb565_2Byte);
  return createSprite(config::kDisplayWidth, config::kDisplayHeight) != nullptr;
}

void displayInit() {
  tft.init();
  tft.setTextWrap(false);
  displayFontInit();
}

void displayPushSpriteGently(LGFX_Sprite& sprite, int x, int y) {
  sprite.pushSprite(&tft, x, y);
}

void displayFillGently(lgfx::LovyanGFX& gfx, uint32_t color) {
  gfx.fillRect(0, 0, gfx.width(), gfx.height(), color);
}

namespace {

// ---- Minimal PNG writer (RGB8, one IDAT) ----

void putU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
}

void putChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  putU32(out, static_cast<uint32_t>(data.size()));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  putU32(out, static_cast<uint32_t>(
                  crc32(0, out.data() + start, static_cast<uInt>(out.size() - start))));
}

bool savePng(const std::string& path) {
  const int w = tft.width();
  const int h = tft.height();
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(h) * (w * 3 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);  // filter: none
    for (int x = 0; x < w; ++x) {
      const auto c = tft.readPixelRGB(x, y);
      raw.push_back(c.R8());
      raw.push_back(c.G8());
      raw.push_back(c.B8());
    }
  }
  uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> z(zlen);
  if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK) {
    return false;
  }
  z.resize(zlen);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  putU32(ihdr, static_cast<uint32_t>(w));
  putU32(ihdr, static_cast<uint32_t>(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  putChunk(png, "IHDR", ihdr);
  putChunk(png, "IDAT", z);
  putChunk(png, "IEND", {});

  FILE* f = fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
  fclose(f);
  std::printf("wrote %s\n", path.c_str());
  return ok;
}

// ---- Sample traffic around London Heathrow (EGLL) ----

const char* kTraffic = R"({"ac":[
 {"hex":"400a1b","flight":"BAW117  ","t":"B77W","lat":51.4775,"lon":-0.2900,"alt_baro":2400,"gs":160,"track":270,"true_heading":268},
 {"hex":"406b2c","flight":"VIR3N   ","t":"A35K","lat":51.4700,"lon":-0.1700,"alt_baro":5000,"gs":190,"track":271,"true_heading":270},
 {"hex":"4ca8d1","flight":"EIN158  ","t":"A20N","lat":51.5400,"lon":-0.7000,"alt_baro":6500,"gs":250,"track":300,"true_heading":297},
 {"hex":"3c6589","flight":"DLH4AN  ","t":"A321","lat":51.3300,"lon":-0.2400,"alt_baro":9500,"gs":300,"track":310,"true_heading":307},
 {"hex":"a8c2f0","flight":"UAL901  ","t":"B789","lat":51.6300,"lon":-0.2000,"alt_baro":11000,"gs":320,"track":230,"true_heading":227},
 {"hex":"484506","flight":"KLM1005 ","t":"E190","lat":51.7000,"lon":-0.5200,"alt_baro":14000,"gs":350,"track":170,"true_heading":168},
 {"hex":"400f77","flight":"EZY82QK ","t":"A319","lat":51.3700,"lon":-0.6600,"alt_baro":5200,"gs":240,"track":40,"true_heading":38},
 {"hex":"4ca6f2","flight":"RYR93LW ","t":"B738","lat":51.1600,"lon":-0.0200,"alt_baro":24000,"gs":440,"track":320,"true_heading":318},
 {"hex":"43c2a1","flight":"RRR4910 ","t":"C17","lat":51.8200,"lon":-1.0000,"alt_baro":21000,"gs":410,"track":110,"true_heading":108}
]})";

void loadTraffic() {
  fakehttp::reset();
  fakehttp::g_chunked = false;
  fakehttp::g_wire_body = kTraffic;
  fakehttp::g_content_length = static_cast<int>(fakehttp::g_wire_body.size());
  services::adsb::fetchUpdate(services::location::lat(), services::location::lon(),
                              ui::radar::fetchRadiusKm());
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : ".";
  fakearduino::g_millis = 100000;

  services::portal_security::init();  // example password from the fake RNG
  displayInit();
  services::location::saveFromStrings("51.470000", "-0.454300");
  ui::radar::rangeInit();
  ui::radar::rangeZoomOut();  // 10 -> 15 km
  ui::radar::rangeZoomOut();  // 15 -> 25 km
  services::adsb::init();
  loadTraffic();

  ui::radarDisplayDraw();
  savePng(out + "/display-radar.png");

  ui::sidePanelToggleLoginCard();
  ui::sidePanelDraw(true);
  savePng(out + "/display-web-login.png");
  ui::sidePanelToggleLoginCard();

  statusScreenPortal();
  savePng(out + "/display-setup.png");

  statusScreenConnectingBegin("HomeNetwork");
  for (int i = 0; i < 8; ++i) statusScreenConnectingTick();
  savePng(out + "/display-connecting.png");

  statusScreenOtaUpdating();
  savePng(out + "/display-ota.png");

  std::printf("example portal password: %s\n", services::portal_security::password());
  return 0;
}
