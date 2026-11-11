# Plane Radar

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](https://github.com/MatixYo/ESP32-Plane-Radar/releases)

> **ESP32-8048S050 port.** This branch targets the **Sunton ESP32-8048S050** (ESP32-S3-WROOM-1 N16R8, 5″ 800×480 16-bit RGB panel, GT911 capacitive touch) instead of the original ESP32-C3 Super Mini + 1.28″ GC9A01. Panel pins and timings come from the ESP-IDF reference driver [mr-sven/esp32-8048S050C](https://github.com/mr-sven/esp32-8048S050C); the firmware stays on Arduino + LovyanGFX (`Bus_RGB` / `Panel_RGB` / `Touch_GT911`) so the radar code is unchanged in structure.
>
> Screen layout: a **480×480 radar scope** on the left (the original 240×240 UI scaled ×2, full RGB565 in PSRAM) and a **320×480 touch panel** on the right with ring range, aircraft count, the six nearest aircraft (callsign, type, altitude, distance + compass point) and **− / + / RWY** buttons.

Firmware that shows a circular **ADS-B radar** around your configured location, with **WiFiManager** for first-time setup.

## What it does

1. **Wi‑Fi setup** (if needed) — captive portal on AP **`PlaneRadar-Setup`**
2. **Radar** — live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid

After Wi‑Fi is saved, the device reconnects automatically; the radar runs in the main loop with periodic ADS-B updates (~5 s).

## Controls

**Touch screen**

| Action | Effect |
|--------|--------|
| Tap **−** | Wider range (next larger preset); saved to flash |
| Tap **+** | Tighter range (next smaller preset); saved to flash |
| Tap **RWY** | Toggle the runway overlay; saved to flash |
| Tap the radar scope | Cycle range preset (5 → 10 → 15 → 25 → 50 km) |

**BOOT button** (on the back of the board, GPIO 0, active LOW)

| Action | Effect |
|--------|--------|
| **Short tap** | Cycle range preset; saved to flash |
| **Hold 3 s** | Clear Wi‑Fi, location, and units; reboot into setup portal |

GPIO 0 is an ESP32-S3 strapping pin: holding BOOT *while powering on* puts the chip in download mode, so on this board press it only after the screen is lit.

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to **`PlaneRadar-Setup`**
2. Open **`http://plane-radar.local`** (preferred) or **`http://192.168.4.1`** — both are shown on the yellow setup screen; captive portal may open automatically
3. Set home Wi‑Fi, then save

**Reconfigure anytime** (after the device is on your network):

1. Open **`http://plane-radar.local`** or **`http://<device-ip>`** (e.g. from your router or serial log at boot)
2. Change Wi‑Fi, location, units, or runway overlay; save

The same portal runs on the setup AP and on the device’s LAN IP while connected to Wi‑Fi. mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). Some clients resolve `.local` slowly; use the IP if needed.

**Custom fields** (stored in NVS):

| Field | Purpose |
|-------|---------|
| **Latitude / Longitude** | Radar center and ADS-B query position (defaults in `config.h` until set) |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** (e.g. `6mi` vs `10km`) |
| **Show airport runways** | Major-airport runway overlay on the radar (off to hide) |

After a reset, the device reboots and shows the setup screen immediately (no “Connecting” loop on stale credentials).

## Radar display

### Grid

- Dark blue background, subdued green rings and crosshairs
- White **N / S / E / W** at the bezel; range label on the **east** spoke (ring 3 = ¾ of outer radius)
- White center dot

Layout and colors: `include/ui/radar_theme.h`.

### Range presets

| Ring 3 label | Outer radius (aircraft scale) |
|------------|-------------------------------|
| 5 km / 3 mi | ~6.7 km |
| 10 km / 6 mi | ~13.3 km (default) |
| 15 km / 9 mi | ~20 km |
| 25 km / 16 mi | ~33.3 km |
| 50 km / 31 mi | ~66.7 km (added for the 5″ screen) |

Preset and miles/km choice persist across reboot (`planeradar` NVS namespace).

### Runways

- Major airports from OurAirports (`large_airport`); all open runway strips in range (helipads excluded)
- Teal runway lines with one ICAO label per airport (e.g. `KJFK`); toggle in the Wi‑Fi setup portal
- Update the embedded list: `python3 scripts/build_large_airports.py`

### Aircraft

- **Inside the outer ring** — red heading triangle, magenta speed vector (clipped at the ring), callsign / type / altitude tags
- **Outside the ring** (still within ADS-B fetch) — small **red dot on the screen rim** at the correct bearing (direction cue; not distance-accurate past the ring)
- **Tags** — placed toward the **center**: west (left) → tag on the **right** of the symbol; east (right) → tag on the **left**

As range decreases (or aircraft approach), targets move inward; beyond-ring dots become full symbols when they cross the outer ring.

### ADS-B

- Source: `https://opendata.adsb.fi/api/v3/`
- Fetch radius: `ui::radar::fetchRadiusKm()` — scales with the active preset to roughly the screen edge (so rim dots have data)
- Poll interval: `kAdsbFetchIntervalMs` (5 s) in `config.h`
- Ground aircraft hidden by default (`kAdsbShowGroundAircraft`)

## Configuration

Edit **`include/config.h`** for hardware and behavior:

| Area | Keys / notes |
|------|----------------|
| Portal | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`) |
| Wi‑Fi timing | connect attempts, reconnect grace, portal timeout (`0` = no timeout) |
| BOOT | `kBootPin`, `kBootResetHoldMs`, `kBootTapMinMs` |
| Display (RGB) | `kLcdPinData[16]`, sync/DE/PCLK pins, porches, `kLcdPclkHz` (14 MHz; 12 MHz gives a white screen on this panel) |
| Touch (GT911) | I2C pins, reset pin, `kTouchI2cAddr` (0x5D — INT is tied to GND on this board) |
| Layout | `kRadarOriginX/Y`, `kPanelX`, `kPanelWidth` |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until portal overrides) |
| ADS-B | `kAdsbFetchIntervalMs`, `kAdsbShowGroundAircraft` |

Range presets: `include/ui/radar_range.h` (`kRangePresets`).

## Project layout

```
include/
  config.h
  hardware/
    lgfx_config.hpp
    display.h
    display_font.h
  data/
    large_airports.h
  ui/
    radar_theme.h
    radar_range.h
    radar_display.h
    runway_overlay.h
    status_screens.h
    side_panel.h           — right-hand touch / info panel
  services/
    wifi_setup.h
    radar_location.h
    adsb_client.h
data/
  ui_font.vlw              — embedded smooth UI font (Noto Sans Bold)
scripts/
  build_large_airports.py
src/
  main.cpp
  data/
    large_airports_data.cpp
  hardware/
  ui/
  services/
```

## Hardware (Sunton ESP32-8048S050, all on-board)

| Function | GPIO |
|----------|------|
| RGB data B0–B4 | 8, 3, 46, 9, 1 |
| RGB data G0–G5 | 5, 6, 7, 15, 16, 4 |
| RGB data R0–R4 | 45, 48, 47, 21, 14 |
| HSYNC / VSYNC / DE / PCLK | 39 / 41 / 40 / 42 |
| Backlight (PWM) | 2 |
| Touch I2C SDA / SCL / RST | 19 / 20 / 38 (INT not connected) |
| BOOT (user) | 0 |

No wiring needed — everything is on the board. Power and serial are via the USB‑C port (CH340 USB‑UART).

## Build

```bash
pio run -t upload
pio device monitor
```

- PlatformIO env: **`esp32-8048s050`** (board `esp32-s3-devkitc-1`, 16 MB QIO flash, OPI PSRAM)
- Serial: **115200** baud over the on-board CH340 (USB CDC on boot disabled)
- Partition table: `partitions/plane_radar_16mb.csv` (two 6 MB app slots for OTA)

### OTA update (over Wi‑Fi)

Once the device runs an OTA-capable build (flash it over USB once after changing to the two-slot partition table), update it from a browser:

1. Build: `pio run -e esp32-8048s050` → `.pio/build/esp32-8048s050/firmware.bin` (the app image — **not** `firmware-merged.bin`)
2. Open **`http://plane-radar.local/update`** (or `http://<device-ip>/update`), choose the `.bin`, press **Update**
3. The screen shows “Updating firmware”; the device reboots into the new build. Settings in NVS are kept.

The upload page has no password — anyone on your LAN can reach it.

### Web-flashable release image

Single `.bin` for [esptool-js](https://espressif.github.io/esptool-js/) and similar tools (ESP32-S3, 16 MB, flash at **0x0**):

```bash
chmod +x scripts/merge-firmware.sh   # once
./scripts/merge-firmware.sh
```

Writes `release/plane-radar-merged.bin`. Skip rebuild if firmware is already built:

```bash
./scripts/merge-firmware.sh --no-build
```

Or via PlatformIO only (output: `.pio/build/esp32-8048s050/firmware-merged.bin`):

```bash
pio run -e esp32-8048s050
pio run -t merge -e esp32-8048s050
```

Put the board in download mode (hold **BOOT**, tap **RESET**), then flash with Chrome/Edge over USB.

### CI and releases (GitHub Actions)

| Workflow | When | Output |
|----------|------|--------|
| [Build](.github/workflows/build.yml) | Push / PR to `main` | Artifact `plane-radar-esp32-8048s050` (merged + split `.bin` files, ~90 days) |
| [Release](.github/workflows/release.yml) | Git tag `v*` (e.g. `v1.0.0`) | GitHub Release asset `plane-radar-v1.0.0.bin` + `.sha256` |

To ship a version users can download:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The release workflow builds firmware in CI and attaches the merged image to the release. Download from **Releases** on GitHub, then flash at **0x0** (ESP32-S3, 16 MB).

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
