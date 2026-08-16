// knapp_sim -- Gate 2 and Gate 3 of the UI-judge microstudy.
//
// Compiles one Oldowan-generated sketch against the host shims, runs its real
// setup()/loop() on a virtual clock, drives it down the canonical six-state
// journey with a scripted input sequence, and writes the panel out as a PNG at
// each state.
//
//   knapp_sim <out_dir> <build_id>
//
// The sketch itself arrives by -include on the command line, so this file never
// names it and the same binary source builds every one of them.
//
// Two things this deliberately does NOT do. It does not ask a model to describe
// or re-implement the UI -- that would put a second LLM inside the measurement
// path. And it does not read each build's own idea of what its screens are: the
// journey below comes from the user brief, so every build is scored on the same
// route rather than one it chose for itself.

#include "sim_arduino.h"

// ------------------------------------------------------------- sim globals
uint32_t g_sim_millis = 0;
uint8_t  g_pin[SIM_NPINS];
void   (*g_isr[SIM_NPINS])();
uint8_t  g_isr_mode[SIM_NPINS];
uint32_t g_motor_writes = 0;
uint32_t g_motor_last_duty = 0;

namespace lgfx { inline namespace v1 {
uint8_t g_sim_rotation = 0;
bool    g_sim_rotation_set = false;
bool    g_sim_init_called = false;
}}

#include "LovyanGFX.hpp"
#include "png.h"

#include <vector>
#include <string>
#include <set>
#include <ctime>

// Filled by Panel_FB's constructor. Every generated sketch declares one panel
// from the display_init snippet; if a sketch somehow declares several, the one
// that ends up with a buffer is the one it initialised and drew into.
static std::vector<lgfx::Panel_FB *> g_panels;
namespace lgfx { inline namespace v1 {
void sim_register_panel(Panel_FB *p) { g_panels.push_back(p); }
}}

// Pin numbers from device_profile.json. Named apart from the sketch's own
// PIN_SW1 / PIN_SW2 macros, which it is free to spell however it likes.
#define SIM_SW1   1
#define SIM_SW2   2
#define SIM_ENC_A 4
#define SIM_ENC_B 5

// ------------------------------------------------------------------ inputs
void sim_advance(uint32_t ms);

void sim_set_pin(int p, int level) {
  if (p < 0 || p >= SIM_NPINS) return;
  int prev = g_pin[p];
  if (prev == level) return;
  g_pin[p] = (uint8_t)level;
  if (!g_isr[p]) return;
  int mode = g_isr_mode[p];
  bool rise = (prev == LOW && level == HIGH);
  bool fall = (prev == HIGH && level == LOW);
  if (mode == CHANGE || (mode == RISING && rise) || (mode == FALLING && fall))
    g_isr[p]();
}

// --------------------------------------------------------------- the sketch
// Declared before the include so the harness can call them; the sketch's own
// definitions satisfy these.
void setup();
void loop();

#include SKETCH_FILE

// ----------------------------------------------------------------- driving
// Time only moves here. Steps are coarse when nothing is about to be captured
// and fine when something is -- a debounce of ~25 ms and a 30 fps redraw both
// need to be resolvable, but stepping 90 minutes at 25 ms would be 216,000
// loops of full-screen rendering for no gain.
static unsigned long long g_loops = 0;
static const unsigned long long LOOP_BUDGET = 2000000ULL;
static bool g_in_loop = false;

void sim_advance(uint32_t ms) {
  // delay() from inside loop() must not re-enter loop(): just move the clock.
  if (g_in_loop) { g_sim_millis += ms; return; }
  g_sim_millis += ms;
}

// Wall-clock ceiling, so one pathological build cannot stall the batch. A
// sketch that blocks *inside* loop() is beyond reach from here and is caught by
// the runner's own timeout instead.
static clock_t g_t0;
static bool g_out_of_time = false;
static const double WALL_BUDGET_S = 240.0;

static bool budget_left() {
  if (g_out_of_time) return false;
  if (g_loops % 256 == 0 &&
      (double)(clock() - g_t0) / CLOCKS_PER_SEC > WALL_BUDGET_S) {
    g_out_of_time = true;
    fprintf(stderr, "[sim] wall budget exhausted at t=%u ms\n", g_sim_millis);
    return false;
  }
  return true;
}

static void run_until(uint32_t target_ms, uint32_t step) {
  while (g_sim_millis < target_ms && g_loops < LOOP_BUDGET && budget_left()) {
    uint32_t before = g_sim_millis;
    g_in_loop = true;
    loop();
    g_in_loop = false;
    g_loops++;
    // A sketch that delays inside loop() has already moved the clock; only add
    // the harness step for what is left, so its own pacing is preserved.
    uint32_t used = g_sim_millis - before;
    if (used < step) g_sim_millis = before + step;
  }
}

// Three step sizes, coarse to fine. The coarse step is one second and not more:
// a cost meter that accrues on a `now - last >= 1000` tick advances once per
// loop, so stepping in whole seconds keeps the accrued total right, while a
// longer stride would silently under-count the money on screen -- the very
// number the build is being judged on. The last half-second is stepped at 20 ms
// so the frame that gets captured is one the sketch would really have drawn,
// with its animations at the right phase.
static void advance_to(uint32_t target_ms) {
  if (target_ms > 4000) run_until(target_ms - 3000, 1000);
  if (target_ms > 500)  run_until(target_ms - 500, 100);
  run_until(target_ms, 20);
}

static void tap(int pin, uint32_t hold_ms = 60) {
  sim_set_pin(pin, LOW);                       // active low, INPUT_PULLUP
  run_until(g_sim_millis + hold_ms, 10);
  sim_set_pin(pin, HIGH);
  run_until(g_sim_millis + 120, 10);           // let debounce settle
}

// ------------------------------------------------------------------ capture
static lgfx::Panel_FB *sim_find_panel() {
  for (auto *p : g_panels)
    if (p->has_buffer()) return p;
  return nullptr;
}

struct Frame {
  bool present = false;
  std::string hash;
  double ink = 0.0;      // fraction of pixels that are not the modal colour
  int w = 0, h = 0;
};

static std::string frame_hash(const uint8_t *rgb, size_t n) {
  // FNV-1a, enough to tell two screens apart, which is all coverage needs.
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) { h ^= rgb[i]; h *= 1099511628211ULL; }
  char buf[24];
  snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
  return buf;
}

static Frame capture(const std::string &path) {
  Frame f;
  lgfx::Panel_FB *pan = sim_find_panel();
  if (!pan) return f;

  // Read in the panel's *logical* (rotated) coordinates, so the image is the
  // one the owner is looking at. The rotation applied is the sketch's own --
  // its setRotation combined with the offset_rotation it put in its own panel
  // config -- because this is the real Panel_Device rotation path, not a
  // reimplementation of it. A build that never calls setRotation therefore
  // comes out portrait, which is exactly what would happen on the device.
  const int w = pan->width(), h = pan->height();
  if (w <= 0 || h <= 0) return f;
  f.w = w; f.h = h;

  std::vector<uint8_t> rgb((size_t)w * h * 3);
  std::vector<uint16_t> row(w);
  for (int y = 0; y < h; y++) {
    // Same call LGFXBase::readPixel makes, a row at a time.
    lgfx::pixelcopy_t p(nullptr, lgfx::swap565_t::depth, pan->read_depth(),
                        false, nullptr);   // 16bpp: no palette
    pan->readRect(0, y, w, 1, row.data(), &p);
    for (int x = 0; x < w; x++) {
      uint16_t v = row[x];
      uint16_t c = (uint16_t)((v << 8) | (v >> 8));   // swap565 -> rgb565
      uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
      size_t i = ((size_t)y * w + x) * 3;
      // Replicate the high bits into the low ones, which is what the panel does.
      rgb[i + 0] = (uint8_t)((r << 3) | (r >> 2));
      rgb[i + 1] = (uint8_t)((g << 2) | (g >> 4));
      rgb[i + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
  }

  // Ink fraction against the modal colour: a screen that is one flat colour has
  // drawn nothing, and that is worth catching before a judge ever sees it.
  size_t counts_max = 0;
  {
    std::map<uint32_t, size_t> hist;
    for (size_t i = 0; i < (size_t)w * h; i++) {
      uint32_t c = (rgb[i*3] << 16) | (rgb[i*3+1] << 8) | rgb[i*3+2];
      size_t n = ++hist[c];
      if (n > counts_max) counts_max = n;
    }
  }
  f.ink = 1.0 - (double)counts_max / (double)(w * h);
  f.hash = frame_hash(rgb.data(), rgb.size());
  f.present = png::write(path.c_str(), rgb.data(), w, h);
  return f;
}

// -------------------------------------------------------------------- main
// The canonical journey, from the brief rather than from any build's output.
static const char *STATE_NAME[6] = {
  "1_boot_idle", "2_started", "3_mid_meeting",
  "4_threshold", "5_extended", "6_stopped",
};

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: knapp_sim <out_dir> <build_id>\n");
    return 2;
  }
  const std::string dir = argv[1];
  const std::string id  = argv[2];

  g_t0 = clock();
  for (int i = 0; i < SIM_NPINS; i++) { g_pin[i] = HIGH; g_isr[i] = nullptr; }

  // Traced because a build that hangs does so somewhere specific, and with 19
  // of them the difference between "hung in setup" and "hung in loop" is the
  // difference between a shim gap and a defect in the sketch.
  fprintf(stderr, "[sim] setup()\n");
  setup();
  fprintf(stderr, "[sim] setup done, t=%u\n", g_sim_millis);

  Frame frames[6];

  advance_to(2000);                    // 1. boot / idle
  fprintf(stderr, "[sim] state 1 at t=%u loops=%llu\n", g_sim_millis, (unsigned long long)g_loops);
  frames[0] = capture(dir + "/" + id + "__" + STATE_NAME[0] + ".png");

  tap(SIM_SW1);                    // meeting starts
  advance_to(6000);                    // 2. first seconds
  fprintf(stderr, "[sim] state 2 at t=%u loops=%llu\n", g_sim_millis, (unsigned long long)g_loops);
  frames[1] = capture(dir + "/" + id + "__" + STATE_NAME[1] + ".png");

  advance_to(5u * 60u * 1000u);        // 3. mid-meeting, cost accumulating
  fprintf(stderr, "[sim] state 3 at t=%u loops=%llu\n", g_sim_millis, (unsigned long long)g_loops);
  frames[2] = capture(dir + "/" + id + "__" + STATE_NAME[2] + ".png");

  advance_to(30u * 60u * 1000u);       // 4. past any plausible threshold
  fprintf(stderr, "[sim] state 4 at t=%u loops=%llu\n", g_sim_millis, (unsigned long long)g_loops);
  frames[3] = capture(dir + "/" + id + "__" + STATE_NAME[3] + ".png");

  advance_to(90u * 60u * 1000u);       // 5. extended run, high cost
  fprintf(stderr, "[sim] state 5 at t=%u loops=%llu\n", g_sim_millis, (unsigned long long)g_loops);
  frames[4] = capture(dir + "/" + id + "__" + STATE_NAME[4] + ".png");

  tap(SIM_SW1);                    // meeting stops
  advance_to(g_sim_millis + 4000);     // 6. stopped
  fprintf(stderr, "[sim] state 6 at t=%u loops=%llu\n", g_sim_millis, (unsigned long long)g_loops);
  frames[5] = capture(dir + "/" + id + "__" + STATE_NAME[5] + ".png");

  // Coverage: how many of the six states the build actually distinguishes. A
  // build that only ever shows one screen has failed the brief however well
  // that screen scores, so identical frames collapse to one covered state.
  std::set<std::string> distinct;
  for (auto &f : frames) if (f.present) distinct.insert(f.hash);

  printf("build_id\t%s\n", id.c_str());
  printf("init_called\t%d\n", (int)lgfx::g_sim_init_called);
  printf("rotation_set\t%d\n", (int)lgfx::g_sim_rotation_set);
  printf("rotation\t%d\n", (int)lgfx::g_sim_rotation);
  printf("panel_w\t%d\n", frames[0].w);
  printf("panel_h\t%d\n", frames[0].h);
  printf("motor_writes\t%u\n", g_motor_writes);
  printf("loops\t%llu\n", (unsigned long long)g_loops);
  printf("timed_out\t%d\n", (int)g_out_of_time);
  printf("distinct_frames\t%d\n", (int)distinct.size());
  for (int i = 0; i < 6; i++)
    printf("frame\t%s\t%d\t%s\t%.4f\n", STATE_NAME[i], (int)frames[i].present,
           frames[i].hash.c_str(), frames[i].ink);
  return 0;
}
