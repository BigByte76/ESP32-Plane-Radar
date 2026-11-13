#!/usr/bin/env bash
# Render preview PNGs of the device screens and the web portal into docs/preview/.
#
#   ./tools/preview/render.sh            # both
#   ./tools/preview/render.sh --display  # device screens only
#   ./tools/preview/render.sh --portal   # web portal only
#
# Device screens: the firmware's UI sources are compiled for the host against
# LovyanGFX's desktop platform (with SDL itself compiled out; host_platform.cpp
# supplies the few timing/heap hooks) and drawn into an in-memory 800x480
# sprite. Needs only a C++17 compiler and zlib.
# Portal: WiFiManager pages are rebuilt from its own HTML templates with this
# firmware's fields, then screenshotted with headless Chrome.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
OUT="$ROOT/docs/preview"
BUILD="$ROOT/.pio/preview"
mkdir -p "$OUT" "$BUILD"

do_display=1
do_portal=1
case "${1:-}" in
  --display) do_portal=0 ;;
  --portal) do_display=0 ;;
esac

if [[ $do_display == 1 ]]; then
  LGFX="$ROOT/.pio/libdeps/esp32-8048s050/LovyanGFX/src"
  JSON="$ROOT/.pio/libdeps/esp32-8048s050/ArduinoJson/src"
  if [[ ! -d "$LGFX" ]]; then
    PIO="$(command -v pio || echo "$ROOT/.venv/bin/pio")"
    "$PIO" pkg install -e esp32-8048s050
  fi
  CXX="${CXX:-c++}"
  inc=(-I"$ROOT/tools/preview/shim" -I"$ROOT/tests/host/stubs" -I"$ROOT/include"
       -I"$LGFX" -I"$JSON")
  flags=(-std=gnu++17 -O1 -w -DLGFX_SDL)

  # The test stubs define a FakeTft `tft`; the preview supplies a real one.
  grep -v '^FakeTft tft;' "$ROOT/tests/host/stubs/stubs.cpp" > "$BUILD/stubs.cpp"

  srcs=(
    "$ROOT/tools/preview/preview_main.cpp"
    "$BUILD/stubs.cpp"
    "$ROOT/src/ui/radar_display.cpp" "$ROOT/src/ui/side_panel.cpp"
    "$ROOT/src/ui/status_screens.cpp" "$ROOT/src/ui/runway_overlay.cpp"
    "$ROOT/src/ui/radar_range.cpp" "$ROOT/src/hardware/display_font.cpp"
    "$ROOT/src/services/adsb_client.cpp" "$ROOT/src/services/radar_location.cpp"
    "$ROOT/src/services/portal_security.cpp" "$ROOT/src/data/large_airports_data.cpp"
    "$ROOT/tools/preview/host_platform.cpp"
    "$LGFX/lgfx/v1/LGFXBase.cpp" "$LGFX/lgfx/v1/lgfx_fonts.cpp"
    "$LGFX/lgfx/v1/lgfx_v1.cpp" "$LGFX/lgfx/v1/lgfx_v1_panel.cpp"
  )
  objs=()
  for s in "${srcs[@]}"; do
    o="$BUILD/$(basename "${s%.*}").o"
    "$CXX" "${flags[@]}" "${inc[@]}" -c "$s" -o "$o"
    objs+=("$o")
  done
  # C helpers and the bitmap font tables LGFXBase references (built once).
  for c in "$LGFX"/lgfx/utility/*.c "$LGFX"/lgfx/Fonts/*/*.c; do
    o="$BUILD/$(basename "${c%.*}").o"
    [[ -f "$o" ]] || cc -O0 -w -c "$c" -o "$o"
    objs+=("$o")
  done
  cc -c -DFONT_PATH="\"$ROOT/data/ui_font.vlw\"" "$ROOT/tools/preview/font_blob.S" \
     -o "$BUILD/font_blob.o"
  "$CXX" "${objs[@]}" "$BUILD/font_blob.o" -lz -o "$BUILD/preview"
  "$BUILD/preview" "$OUT"
fi

if [[ $do_portal == 1 ]]; then
  python3 "$ROOT/tools/preview/portal_pages.py" "$BUILD/portal"
  CHROME="${CHROME:-}"
  for c in "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
           "$(command -v google-chrome || true)" "$(command -v chromium || true)"; do
    [[ -z "$CHROME" && -n "$c" && -x "$c" ]] && CHROME="$c"
  done
  if [[ -z "$CHROME" ]]; then
    echo "Chrome not found; portal HTML is in $BUILD/portal" >&2
    exit 1
  fi
  while IFS=' ' read -r page height; do
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=2 \
      --window-size=560,"$height" --screenshot="$OUT/portal-$page.png" \
      "file://$BUILD/portal/$page.html" >/dev/null 2>&1
    echo "wrote $OUT/portal-$page.png"
  done < "$BUILD/portal/pages.txt"
fi
