#!/usr/bin/env bash
# Run the host-side test suites (no board needed).
#
#   ./scripts/run-tests.sh            # all C++ host tests + Python tests
#   ./scripts/run-tests.sh adsb       # only C++ test binaries whose name contains "adsb"
#   ./scripts/run-tests.sh --cpp      # C++ only
#   ./scripts/run-tests.sh --python   # Python only
#
# C++ tests compile firmware sources from src/ against the stubs in
# tests/host/stubs (Arduino, Preferences/NVS, HTTPClient, FreeRTOS) with the
# host compiler, under AddressSanitizer + UBSan. Set NO_SANITIZE=1 to disable.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

run_cpp=1
run_py=1
filter=""
for arg in "$@"; do
  case "$arg" in
    --cpp) run_py=0 ;;
    --python) run_cpp=0 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) filter="$arg" ;;
  esac
done

CXX="${CXX:-c++}"
BUILD_DIR="$ROOT/.pio/host-tests"
JSON_DIR="$ROOT/.pio/libdeps/esp32-8048s050/ArduinoJson/src"
failed=()

if [[ $run_cpp == 1 ]]; then
  if [[ ! -f "$JSON_DIR/ArduinoJson.h" ]]; then
    echo "ArduinoJson not found; installing PlatformIO library deps..."
    PIO="$(command -v pio || echo "$ROOT/.venv/bin/pio")"
    "$PIO" pkg install -e esp32-8048s050
  fi

  mkdir -p "$BUILD_DIR"
  flags=(-std=gnu++17 -g -O1 -Wall -Wextra -Wno-unused-parameter
         -Wno-unused-function -Wno-unused-variable
         -I"$ROOT/tests/host/stubs" -I"$ROOT/tests/host" -I"$ROOT/include"
         -I"$JSON_DIR")
  if [[ "${NO_SANITIZE:-0}" != 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer
            -fno-sanitize-recover=undefined)
  fi

  for src in "$ROOT"/tests/host/test_*.cpp; do
    name="$(basename "$src" .cpp)"
    [[ -n "$filter" && "$name" != *"$filter"* ]] && continue
    echo "==> $name"
    if ! "$CXX" "${flags[@]}" "$src" "$ROOT/tests/host/stubs/stubs.cpp" \
         -o "$BUILD_DIR/$name"; then
      failed+=("$name (compile)")
      continue
    fi
    if ! "$BUILD_DIR/$name"; then
      failed+=("$name")
    fi
  done
fi

if [[ $run_py == 1 && -z "$filter" ]]; then
  echo "==> python tests"
  PY="$(command -v python3)"
  [[ -x "$ROOT/.venv/bin/python3" ]] && PY="$ROOT/.venv/bin/python3"
  if ! "$PY" -m unittest discover -s "$ROOT/tests/python" -t "$ROOT/tests/python" -v; then
    failed+=("python")
  fi
fi

echo
if [[ ${#failed[@]} -gt 0 ]]; then
  echo "FAILED: ${failed[*]}"
  exit 1
fi
echo "All tests passed."
