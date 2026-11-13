# Plane Radar

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](https://github.com/MatixYo/ESP32-Plane-Radar/releases)

> **ESP32-8048S050 port.** This branch targets the **Sunton ESP32-8048S050** (ESP32-S3-WROOM-1 N16R8, 5″ 800×480 16-bit RGB panel, GT911 capacitive touch) instead of the original ESP32-C3 Super Mini + 1.28″ GC9A01. Panel pins and timings come from the ESP-IDF reference driver [mr-sven/esp32-8048S050C](https://github.com/mr-sven/esp32-8048S050C); the firmware stays on Arduino + LovyanGFX (`Bus_RGB` / `Panel_RGB` / `Touch_GT911`) so the radar code is unchanged in structure.
>
> Screen layout: a **480×480 radar scope** on the left (the original 240×240 UI scaled ×2, full RGB565 in PSRAM) and a **320×480 touch panel** on the right with ring range, aircraft count, the six nearest aircraft (callsign, type, altitude, distance + compass point) and **− / + / RWY** buttons.

Firmware that shows a circular **ADS-B radar** around your configured location, with **WiFiManager** for first-time setup.

## What it does

1. **Wi‑Fi setup** (if needed) — captive portal on the password-protected AP **`PlaneRadar-Setup`**
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
| Tap the panel title (**WEB LOGIN**) | Show / hide the web portal address, user and password (hides itself after a minute) |
| Tap while the display is scheduled off | Wake it for a minute |

**BOOT button** (on the back of the board, GPIO 0, active LOW)

| Action | Effect |
|--------|--------|
| **Short tap** | Cycle range preset; saved to flash |
| **Hold 3 s** | Clear Wi‑Fi, location, units and display schedule, generate a new portal password; reboot into setup portal |

GPIO 0 is an ESP32-S3 strapping pin: holding BOOT *while powering on* puts the chip in download mode, so on this board press it only after the screen is lit.

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to **`PlaneRadar-Setup`** using the **password shown on the yellow setup screen**
2. Open **`http://plane-radar.local`** (preferred) or **`http://192.168.4.1`** — both are shown on the setup screen; captive portal may open automatically
3. Set home Wi‑Fi, then save

If nobody uses the setup portal for 10 minutes, it closes, the device retries the saved network, and the portal reopens if that still fails.

**Reconfigure anytime** (after the device is on your network):

1. Tap **WEB LOGIN** at the top of the side panel to see the address, user and password
2. Open **`http://plane-radar.local`** or **`http://<device-ip>`** and log in as **`admin`** with that password
3. Change Wi‑Fi, location, units, runway overlay or display schedule; save

The same portal runs on the setup AP and on the device’s LAN IP while connected to Wi‑Fi. The portal password is random per device, stored in flash, and only ever shown on the device's screen. It is both the setup AP's Wi‑Fi password and the LAN login (see [Security](#security)). mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). Some clients resolve `.local` slowly; use the IP if needed.

**Custom fields** (stored in NVS):

| Field | Purpose |
|-------|---------|
| **Latitude / Longitude** | Radar center and ADS-B query position (defaults in `config.h` until set) |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** (e.g. `6mi` vs `10km`) |
| **Show airport runways** | Major-airport runway overlay on the radar (off to hide) |
| **Turn display off on a schedule** | Backlight off between **Display off at** and **Display on at**; redraws and ADS-B fetches pause |
| **Time zone** | Used by the schedule (list of common zones, or **Other** for any POSIX TZ string) |

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
- HTTPS is verified against the root CAs pinned in `include/services/adsb_root_cas.h`. If adsb.fi moves to a CA outside that set, aircraft stop loading until its root is added there

## Configuration

Edit **`include/config.h`** for hardware and behavior:

| Area | Keys / notes |
|------|----------------|
| Portal | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`) |
| Wi‑Fi timing | connect attempts, reconnect grace, setup portal idle timeout (`kWifiPortalTimeoutSec`, 600 s) |
| BOOT | `kBootPin`, `kBootResetHoldMs`, `kBootTapMinMs` |
| Display (RGB) | `kLcdPinData[16]`, sync/DE/PCLK pins, porches, `kLcdPclkHz` (14 MHz; 12 MHz gives a white screen on this panel) |
| Touch (GT911) | I2C pins, reset pin, `kTouchI2cAddr` (0x5D — INT is tied to GND on this board) |
| Layout | `kRadarOriginX/Y`, `kPanelX`, `kPanelWidth` |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until portal overrides) |
| ADS-B | `kAdsbFetchIntervalMs`, `kAdsbShowGroundAircraft` |
| Display schedule | default off/on times and time zone, `kDisplayWakeMs` |

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
    portal_security.h      — portal password and web request policy
    radar_location.h
    adsb_client.h
    adsb_root_cas.h        — pinned root CAs for adsb.fi
    display_schedule.h
data/
  ui_font.vlw              — embedded smooth UI font (Noto Sans Bold)
scripts/
  build_large_airports.py
  run-tests.sh             — host unit tests
tests/
  host/                    — C++ tests against stubbed Arduino/ESP APIs
  python/
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

### Tests

```bash
./scripts/run-tests.sh          # C++ host tests (ASan/UBSan) + Python tests, no board needed
./scripts/run-tests.sh adsb     # only C++ tests whose name contains "adsb"
```

Rendering, touch and Wi‑Fi need the board and are not covered.

### OTA update (over Wi‑Fi)

Once the device runs an OTA-capable build (flash it over USB once after changing to the two-slot partition table), update it from a browser:

1. Get the app image: build with `pio run -e esp32-8048s050` → `.pio/build/esp32-8048s050/firmware.bin`, or download `plane-radar-<tag>-ota.bin` from a release. **Not** the merged image.
2. Open **`http://plane-radar.local/update`** (or `http://<device-ip>/update`) and log in as **`admin`** with the password from the **WEB LOGIN** card
3. Choose the `.bin` and press **Update**
4. The screen shows “Updating firmware”; the device reboots into the new build. Settings in NVS are kept.

A new image is only confirmed once its web portal comes up. If it crashes or resets before then, the device boots the previous firmware again.

Updating *from* a build older than the portal password goes through that build's unprotected upload page; uploads after that need the login.

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
| [Release](.github/workflows/release.yml) | Git tag `v*` (e.g. `v1.0.0`) | GitHub Release: `plane-radar-v1.0.0.bin` (merged, USB flash), `plane-radar-v1.0.0-ota.bin` (for `/update`), `plane-radar-v1.0.0.sha256`, plus a signed build-provenance attestation |

To ship a version users can download:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The release workflow builds firmware in CI and attaches both images to the release. Download from **Releases** on GitHub, then flash the merged image at **0x0** (ESP32-S3, 16 MB), or upload the `-ota.bin` at `/update`.

To check that a downloaded image was built by this repository's release workflow:

```bash
gh attestation verify plane-radar-v1.0.0-ota.bin -R <owner>/<repo>
```

Library versions in `platformio.ini` and the GitHub Actions are pinned exactly (actions by commit SHA), so updating any of them is a deliberate change.

## Security

| Area | Protection |
|------|------------|
| Setup AP | WPA2, with the per-device password shown on the setup screen |
| LAN portal and `/update` | HTTP Digest login (`admin` + the on-screen password); the password never crosses the network in clear |
| Cross-site requests | Uploads, form saves, erase, restart and exit must come from the portal's own pages (Origin/Referer check), so another website can't trigger them through your browser |
| DNS rebinding | Requests are only accepted under the device's own names (`plane-radar.local`, its IP, `192.168.4.1`); others are redirected |
| Portal fields | Time zone input is limited to POSIX TZ characters and the page escapes it, preventing script injection |
| ADS-B feed | TLS verified against pinned roots; numbers are range-checked and parsed JSON is capped at 256 KB |
| Bad updates | App rollback: a new image that crashes before its portal comes up is replaced by the previous one |

Limits to be aware of:

- **Firmware images are not signed.** Anyone with the portal password can install any firmware, so treat the password as the key to the device.
- **The portal uses plain HTTP.** Digest auth protects the password, but someone on your network could still read or tamper with pages in transit.
- **Physical access is full access.** The BOOT button and USB serial (download mode) can always reflash or reset the device; holding BOOT 3 s also issues a new password.

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
