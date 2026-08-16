#!/bin/sh
# Build the journey harness around one generated sketch.
#
#   ./build_one.sh <sketch.ino> <out.exe>
#
# The sketch is fed in by -DSKETCH_FILE so knapp_sim.cpp never names it, and the
# shim directory is FIRST on the include path so the sketch's own
# `#include <LovyanGFX.hpp>` resolves to the RAM-backed shim rather than the
# real driver, which would try to talk to SPI that is not here.
#
# LovyanGFX itself comes from liblgfx_sim.a (build_lib.sh), so only this one
# translation unit is compiled per sketch.
set -e
cd "$(dirname "$0")"

SKETCH="$1"
OUT="$2"

FW=/c/Users/aaron/dev/lith/firmware
LGFX=$FW/.pio/libdeps/esp32-s3-zero/LovyanGFX/src
QR=$FW/.pio/libdeps/esp32-s3-zero/QRCode/src

[ -f liblgfx_sim.a ] || ./build_lib.sh >&2

# -fpermissive because these sketches were written for the Arduino toolchain and
# a few lean on its laxer conversions; this is Gate 2 (does it render), not
# Gate 1 (does it build for the device), which arduino-cli already judged.
g++ -std=gnu++17 -O2 -w -fpermissive -o "$OUT" \
    -DLITH_SIM=1 -DSKETCH_FILE="\"$SKETCH\"" \
    -I . -I stub -I "$LGFX" -I "$QR" \
    knapp_sim.cpp liblgfx_sim.a -lstdc++
