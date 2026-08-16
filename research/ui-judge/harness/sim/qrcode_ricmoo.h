// The build server installs ricmoo's QRCode library under this name, because
// the ESP32 Arduino core ships an unrelated `qrcode.h` of its own that would
// otherwise shadow it (see device_profile.json's libraries_available note). The
// library's own header is still `qrcode.h`, so this is the same indirection the
// build server applies, done here so a sketch that follows the profile's
// instruction compiles against the real encoder.
#pragma once
#include "qrcode.h"
