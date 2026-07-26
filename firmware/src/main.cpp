// =====================================================================
// Lith hardware bring-up test — ESP32-S3-Zero
//   ST7789V2 170x320 display (LovyanGFX) + rotary encoder + 2 switches
//   + vibration motor on an NPN low-side switch
//
// PlatformIO: prefer saving this as src/main.cpp rather than a .ino file.
// .ino files get auto-generated prototypes inserted above your own type
// definitions, which breaks any function whose signature uses a custom
// type. This file is written to survive that, but .cpp avoids it entirely.
//
// CONTROLS
//   Encoder turn      -> motor duty +/- 8 (0..255)
//   SW1 short press   -> 120 ms motor pulse at current duty
//   SW1 long  press   -> reset encoder count and press counters
//   SW2 short press   -> toggle motor continuous on/off
//   SW2 long  press   -> cycle display test screen
//
// SERIAL COMMANDS (115200)
//   r reset   p pulse   m toggle motor   d next screen   + / - duty   ? help
//
// =====================================================================

#define LGFX_USE_V1
#include <Arduino.h>
#include <stdarg.h>
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
static constexpr uint32_t DEBOUNCE_MS    = 25;
static constexpr uint32_t LONGPRESS_MS   = 800;
static constexpr uint32_t PULSE_MS       = 120;

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
LGFX_Sprite ball(&tft);

bool displayOK      = false;
bool useFrameBuffer = false;

// ------------------------- palette -----------------------------------
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static const uint16_t COL_BG     = RGB565(  8, 10, 14);
static const uint16_t COL_PANEL  = RGB565( 24, 28, 36);
static const uint16_t COL_LINE   = RGB565( 62, 70, 86);
static const uint16_t COL_TEXT   = RGB565(228,232,240);
static const uint16_t COL_DIM    = RGB565(128,138,156);
static const uint16_t COL_ACCENT = RGB565(255,150, 40);
static const uint16_t COL_OK     = RGB565( 70,200,120);

// ------------------------- encoder -----------------------------------
static const int8_t QUAD[16] = { 0, -1,  1,  0,
                                 1,  0,  0, -1,
                                -1,  0,  0,  1,
                                 0,  1, -1,  0 };
volatile uint8_t  encState = 0;
volatile int32_t  encRaw   = 0;
volatile uint32_t encEdges = 0;
int32_t encDetentLast = 0;

void IRAM_ATTR encISR() {
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  encState = ((encState << 2) | (a << 1) | b) & 0x0F;
  encRaw  += QUAD[encState];
  encEdges++;
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
  uint32_t count;
  bool     longFired;
};

Button btns[BTN_COUNT] = {
  {PIN_SW1, false, false, 0, 0, 0, false},
  {PIN_SW2, false, false, 0, 0, 0, false}
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
      b.count++;
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
int      motorDuty       = 128;
bool     motorLatched    = false;
uint32_t motorPulseUntil = 0;

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

bool motorIsOn() { return motorLatched || (millis() < motorPulseUntil); }

void motorService() { motorRaw(motorIsOn() ? (uint8_t)motorDuty : 0); }

void motorPulse(uint32_t ms) { motorPulseUntil = millis() + ms; }

// ------------------------- event log ---------------------------------
char logLine[3][40] = {"", "", ""};

void logEvent(const char *fmt, ...) {
  char buf[40];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  memcpy(logLine[2], logLine[1], sizeof(logLine[0]));
  memcpy(logLine[1], logLine[0], sizeof(logLine[0]));
  strncpy(logLine[0], buf, sizeof(logLine[0]) - 1);
  logLine[0][sizeof(logLine[0]) - 1] = '\0';
  Serial.printf("[%8lu] %s\n", (unsigned long)millis(), buf);
}

// ------------------------- screens -----------------------------------
static const uint8_t SCR_STATUS   = 0;
static const uint8_t SCR_COLOUR   = 1;
static const uint8_t SCR_GEOMETRY = 2;
static const uint8_t SCR_SPRITE   = 3;
static const uint8_t SCR_COUNT    = 4;

uint8_t screen = SCR_STATUS;
const char *screenName[SCR_COUNT] = {"STATUS", "COLOUR", "GEOMETRY", "SPRITE"};

float    fps       = 0;
uint32_t lastFrame = 0;

void panelBox(LovyanGFX *g, int x, int y, int w, int h, const char *title) {
  g->fillRoundRect(x, y, w, h, 4, COL_PANEL);
  g->drawRoundRect(x, y, w, h, 4, COL_LINE);
  g->setFont(&fonts::Font0);
  g->setTextSize(1);
  g->setTextColor(COL_DIM, COL_PANEL);
  g->setTextDatum(textdatum_t::top_left);
  g->drawString(title, x + 7, y + 6);
}

void renderStatus(LovyanGFX *g) {
  char buf[40];
  g->fillScreen(COL_BG);

  // header
  g->fillRect(0, 0, 320, 20, COL_PANEL);
  g->setFont(&fonts::Font2);
  g->setTextColor(COL_ACCENT, COL_PANEL);
  g->setTextDatum(textdatum_t::top_left);
  g->drawString("LITH DEBUG", 6, 2);

  uint32_t s = millis() / 1000;
  snprintf(buf, sizeof(buf), "%02lu:%02lu  %2.0ffps",
           (unsigned long)(s / 60), (unsigned long)(s % 60), fps);
  g->setTextColor(COL_DIM, COL_PANEL);
  g->setTextDatum(textdatum_t::top_right);
  g->drawString(buf, 314, 2);

  // ---- switches ----
  panelBox(g, 4, 24, 100, 76, "SWITCHES");
  const int   rowY[BTN_COUNT] = {42, 66};
  const char *rowN[BTN_COUNT] = {"SW1", "SW2"};
  for (int i = 0; i < BTN_COUNT; i++) {
    bool on = btns[i].level;
    g->fillRoundRect(12, rowY[i], 16, 16, 3, on ? COL_OK : RGB565(48, 54, 66));
    g->setFont(&fonts::Font2);
    g->setTextColor(on ? COL_TEXT : COL_DIM, COL_PANEL);
    g->setTextDatum(textdatum_t::top_left);
    g->drawString(rowN[i], 34, rowY[i]);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)btns[i].count);
    g->setTextColor(COL_DIM, COL_PANEL);
    g->setTextDatum(textdatum_t::top_right);
    g->drawString(buf, 96, rowY[i]);
  }

  // ---- encoder ----
  panelBox(g, 110, 24, 100, 76, "ENCODER");
  noInterrupts();
  int32_t  raw   = encRaw;
  uint32_t edges = encEdges;
  interrupts();
  int32_t det = raw / ENC_STEPS_PER_DETENT;

  g->setFont(&fonts::Font4);
  g->setTextColor(COL_ACCENT, COL_PANEL);
  g->setTextDatum(textdatum_t::top_center);
  snprintf(buf, sizeof(buf), "%ld", (long)det);
  g->drawString(buf, 160, 42);

  g->setFont(&fonts::Font0);
  g->setTextColor(COL_DIM, COL_PANEL);
  g->setTextDatum(textdatum_t::top_left);
  snprintf(buf, sizeof(buf), "raw %ld", (long)raw);
  g->drawString(buf, 118, 74);
  snprintf(buf, sizeof(buf), "A%d B%d", digitalRead(PIN_ENC_A), digitalRead(PIN_ENC_B));
  g->setTextDatum(textdatum_t::top_right);
  g->drawString(buf, 202, 74);
  snprintf(buf, sizeof(buf), "edges %lu", (unsigned long)edges);
  g->setTextDatum(textdatum_t::top_left);
  g->drawString(buf, 118, 86);

  // ---- motor ----
  panelBox(g, 216, 24, 100, 76, "MOTOR");
  bool on = motorIsOn();
  g->setFont(&fonts::Font4);
  g->setTextColor(on ? COL_OK : COL_DIM, COL_PANEL);
  g->setTextDatum(textdatum_t::top_center);
  snprintf(buf, sizeof(buf), "%d%%", (motorDuty * 100) / 255);
  g->drawString(buf, 266, 42);

  g->drawRect(224, 72, 84, 10, COL_LINE);
  int bw = (motorDuty * 82) / 255;
  if (bw > 0) g->fillRect(225, 73, bw, 8, on ? COL_OK : RGB565(70, 78, 94));

  g->setFont(&fonts::Font0);
  g->setTextColor(COL_DIM, COL_PANEL);
  g->setTextDatum(textdatum_t::top_left);
  g->drawString(motorLatched ? "LATCHED" : (on ? "PULSE" : "off"), 224, 86);
  snprintf(buf, sizeof(buf), "d%3d", motorDuty);
  g->setTextDatum(textdatum_t::top_right);
  g->drawString(buf, 308, 86);

  // ---- log ----
  panelBox(g, 4, 104, 312, 44, "EVENTS");
  g->setFont(&fonts::Font0);
  g->setTextDatum(textdatum_t::top_left);
  for (int i = 0; i < 3; i++) {
    g->setTextColor(i == 0 ? COL_TEXT : COL_DIM, COL_PANEL);
    g->drawString(logLine[i], 11, 118 + i * 10);
  }

  // ---- hints ----
  g->setFont(&fonts::Font0);
  g->setTextColor(COL_DIM, COL_BG);
  g->setTextDatum(textdatum_t::top_left);
  g->drawString("SW1 pulse / hold=reset   SW2 motor / hold=screen   ENC duty", 6, 154);
}

void renderColour(LovyanGFX *g) {
  const uint16_t bars[6] = {TFT_RED, TFT_GREEN, TFT_BLUE,
                            TFT_CYAN, TFT_MAGENTA, TFT_YELLOW};
  for (int i = 0; i < 6; i++) g->fillRect(i * 53, 0, 53, 90, bars[i]);
  for (int i = 0; i < 32; i++) {
    uint8_t v = i * 8;
    g->fillRect(i * 10, 90, 10, 40, RGB565(v, v, v));
  }
  g->fillRect(0, 130, 320, 40, TFT_BLACK);
  g->setFont(&fonts::Font2);
  g->setTextColor(TFT_WHITE, TFT_BLACK);
  g->setTextDatum(textdatum_t::top_left);
  g->drawString("COLOUR  R G B C M Y + grey ramp", 6, 140);
}

void renderGeometry(LovyanGFX *g) {
  g->fillScreen(TFT_BLACK);
  g->drawRect(0, 0, 320, 170, TFT_WHITE);
  g->drawRect(2, 2, 316, 166, TFT_YELLOW);
  for (int x = 0; x < 320; x += 20) g->drawFastVLine(x, 0, 170, RGB565(40, 40, 40));
  for (int y = 0; y < 170; y += 20) g->drawFastHLine(0, y, 320, RGB565(40, 40, 40));
  g->drawFastVLine(160, 0, 170, TFT_CYAN);
  g->drawFastHLine(0, 85, 320, TFT_CYAN);
  g->drawCircle(160, 85, 80, TFT_GREEN);
  g->fillRect(0, 0, 6, 6, TFT_RED);
  g->fillRect(314, 164, 6, 6, TFT_RED);
  g->setFont(&fonts::Font2);
  g->setTextColor(TFT_WHITE, TFT_BLACK);
  g->setTextDatum(textdatum_t::middle_center);
  g->drawString("GEOMETRY 320x170", 160, 85);
}

int bx = 10, by = 10, bdx = 3, bdy = 2;

void renderSpriteScreen() {
  ball.fillSprite(TFT_BLACK);
  ball.fillCircle(30, 30, 26, COL_ACCENT);
  ball.fillCircle(30, 30, 14, TFT_BLACK);
  ball.pushSprite(bx, by);
  bx += bdx; by += bdy;
  if (bx <= 0 || bx >= tft.width()  - 60) bdx = -bdx;
  if (by <= 0 || by >= tft.height() - 60) bdy = -bdy;
}

// ------------------------- actions -----------------------------------
void resetAll() {
  noInterrupts();
  encRaw = 0; encEdges = 0; encState = 0;
  interrupts();
  encDetentLast = 0;
  btns[BTN1].count = 0;
  btns[BTN2].count = 0;
  logEvent("reset: encoder + counters");
}

void setDuty(int d) {
  motorDuty = constrain(d, 0, 255);
  logEvent("duty -> %d (%d%%)", motorDuty, (motorDuty * 100) / 255);
}

void nextScreen() {
  screen = (screen + 1) % SCR_COUNT;
  if (displayOK) tft.fillScreen(TFT_BLACK);
  logEvent("screen -> %s", screenName[screen]);
}

void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    switch (c) {
      case 'r': resetAll(); break;
      case 'p': motorPulse(PULSE_MS); logEvent("serial: pulse"); break;
      case 'm': motorLatched = !motorLatched;
                logEvent("motor %s", motorLatched ? "LATCHED ON" : "off"); break;
      case 'd': nextScreen(); break;
      case '+': case '=': setDuty(motorDuty + 8); break;
      case '-': case '_': setDuty(motorDuty - 8); break;
      case '?': Serial.println(F("r reset | p pulse | m motor | d screen | +/- duty")); break;
      default: break;
    }
  }
}

// ------------------------- setup -------------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println(F("\n=== Lith hardware bring-up ==="));
  Serial.printf("SCLK %d  MOSI %d  DC %d  CS %d  RST %d  BLK %d\n",
                PIN_TFT_SCLK, PIN_TFT_MOSI, PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_RST, PIN_TFT_BLK);
  Serial.printf("SW1 %d  SW2 %d  ENC_A %d  ENC_B %d  MOTOR %d\n",
                PIN_SW1, PIN_SW2, PIN_ENC_A, PIN_ENC_B, PIN_MOTOR);

  pinMode(PIN_SW1,   INPUT_PULLUP);
  pinMode(PIN_SW2,   INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);

  Serial.printf("idle levels: SW1=%d SW2=%d A=%d B=%d (all should read 1)\n",
                digitalRead(PIN_SW1), digitalRead(PIN_SW2),
                digitalRead(PIN_ENC_A), digitalRead(PIN_ENC_B));

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
    tft.fillScreen(TFT_BLACK);

    frame.setColorDepth(16);
    frame.setPsram(false);
    if (frame.createSprite(320, 170) == nullptr) {
      frame.setPsram(true);
      if (frame.createSprite(320, 170) == nullptr) {
        Serial.println(F("frame buffer alloc failed - drawing direct (expect flicker)"));
      } else { useFrameBuffer = true; Serial.println(F("frame buffer in PSRAM")); }
    } else { useFrameBuffer = true; Serial.println(F("frame buffer in SRAM")); }

    ball.setColorDepth(16);
    if (ball.createSprite(60, 60) == nullptr) Serial.println(F("ball sprite alloc failed"));
  }

  logEvent("boot ok");
  Serial.println(F("commands: r p m d + - ?"));
  lastFrame = millis();
}

// ------------------------- loop --------------------------------------
void loop() {
  handleSerial();

  // --- buttons ---
  uint8_t e1 = pollButton(BTN1);
  if (e1 == EV_SHORT) { motorPulse(PULSE_MS); logEvent("SW1 short: motor pulse"); }
  if (e1 == EV_LONG)  { resetAll(); }

  uint8_t e2 = pollButton(BTN2);
  if (e2 == EV_SHORT) {
    motorLatched = !motorLatched;
    logEvent("SW2 short: motor %s", motorLatched ? "ON" : "off");
  }
  if (e2 == EV_LONG) { nextScreen(); }

  // --- encoder -> duty ---
  noInterrupts();
  int32_t raw = encRaw;
  interrupts();
  int32_t det = raw / ENC_STEPS_PER_DETENT;
  if (det != encDetentLast) {
    int delta = (int)(det - encDetentLast);
    encDetentLast = det;
    setDuty(motorDuty + delta * 8);
    logEvent("enc %s  det %ld", delta > 0 ? "CW" : "CCW", (long)det);
  }

  motorService();

  // --- render ~40 fps ---
  static uint32_t tNext = 0;
  uint32_t now = millis();
  if (now >= tNext) {
    tNext = now + 25;
    uint32_t dt = now - lastFrame;
    lastFrame = now;
    if (dt) fps = fps * 0.85f + (1000.0f / dt) * 0.15f;

    if (displayOK) {
      if (screen == SCR_SPRITE) {
        renderSpriteScreen();
      } else {
        LovyanGFX *g = useFrameBuffer ? (LovyanGFX *)&frame : (LovyanGFX *)&tft;
        if      (screen == SCR_COLOUR)   renderColour(g);
        else if (screen == SCR_GEOMETRY) renderGeometry(g);
        else                             renderStatus(g);
        if (useFrameBuffer) frame.pushSprite(0, 0);
      }
    }
  }
}
