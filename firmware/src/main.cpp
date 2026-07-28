// =====================================================================
// Lith pomodoro timer — ESP32-S3-Zero
//   ST7789V2 170x320 display (LovyanGFX) + rotary encoder + 2 switches
//   + vibration motor on an NPN low-side switch
//
// CONTROLS
//   Encoder           -> set the length shown on screen (when idle)
//   SW1 short press   -> start / pause / resume
//   SW1 long  press   -> reset current phase back to full
//   SW2 short press   -> skip to next phase
//   SW2 long  press   -> cycle which setting the encoder edits
//
// SERIAL COMMANDS (115200)
//   s start/pause   r reset   n next phase   f field   + / - adjust   ? help
// =====================================================================

#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>

// ------------------------- pin map -----------------------------------
static constexpr int PIN_TFT_SCLK = 12;
static constexpr int PIN_TFT_MOSI = 11;
static constexpr int PIN_TFT_DC   = 13;
static constexpr int PIN_TFT_CS   = 10;
static constexpr int PIN_TFT_RST  =  9;
static constexpr int PIN_TFT_BLK  =  8;

static constexpr int PIN_SW1      =  1;
static constexpr int PIN_SW2      =  2;
static constexpr int PIN_ENC_A    =  4;
static constexpr int PIN_ENC_B    =  5;
static constexpr int PIN_MOTOR    =  6;

// ------------------------- tuning ------------------------------------
static constexpr int      ENC_STEPS_PER_DETENT = 4;
static constexpr uint32_t MOTOR_PWM_FREQ = 20000;
static constexpr uint8_t  MOTOR_PWM_BITS = 8;
static constexpr int      MOTOR_LEDC_CH  = 0;
static constexpr uint8_t  HAPTIC_DUTY    = 190;
static constexpr uint32_t DEBOUNCE_MS    = 25;
static constexpr uint32_t LONGPRESS_MS   = 800;

static constexpr int ROUNDS_PER_LONG = 4;
static constexpr int MIN_LEN         = 1;
static constexpr int MAX_LEN         = 90;

// ------------------------- layout ------------------------------------
static constexpr int SCR_W     = 320;
static constexpr int SCR_H     = 170;
static constexpr int MARGIN    = 18;
static constexpr int CONTENT_W = SCR_W - 2 * MARGIN;

static constexpr int LABEL_Y   = 24;
static constexpr int LABEL_SP  = 3;    // extra letter spacing
static constexpr int TIME_Y    = 86;
static constexpr int DOT_Y     = 152;
static constexpr int DOT_GAP   = 16;

// ------------------------- display driver ----------------------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 50000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = PIN_TFT_SCLK;
      cfg.pin_mosi    = PIN_TFT_MOSI;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = PIN_TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs          = PIN_TFT_CS;
      cfg.pin_rst         = PIN_TFT_RST;
      cfg.pin_busy        = -1;
      cfg.panel_width     = 170;
      cfg.panel_height    = 320;
      cfg.offset_x        = 35;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 0;
      cfg.readable        = false;
      cfg.invert          = true;
      cfg.rgb_order       = false;
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = PIN_TFT_BLK;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX        tft;
LGFX_Sprite frame(&tft);

bool displayOK      = false;
bool useFrameBuffer = false;
bool bufSwapped     = false;   // probed at boot, see spriteProbe()

// ------------------------- palette -----------------------------------
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static const uint16_t COL_BG       = RGB565(  9, 10, 12);
static const uint16_t COL_INK      = RGB565(238,240,245);
static const uint16_t COL_INK_DEEP = RGB565( 14, 12, 16);

struct Rgb { uint8_t r, g, b; };
static const Rgb PH_RGB[3] = {{255,138, 61}, {74,205,150}, {74,205,150}};

// ------------------------- encoder -----------------------------------
static const int8_t QUAD[16] = { 0, -1,  1,  0,
                                 1,  0,  0, -1,
                                -1,  0,  0,  1,
                                 0,  1, -1,  0 };
volatile uint8_t  encState = 0;
volatile int32_t  encRaw   = 0;
int32_t encDetentLast = 0;

void IRAM_ATTR encISR() {
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  encState = ((encState << 2) | (a << 1) | b) & 0x0F;
  encRaw  += QUAD[encState];
}

// ------------------------- buttons -----------------------------------
static const uint8_t EV_NONE  = 0;
static const uint8_t EV_SHORT = 1;
static const uint8_t EV_LONG  = 2;

static const int BTN_COUNT = 2;
static const int BTN1 = 0;
static const int BTN2 = 1;

struct Button {
  int      pin;
  bool     level;
  bool     rawLast;
  uint32_t tChange;
  uint32_t tPress;
  bool     longFired;
};

Button btns[BTN_COUNT] = {
  {PIN_SW1, false, false, 0, 0, false},
  {PIN_SW2, false, false, 0, 0, false}
};

uint8_t pollButton(int idx) {
  Button &b = btns[idx];
  uint32_t now = millis();
  bool raw = (digitalRead(b.pin) == LOW);
  if (raw != b.rawLast) { b.rawLast = raw; b.tChange = now; }

  uint8_t ev = EV_NONE;
  if ((now - b.tChange) >= DEBOUNCE_MS && raw != b.level) {
    b.level = raw;
    if (b.level) {
      b.tPress    = now;
      b.longFired = false;
    } else if (!b.longFired) {
      ev = EV_SHORT;
    }
  }
  if (b.level && !b.longFired && (now - b.tPress) >= LONGPRESS_MS) {
    b.longFired = true;
    ev = EV_LONG;
  }
  return ev;
}

// ------------------------- motor -------------------------------------
void motorRaw(uint8_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR, duty);
#else
  ledcWrite(MOTOR_LEDC_CH, duty);
#endif
}

void motorInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_MOTOR, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
#else
  ledcSetup(MOTOR_LEDC_CH, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
  ledcAttachPin(PIN_MOTOR, MOTOR_LEDC_CH);
#endif
  motorRaw(0);
}

// ------------------------- haptics -----------------------------------
// on/off durations in ms, alternating, zero terminates
static const uint16_t PAT_TICK[]   = {14, 0};
static const uint16_t PAT_START[]  = {45, 60, 45, 0};
static const uint16_t PAT_BREAK[]  = {200, 130, 200, 0};
static const uint16_t PAT_LONG[]   = {200, 130, 200, 130, 320, 0};
static const uint16_t PAT_BACK[]   = {320, 0};

const uint16_t *hapSeq = nullptr;
uint8_t  hapIdx  = 0;
uint32_t hapNext = 0;

void hapticPlay(const uint16_t *seq) {
  hapSeq  = seq;
  hapIdx  = 0;
  hapNext = millis();
}

void hapticService() {
  if (hapSeq == nullptr) return;
  uint32_t now = millis();
  if ((int32_t)(now - hapNext) < 0) return;

  uint16_t d = hapSeq[hapIdx];
  if (d == 0) {
    hapSeq = nullptr;
    motorRaw(0);
    return;
  }
  motorRaw((hapIdx % 2) == 0 ? HAPTIC_DUTY : 0);
  hapNext = now + d;
  hapIdx++;
}

// ------------------------- timer state -------------------------------
static const uint8_t PH_FOCUS = 0;
static const uint8_t PH_SHORT = 1;
static const uint8_t PH_LONG  = 2;

int lenMin[3] = {25, 5, 15};
const char *phaseName[3] = {"FOCUS", "BREAK", "LONG BREAK"};

uint8_t  phase         = PH_FOCUS;
bool     running       = false;
uint32_t phaseRemainMs = 0;
uint32_t lastTickMs    = 0;
int      focusDone     = 0;
uint8_t  editField     = PH_FOCUS;

uint32_t phaseLenMs() { return (uint32_t)lenMin[phase] * 60000UL; }

void loadPhase() {
  phaseRemainMs = phaseLenMs();
  lastTickMs    = millis();
}

void advance(bool byUser) {
  if (phase == PH_FOCUS) {
    if (!byUser) focusDone++;
    bool longBreak = (focusDone > 0) && (focusDone % ROUNDS_PER_LONG == 0);
    phase   = longBreak ? PH_LONG : PH_SHORT;
    running = true;
    hapticPlay(longBreak ? PAT_LONG : PAT_BREAK);
  } else {
    phase   = PH_FOCUS;
    running = false;
    hapticPlay(PAT_BACK);
  }
  loadPhase();
}

void toggleRun() {
  running = !running;
  lastTickMs = millis();
  hapticPlay(running ? PAT_START : PAT_TICK);
}

void resetPhase() {
  running = false;
  loadPhase();
  hapticPlay(PAT_TICK);
}

void timerService() {
  if (!running) { lastTickMs = millis(); return; }
  uint32_t now = millis();
  uint32_t dt  = now - lastTickMs;
  lastTickMs = now;

  if (dt >= phaseRemainMs) {
    phaseRemainMs = 0;
    advance(false);
  } else {
    phaseRemainMs -= dt;
  }
}

// ------------------------- render ------------------------------------
int spacedWidth(LovyanGFX *g, const char *s, int extra) {
  int n = strlen(s);
  return g->textWidth(s) + (n > 1 ? extra * (n - 1) : 0);
}

void drawSpaced(LovyanGFX *g, const char *s, int cx, int y, int extra) {
  int x = cx - spacedWidth(g, s, extra) / 2;
  char c[2] = {0, 0};
  g->setTextDatum(textdatum_t::top_left);
  for (int i = 0; s[i]; i++) {
    c[0] = s[i];
    g->drawString(c, x, y);
    x += g->textWidth(c) + extra;
  }
}

// ------------------------- fluid -------------------------------------
// Metaball field sampled on a coarse grid, then bilinearly upsampled to
// pixels. The liquid surface is itself a field source, so blobs drifting
// near it stretch and merge into the body rather than sitting on top of
// it as hard circles. Level is time remaining, so the vessel drains.
static constexpr int CELL  = 5;
static constexpr int FW    = SCR_W / CELL + 2;
static constexpr int FH    = SCR_H / CELL + 2;
static constexpr int NBLOB = 5;

static constexpr float FIELD_T = 1.00f;   // at or above this is inside
static constexpr float RIM_T   = 1.70f;   // brighter band just inside
static constexpr float SURF_K  = 44.0f;   // reach of the surface, px^2

// Depth shading across the body. RGB565 carries 5 bits of red and blue but
// 6 of green, so an amber ramp steps twice as coarsely as a teal one and
// bands about twice as hard. A 4x4 ordered dither breaks the contours into
// a stipple the panel is too dense to resolve, which costs one AND in the
// inner loop and buys back the gradient. Set to 0 for a flat block colour.
static constexpr float DEPTH_FADE = 0.22f;

static const uint8_t BAYER4[4][4] = {
  { 0,  8,  2, 10},
  {12,  4, 14,  6},
  { 3, 11,  1,  9},
  {15,  7, 13,  5},
};

// LovyanGFX keeps 16bpp sprite pixels in SPI wire order, which is byte
// swapped relative to the RGB565 literals above. Writing the buffer raw
// bypasses its colour conversion, so every constant we poke in or compare
// against has to be put through here first.
static inline uint16_t store(uint16_t c) {
  return bufSwapped ? (uint16_t)((c >> 8) | (c << 8)) : c;
}

struct Blob { float cx, rel, ax, ay, fx, fy, ph, r; };

// lissajous drift: cheaper than a physics step and never quite repeats
static Blob blobs[NBLOB] = {
  { 66, -18, 44, 22, 0.23f, 0.31f, 0.0f, 30},
  {158,  14, 60, 30, 0.17f, 0.26f, 1.7f, 26},
  {252,  -6, 42, 26, 0.29f, 0.19f, 3.1f, 28},
  {112,  40, 52, 20, 0.13f, 0.37f, 4.4f, 22},
  {214,  30, 48, 24, 0.21f, 0.23f, 5.6f, 24},
};

static float    fld[FH][FW];
static float    surf[FW];
static float    bX[NBLOB], bY[NBLOB], bR2[NBLOB];
static uint16_t bodyDith[SCR_H][4];   // depth ramp, one entry per x phase

void fluidStep(float t, float level, bool ambient) {
  float base = SCR_H * (1.0f - level);
  for (int i = 0; i < FW; i++) {
    float x = i * CELL;
    surf[i] = base + 5.0f * sinf(x * 0.028f + t * 1.10f)
                   + 3.0f * sinf(x * 0.017f - t * 0.73f);
  }

  // idle lets the blobs roam mid-screen; running tethers them to the
  // surface so the whole mass settles as the level drops
  float anchor = ambient ? SCR_H * 0.52f : base;
  for (int i = 0; i < NBLOB; i++) {
    Blob &b = blobs[i];
    bX[i]  = b.cx + b.ax * sinf(t * b.fx + b.ph);
    bY[i]  = anchor + b.rel + b.ay * sinf(t * b.fy + b.ph * 1.3f);
    bR2[i] = b.r * b.r;
  }

  for (int gy = 0; gy < FH; gy++) {
    float py = gy * CELL;
    for (int gx = 0; gx < FW; gx++) {
      float d = py - surf[gx];
      if (d >= 0.0f) { fld[gy][gx] = 99.0f; continue; }   // below the line
      float px = gx * CELL;
      float f  = SURF_K / (d * d);
      for (int i = 0; i < NBLOB; i++) {
        float dx = px - bX[i], dy = py - bY[i];
        float d2 = dx * dx + dy * dy;
        if (d2 < 1.0f) d2 = 1.0f;
        f += bR2[i] / d2;
      }
      fld[gy][gx] = f;
    }
  }
}

// Chrome is already in the buffer as COL_INK. Where liquid covers it we
// swap to COL_INK_DEEP, so the readout flips light-on-dark to dark-on-lit
// and stays legible at any level. The bitmap fonts are not antialiased,
// so an exact colour compare is a sound test for "this pixel is text".
void fluidRaster(uint16_t *fb, const Rgb &c) {
  int rr = c.r + 60, gg = c.g + 60, bb = c.b + 55;
  if (rr > 255) rr = 255;
  if (gg > 255) gg = 255;
  if (bb > 255) bb = 255;
  const uint16_t rim    = store(RGB565(rr, gg, bb));
  const uint16_t inkOn  = store(COL_INK);
  const uint16_t inkOff = store(COL_INK_DEEP);

  // Nudge each channel by up to one quantisation step before it truncates:
  // red and blue span 8 codes per step, green 4.
  for (int y = 0; y < SCR_H; y++) {
    float k  = 1.0f - DEPTH_FADE * ((float)y / SCR_H);
    float rf = c.r * k, gf = c.g * k, bf = c.b * k;
    const uint8_t *brow = BAYER4[y & 3];
    for (int p = 0; p < 4; p++) {
      int d  = brow[p];
      int rq = (int)(rf + d * 0.50f);
      int gq = (int)(gf + d * 0.25f);
      int bq = (int)(bf + d * 0.50f);
      if (rq > 255) rq = 255;
      if (gq > 255) gq = 255;
      if (bq > 255) bq = 255;
      bodyDith[y][p] = store(RGB565(rq, gq, bq));
    }
  }

  // Most blocks are wholly inside or wholly outside; only the contour needs
  // interpolating. Testing the four corners first skips the float work for
  // the bulk of the screen, which is what keeps this inside a frame budget.
  const float INV_CELL = 1.0f / CELL;

  for (int gy = 0; gy < FH - 1; gy++) {
    int y0 = gy * CELL;
    if (y0 >= SCR_H) break;
    int rows = (y0 + CELL > SCR_H) ? SCR_H - y0 : CELL;

    for (int gx = 0; gx < FW - 1; gx++) {
      int x0 = gx * CELL;
      if (x0 >= SCR_W) break;
      int n = (x0 + CELL > SCR_W) ? SCR_W - x0 : CELL;

      float f00 = fld[gy][gx],     f10 = fld[gy][gx + 1];
      float f01 = fld[gy + 1][gx], f11 = fld[gy + 1][gx + 1];

      float lo = f00, hi = f00;
      if (f10 < lo) lo = f10;  if (f10 > hi) hi = f10;
      if (f01 < lo) lo = f01;  if (f01 > hi) hi = f01;
      if (f11 < lo) lo = f11;  if (f11 > hi) hi = f11;

      if (hi < FIELD_T) continue;                 // wholly background

      if (lo >= RIM_T) {                          // wholly body
        for (int sy = 0; sy < rows; sy++) {
          uint16_t       *row = &fb[(y0 + sy) * SCR_W + x0];
          const uint16_t *bod = bodyDith[y0 + sy];
          for (int sx = 0; sx < n; sx++)
            row[sx] = (row[sx] == inkOn) ? inkOff : bod[(x0 + sx) & 3];
        }
        continue;
      }

      for (int sy = 0; sy < rows; sy++) {         // contour block
        int   y  = y0 + sy;
        float ty = sy * INV_CELL;
        float a  = f00 + (f01 - f00) * ty;
        float b  = f10 + (f11 - f10) * ty;
        float st = (b - a) * INV_CELL;
        float v  = a;
        uint16_t       *row = &fb[y * SCR_W + x0];
        const uint16_t *bod = bodyDith[y];
        for (int sx = 0; sx < n; sx++) {
          if (v >= FIELD_T) {
            row[sx] = (row[sx] == inkOn) ? inkOff
                    : (v < RIM_T ? rim : bod[(x0 + sx) & 3]);
          }
          v += st;
        }
      }
    }
  }
}

// Idle shows the setting the encoder is editing, so turning the knob moves
// the big number directly. Running shows the countdown. One number, always
// the thing you are acting on.
void drawChrome(LovyanGFX *g, uint8_t shown, uint32_t secs) {
  char buf[16];

  g->setFont(&fonts::Font2);
  g->setTextColor(COL_INK, COL_BG);
  drawSpaced(g, phaseName[shown], SCR_W / 2, LABEL_Y, LABEL_SP);

  snprintf(buf, sizeof(buf), "%02lu:%02lu",
           (unsigned long)(secs / 60), (unsigned long)(secs % 60));
  g->setFont(&fonts::Font8);
  g->setTextColor(COL_INK, COL_BG);
  g->setTextDatum(textdatum_t::middle_center);
  g->drawString(buf, SCR_W / 2, TIME_Y);

  int ring = focusDone % ROUNDS_PER_LONG;
  if (focusDone > 0 && ring == 0) ring = ROUNDS_PER_LONG;
  int x0 = SCR_W / 2 - ((ROUNDS_PER_LONG - 1) * DOT_GAP) / 2;
  for (int i = 0; i < ROUNDS_PER_LONG; i++) {
    int cx = x0 + i * DOT_GAP;
    if (i < ring) g->fillCircle(cx, DOT_Y, 3, COL_INK);
    else          g->drawCircle(cx, DOT_Y, 3, COL_INK);
  }
}

void render() {
  bool     fresh = !running && phaseRemainMs == phaseLenMs();
  uint8_t  shown = fresh ? editField : phase;
  float    t     = millis() * 0.001f;
  uint32_t total = phaseLenMs();

  uint32_t secs  = fresh ? (uint32_t)lenMin[editField] * 60
                         : (phaseRemainMs + 999) / 1000;
  float    level = fresh ? 0.17f + 0.05f * sinf(t * 0.6f)
                         : (total ? (float)phaseRemainMs / (float)total : 0.0f);

  if (!useFrameBuffer) {
    tft.fillScreen(COL_BG);
    drawChrome(&tft, shown, secs);
    return;
  }

  frame.fillScreen(COL_BG);
  drawChrome(&frame, shown, secs);
  fluidStep(t, level, fresh);
  fluidRaster((uint16_t *)frame.getBuffer(), PH_RGB[shown]);
  frame.pushSprite(0, 0);
}

// ------------------------- layout audit ------------------------------
// Fixed strings clip silently when they overrun the panel, and a 1.9"
// screen is a poor place to notice 4 missing pixels. Measure them once
// at boot and report over serial instead of eyeballing the result.
struct TextFit {
  const char        *text;
  const lgfx::IFont *font;
  int                spacing;
  int                maxW;
  const char        *tag;
};

static const TextFit LAYOUT_CHECKS[] = {
  {"LONG BREAK", &fonts::Font2, LABEL_SP, CONTENT_W, "phase label"},
  {"88:88",      &fonts::Font8, 0,        CONTENT_W, "countdown"},
};

void layoutAudit() {
  if (!displayOK) return;
  LovyanGFX *g = useFrameBuffer ? (LovyanGFX *)&frame : (LovyanGFX *)&tft;
  int bad = 0;
  for (const TextFit &t : LAYOUT_CHECKS) {
    g->setFont(t.font);
    int w = spacedWidth(g, t.text, t.spacing);
    Serial.printf("layout %-12s %3d / %3d px\n", t.tag, w, t.maxW);
    if (w > t.maxW) {
      bad++;
      Serial.printf("  OVERFLOW by %d px: \"%s\"\n", w - t.maxW, t.text);
    }
  }
  Serial.printf("layout audit: %d overflow\n", bad);
}

// ------------------------- actions -----------------------------------
void nextField() {
  editField = (editField + 1) % 3;
  hapticPlay(PAT_TICK);
}

void adjustField(int delta) {
  lenMin[editField] = constrain(lenMin[editField] + delta, MIN_LEN, MAX_LEN);
  if (editField == phase && !running) loadPhase();
  hapticPlay(PAT_TICK);
}

bool reportFrame = false;

void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    switch (c) {
      case 's': toggleRun(); break;
      case 'r': resetPhase(); break;
      case 'n': advance(true); break;
      case 'f': nextField(); break;
      case '+': case '=': adjustField(1); break;
      case '-': case '_': adjustField(-1); break;
      case '?': Serial.println(F("s start/pause | r reset | n next | f field | +/- adjust | t frame time")); break;
      case 't': reportFrame = true; break;
      default: break;
    }
  }
}

// ------------------------- setup -------------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println(F("\n=== Lith pomodoro ==="));

  pinMode(PIN_SW1,   INPUT_PULLUP);
  pinMode(PIN_SW2,   INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);

  encState = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  motorInit();

  displayOK = tft.init();
  if (!displayOK) {
    Serial.println(F("ERROR: tft.init() failed - continuing in serial-only mode"));
  } else {
    tft.setRotation(1);
    tft.setBrightness(255);
    tft.fillScreen(COL_BG);

    frame.setColorDepth(16);
    frame.setPsram(false);
    if (frame.createSprite(320, 170) == nullptr) {
      frame.setPsram(true);
      if (frame.createSprite(320, 170) == nullptr) {
        Serial.println(F("frame buffer alloc failed - drawing direct (expect flicker)"));
      } else { useFrameBuffer = true; }
    } else { useFrameBuffer = true; }

    if (useFrameBuffer) {
      // Ask LovyanGFX to lay down one pixel, then read the raw buffer back.
      // Whatever it did is the order our direct writes have to match.
      frame.drawPixel(0, 0, COL_INK);
      uint16_t probe = ((uint16_t *)frame.getBuffer())[0];
      bufSwapped = (probe != COL_INK);
      Serial.printf("sprite pixel order: %s (raw %04X, literal %04X)\n",
                    bufSwapped ? "byte-swapped" : "native", probe, COL_INK);
    }
  }

  layoutAudit();
  loadPhase();
  Serial.printf("free heap %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.println(F("commands: s r n f + - ?"));
}

// ------------------------- loop --------------------------------------
void loop() {
  handleSerial();

  uint8_t e1 = pollButton(BTN1);
  if (e1 == EV_SHORT) toggleRun();
  if (e1 == EV_LONG)  resetPhase();

  uint8_t e2 = pollButton(BTN2);
  if (e2 == EV_SHORT) advance(true);
  if (e2 == EV_LONG)  nextField();

  noInterrupts();
  int32_t raw = encRaw;
  interrupts();
  int32_t det = raw / ENC_STEPS_PER_DETENT;
  if (det != encDetentLast) {
    int delta = (int)(det - encDetentLast);
    encDetentLast = det;
    if (!running) adjustField(delta);
  }

  timerService();
  hapticService();

  static uint32_t tNext = 0;
  uint32_t now = millis();
  if (displayOK && now >= tNext) {
    tNext = now + 40;
    uint32_t t0 = micros();
    render();
    if (reportFrame) {
      reportFrame = false;
      Serial.printf("frame %lu us\n", (unsigned long)(micros() - t0));
    }
  }
}
