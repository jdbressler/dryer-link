#!/bin/bash
# Flash Dryer Link modules through the USB-serial adapter on J2, one after another (macOS or Linux).
#
#   ./flash.sh            the release firmware. Each module is wiped, flashed, given its own setup password,
#                         and gets a label to print in labels/ (Wi-Fi name, password, QR code).
#   ./flash.sh --test     the same with the test build (dryer-link-test.yaml). Not for customers.
#   ./flash.sh --keep     firmware only: the module keeps its Wi-Fi details, what it learned, and its password.
#                         Add --test for the test build.
#
# Wiring, exactly as for any first flash of this board: 3v3, GND, TX to TX, RX to RX; never 5 V.
# The firmware is the same for every module. Only the password differs, and it exists in two places: on the
# module and in its label file. labels/ is not part of the repository; keep it to yourself.
cd "$(dirname "$0")" || exit 1
ESPHOME="${ESPHOME:-$HOME/.local/bin/esphome}"
CONF=dryer-link.yaml
BUILD=.esphome/build/dryer-link
KEEP=""
for arg in "$@"; do
  case "$arg" in
    --test) CONF=dryer-link-test.yaml; BUILD=.esphome/build/dryer-link-test ;;
    --keep) KEEP="--keep" ;;
    *) echo "Unknown option $arg. See the top of this file."; exit 2 ;;
  esac
done
BIN="$BUILD/build/firmware.factory.bin"

if [ ! -x "$ESPHOME" ]; then
  echo "ESPHome was not found at $ESPHOME. Install it, or run: ESPHOME=/path/to/esphome ./flash.sh"; exit 1
fi
if [ ! -f "$BIN" ] || [ -n "$(find dryer-link.yaml "$CONF" components -newer "$BIN" -type f 2>/dev/null | head -1)" ]; then
  echo "Building the firmware first (a few minutes the first time)..."
  "$ESPHOME" compile "$CONF" || { echo "The build failed. Nothing was flashed."; exit 1; }
fi

# esptool comes with ESPHome: run the tool with the Python that ESPHome itself runs on.
PY="$(head -1 "$ESPHOME" | sed 's/^#! *//')"
N=0
while true; do
  printf '\nModule %d: hold SW2, plug the adapter into USB, count to two, release.\nPress Return to flash it, or type q and Return to stop: ' $((N + 1))
  read -r ans
  [ "$ans" = "q" ] && break
  # shellcheck disable=SC2086
  if "$PY" tools/flash_module.py "$BUILD" $KEEP; then
    N=$((N + 1))
    echo "Unplug the adapter and take the header out. Do not run the module from the adapter: the adapter"
    echo "cannot power its Wi-Fi. Plugged into a dryer, the module opens its setup network within half a minute."
  else
    echo "That module did not flash. Check the wiring (swap TX and RX if it never connects) and try it again."
  fi
done
echo "$N module(s) flashed."
if [ "$N" -gt 0 ] && [ -z "$KEEP" ]; then
  echo "Their labels are in the labels folder. Print each one and put it on its module."
  command -v open >/dev/null 2>&1 && open labels
fi
