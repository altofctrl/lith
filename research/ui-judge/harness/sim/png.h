// PNG writer lifted verbatim from lith/tools/sim/lith_screen.cpp, so frames
// from this harness and from the stock-firmware simulator are byte-identical
// in encoding as well as in pixels.
#pragma once
#include <cstdio>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------- png out
// A minimal writer: deflate "stored" blocks, so no zlib. The panel is 54,400
// pixels, and these files exist to be pulled into Blender as textures, so the
// few hundred KB costs nothing and the dependency-free build is worth more.
namespace png {

static uint32_t crc_table[256];
static bool     crc_ready = false;

static void crc_init() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
  crc_ready = true;
}

static uint32_t crc(const uint8_t *buf, size_t len, uint32_t c = 0xFFFFFFFFu) {
  if (!crc_ready) crc_init();
  for (size_t i = 0; i < len; i++) c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
  return c;
}

static void be32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

static void chunk(FILE *f, const char *tag, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> hdr;
  be32(hdr, (uint32_t)data.size());
  fwrite(hdr.data(), 1, 4, f);
  fwrite(tag, 1, 4, f);
  if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
  uint32_t c = crc((const uint8_t *)tag, 4);
  c = crc(data.data(), data.size(), c) ^ 0xFFFFFFFFu;
  std::vector<uint8_t> tail;
  be32(tail, c);
  fwrite(tail.data(), 1, 4, f);
}

static uint32_t adler32(const uint8_t *d, size_t n) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
  return (b << 16) | a;
}

// rgb is w*h*3, top-down
static bool write(const char *path, const uint8_t *rgb, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
  fwrite(sig, 1, 8, f);

  std::vector<uint8_t> ihdr;
  be32(ihdr, (uint32_t)w);
  be32(ihdr, (uint32_t)h);
  ihdr.push_back(8);            // bit depth
  ihdr.push_back(2);            // colour type: truecolour
  ihdr.push_back(0);            // deflate
  ihdr.push_back(0);            // adaptive filtering
  ihdr.push_back(0);            // no interlace
  chunk(f, "IHDR", ihdr);

  // raw scanlines, each prefixed with filter type 0
  std::vector<uint8_t> raw;
  raw.reserve((size_t)h * (1 + (size_t)w * 3));
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
  }

  std::vector<uint8_t> z;
  z.push_back(0x78); z.push_back(0x01);           // zlib header, no compression
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t n    = raw.size() - pos;
    if (n > 65535) n = 65535;
    bool   last = (pos + n) >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(n & 0xFF); z.push_back((n >> 8) & 0xFF);
    z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  be32(z, adler32(raw.data(), raw.size()));
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  fclose(f);
  return true;
}

}  // namespace png

