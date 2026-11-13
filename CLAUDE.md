# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Arduino/PlatformIO firmware for an ADS-B "radar" on the **Sunton ESP32-8048S050** (ESP32-S3 N16R8, 5″ 800×480 parallel RGB panel, GT911 touch). It is a port of an ESP32-C3 + 1.28″ GC9A01 (240×240) project: the radar UI keeps the original structure but is scaled ×2 into a 480×480 scope on the left, plus a 320×480 touch info panel on the right. Graphics use LovyanGFX, setup uses WiFiManager, and JSON parsing uses ArduinoJson. Aircraft data comes from `https://opendata.adsb.fi/api/v3/`.

## Commands

PlatformIO is installed in the project venv (`.venv/bin/pio`); use it if `pio` is not on PATH. There is a single env, `esp32-8048s050`.

```bash
pio run -e esp32-8048s050                 # build
pio run -e esp32-8048s050 -t upload       # flash over USB-C (CH340, UART0)
pio device monitor                        # serial @ 115200
pio run -t merge -e esp32-8048s050        # single web-flash image -> .pio/build/esp32-8048s050/firmware-merged.bin (flash at 0x0)
./scripts/merge-firmware.sh [--no-build]  # same, copied to release/plane-radar-merged.bin
python3 scripts/build_large_airports.py   # regenerate runway dataset from OurAirports
```

```bash
./scripts/run-tests.sh                    # host unit tests (C++ + Python), no board needed
./scripts/run-tests.sh adsb               # only C++ test binaries whose name contains "adsb"
```

Host tests live in `tests/host/test_*.cpp`. Each file `#include`s the `.cpp` under test (so it can reach anonymous-namespace helpers and reset file-local state) and builds with the host compiler under ASan/UBSan against fakes in `tests/host/stubs/`: `Arduino.h` (fake `millis()`/`delay()`, `Serial` capture, `String`, `configTzTime`), `Preferences.h` (in-memory NVS that persists across `begin()`/`end()`), `HTTPClient.h`/`WiFiClientSecure.h` (scripted status codes and wire bytes), FreeRTOS no-op mutexes, and a `tft` that records backlight calls. ArduinoJson comes from `.pio/libdeps`. `tests/python/` covers `build_large_airports.py` and the consistency of the generated data. Rendering, touch and Wi-Fi code is not covered: it needs the board. There are no linters. CI (`.github/workflows/build.yml`) runs build, host tests, then merge. Pushing a `v*` tag runs `release.yml`, which attaches `plane-radar-<tag>.bin` to a GitHub Release.

## Architecture

**Threading model** (`src/main.cpp`): `loop()` on core 1 handles BOOT button, touch polling, Wi-Fi reconnect, and redraws the radar at 2 Hz (`kRadarRedrawIntervalMs`; 4 Hz caused PSRAM-bandwidth jitter, see `kLcdStripRows`). ADS-B HTTPS fetches block for 1–2 s, so they run in a separate FreeRTOS task (`adsbFetchTask`, pinned to core 0) every `kAdsbFetchIntervalMs`. `services::adsb` guards its aircraft buffer with a mutex. The renderer must read through `snapshotAircraft()`, not `aircraftList()`, and it dead-reckons each position from `track_deg`/`gs_knots`, the elapsed time since `lastUpdateMs()`, and `pos_age_ms`.

**Rendering** (`src/ui/radar_display.cpp`): every frame redraws the whole scope (grid, runways, labels, aircraft) into a 480×480 RGB565 PSRAM sprite and blits it with one `pushSprite`, which avoids flicker. `sidePanelDraw()` then redraws the right panel. If sprite allocation fails, it falls back to drawing directly to `tft`; that path assumes `kRadarOriginX/Y == 0` (enforced by a `static_assert`). Drawing code is templated or parameterized on the target `LGFX_Device`/sprite, so pass the draw target through rather than using the global `tft`.

**Geometry and theme**: `include/ui/radar_theme.h` defines all scope pixel sizes as original-240px values × `kScale` (2), in sprite-local coordinates. When porting or tweaking a layout value, keep that `N * kScale` form. Range presets live in `include/ui/radar_range.h`: the label shows ring 3, and the outer radius is ring3 × 4/3. `fetchRadiusKm()` scales the ADS-B query radius to the screen edge so off-ring "rim dots" have data.

**Hardware layer**: panel and touch setup is in `include/hardware/lgfx_config.hpp` (LovyanGFX `Bus_RGB`/`Panel_RGB`/`Touch_GT911`). All pins, timings, and layout constants are in `include/config.h`. The global `tft` comes from `hardware/display.h`. The smooth UI font `data/ui_font.vlw` is embedded through `board_build.embed_files` and loaded by `hardware/display_font.cpp`.

**HTTP**: `adsb_client.cpp` uses `WiFiClientSecure` verified against the roots pinned in `include/services/adsb_root_cas.h` (add a root there if adsb.fi changes CA) and streams the response through `services::http::BodyFramer` (`include/services/http_body_framing.h`), a byte-at-a-time decoder for identity/chunked bodies that feeds `deserializeJson` directly with an ArduinoJson field filter. Do not buffer the whole body, because the response can be large at the 50 km range. The parsed document is capped by a `BudgetAllocator` (`kJsonBudgetBytes`), and numeric fields are sanitized (non-finite or out-of-range values are dropped or clamped) before anything renders them.

**Portal security** (`services/portal_security`, glue in `wifi_setup.cpp`): WiFiManager has no auth or CSRF protection, so a `PortalGate` request handler is installed via `setWebServerCallback()` (which runs before WiFiManager registers its routes) and claims every request. It applies `portal_security::evaluate()`: Host allow-list (anti DNS-rebinding), then HTTP Digest auth (user `admin`) for LAN requests, then an Origin/Referer same-origin check on POSTs and state-changing GETs (`/erase`, `/restart`, `/u`, ...). Only then does it forward to the WiFiManager handler further down the chain, and it drops firmware upload bytes unless the upload was allowed. The password is random per device (NVS namespace `portalauth`), shown only on the device (setup screen, and the side panel's "WEB LOGIN" card from tapping the title row), and is also the setup AP's WPA2 passphrase. Requests arriving on the AP interface skip Digest auth. Keep policy changes in the pure `evaluate()` and its host tests.

**Persistence**: settings are stored in NVS via `Preferences`. Wi-Fi state is handled in `wifi_setup.cpp`, location in `radar_location.cpp`, and range, miles/km, and runway toggle in `radar_range.cpp` (namespace `planeradar`). The scheduled display off/on (times, POSIX TZ, SNTP) is in `display_schedule.cpp` (namespace `dispsched`); while the display is off, the backlight is off, redraws and ADS-B fetches pause, and a tap wakes it for `kDisplayWakeMs`. The WiFiManager portal (AP `PlaneRadar-Setup`, mDNS `plane-radar.local`, which needs `-DWM_MDNS`) also runs on the LAN IP after connecting, and writes these custom fields. Holding BOOT for 3 s wipes everything (and generates a new portal password) and reboots into the portal. The setup portal times out after `kWifiPortalTimeoutSec` idle, retries the saved network, and reopens if that fails.

**Generated data**: `include/data/large_airports.h` and `src/data/large_airports_data.cpp` are generated by `scripts/build_large_airports.py`. Do not hand-edit them.

## Board gotchas

- GPIO0 (BOOT) is a strapping pin. Holding it at power-on enters download mode.
- GT911 INT is tied to GND (address 0x5D), so touch is polled every `kTouchPollMs`.
- `kLcdPclkHz` 12 MHz gives a white screen on this panel (below its minimum refresh); 14 MHz works.
- Partition table `partitions/plane_radar_16mb.csv` has two 6 MB app slots (`ota_0`/`ota_1`). Changing partition offsets requires a USB flash; OTA cannot change the table.
- OTA uses WiFiManager's built-in `/update` page on the LAN portal behind `PortalGate` (Digest login + same-origin; upload `firmware.bin` / the release's `-ota.bin`, never the merged image). `onPreOtaUpdate()` in `wifi_setup.cpp` shows a status screen and sets `wifiOtaInProgress()`, which pauses the ADS-B task. Images are not signature-checked; access control is the gate.
- App rollback is on: `main.cpp` overrides `verifyRollbackLater()`, and `markFirmwareValid()` in `wifi_setup.cpp` confirms a new OTA image only once the web portal (LAN or setup AP) is up. A crash or reset before that reverts to the previous image.
- `lib_deps` and the GitHub Actions are pinned exactly (actions by commit SHA). `PortalGate` depends on WiFiManager internals (route names, `setWebServerCallback()` order, public `server`), so re-check it when bumping WiFiManager.
- USB CDC on boot is disabled (`ARDUINO_USB_CDC_ON_BOOT=0`) because serial goes through the CH340.
