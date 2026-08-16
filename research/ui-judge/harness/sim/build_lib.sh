#!/bin/sh
# Build LovyanGFX once into a static library the per-sketch builds link against.
#
# Without this every sketch recompiles the same fourteen LovyanGFX translation
# units -- about ninety seconds each, and the corpus is built twice (first-pass
# and as-deployed). The library is identical for every sketch because nothing
# in it depends on the sketch: the only thing a sketch changes is which drawing
# calls it makes.
set -e
cd "$(dirname "$0")"

FW=/c/Users/aaron/dev/lith/firmware
LGFX=$FW/.pio/libdeps/esp32-s3-zero/LovyanGFX/src
QR=$FW/.pio/libdeps/esp32-s3-zero/QRCode/src
OBJ=obj
mkdir -p "$OBJ"

CXXSRC="$LGFX/lgfx/v1/LGFXBase.cpp
        $LGFX/lgfx/v1/LGFX_Sprite.cpp
        $LGFX/lgfx/v1/lgfx_fonts.cpp
        $LGFX/lgfx/v1/misc/pixelcopy.cpp
        $LGFX/lgfx/v1/misc/common_function.cpp
        $LGFX/lgfx/v1/misc/SpriteBuffer.cpp
        $LGFX/lgfx/v1/misc/DividedFrameBuffer.cpp
        $LGFX/lgfx/v1/panel/Panel_Device.cpp
        $LGFX/lgfx/v1/panel/Panel_FrameBufferBase.cpp
        $LGFX/lgfx/v1/platforms/sdl/common.cpp"

CSRC="$LGFX/lgfx/utility/lgfx_qrcode.c
      $LGFX/lgfx/utility/lgfx_tjpgd.c
      $LGFX/lgfx/utility/lgfx_pngle.c
      $LGFX/lgfx/utility/lgfx_qoi.c
      $LGFX/lgfx/utility/lgfx_miniz.c
      $LGFX/lgfx/Fonts/efont/lgfx_efont_cn.c
      $LGFX/lgfx/Fonts/efont/lgfx_efont_ja.c
      $LGFX/lgfx/Fonts/efont/lgfx_efont_kr.c
      $LGFX/lgfx/Fonts/efont/lgfx_efont_tw.c
      $LGFX/lgfx/Fonts/IPA/lgfx_font_japan.c
      $QR/qrcode.c"

for s in $CXXSRC; do
  o="$OBJ/$(basename "$s" .cpp).o"
  [ -f "$o" ] && continue
  echo "cc $(basename "$s")"
  g++ -std=gnu++17 -O2 -w -c -o "$o" -DLITH_SIM=1 \
      -I . -I stub -I "$LGFX" -I "$QR" "$s"
done
for s in $CSRC; do
  o="$OBJ/$(basename "$s" .c).o"
  [ -f "$o" ] && continue
  echo "cc $(basename "$s")"
  gcc -O2 -w -c -o "$o" -I . -I stub -I "$LGFX" -I "$QR" "$s"
done

ar rcs liblgfx_sim.a "$OBJ"/*.o
echo "built $(pwd)/liblgfx_sim.a"
