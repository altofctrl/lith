// meeting-burn: a silent meeting cost meter for lith v1
// no haptics by request. state is carried by one big number and the ambient level.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <math.h>

// ---------- all type declarations first (Arduino auto-prototypes) ----------
enum BtnEvent { BTN_NONE, BTN_SHORT, BTN_LONG };
enum Mode { IDLE, RUNNING, PAUSED, DONE };
enum Field { F_HEADS, F_RATE };

struct Button {
  uint8_t pin;
  bool stable;        // true = pressed
  bool raw;
  uint32_t tChange;
  uint32_t tDown;
  bool longFired;
};

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel; lgfx::Bus_SPI _bus; lgfx::Light_PWM _light;
public:
  LGFX() {
    { auto cfg = _bus.config(); cfg.spi_host = SPI2_HOST; cfg.spi_mode = 0;
      cfg.freq_write = 50000000; cfg.freq_read = 16000000; cfg.spi_3wire = false;
      cfg.use_lock = true; cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12; cfg.pin_mosi = 11; cfg.pin_miso = -1; cfg.pin_dc = 13;
      _bus.config(cfg); _panel.setBus(&_bus); }
    { auto cfg = _panel.config(); cfg.pin_cs = 10; cfg.pin_rst = 9; cfg.pin_busy = -1;
      cfg.panel_width = 170; cfg.panel_height = 320; cfg.offset_x = 35; cfg.offset_y = 0;
      cfg.offset_rotation = 2; cfg.readable = false; cfg.invert = true;
      cfg.rgb_order = false; cfg.dlen_16bit = false; cfg.bus_shared = false;
      _panel.config(cfg); }
    { auto cfg = _light.config(); cfg.pin_bl = 8; cfg.invert = false;
      cfg.freq = 44100; cfg.pwm_channel = 7; _light.config(cfg); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};

static const int PIN_TFT_SCLK = 12;
static const int PIN_TFT_MOSI = 11;
static const int PIN_TFT_DC   = 13;
static const int PIN_TFT_CS   = 10;
static const int PIN_TFT_RST  = 9;
static const int PIN_TFT_BLK  = 8;
static const int PIN_SW1      = 1;
static const int PIN_SW2      = 2;
static const int PIN_ENC_A    = 4;
static const int PIN_ENC_B    = 5;
static const int PIN_MOTOR    = 6;

static const int SCR_W = 320;
static const int SCR_H = 170;
static const int SAFE_W = 296;

LGFX tft;
LGFX_Sprite spr(&tft);
bool useSprite = false;
Preferences prefs;

static uint16_t COL_BG, COL_INK;

// ---------- encoder (interrupt driven) ----------
static const int8_t QTAB[16] = { 0,-1, 1, 0,  1, 0, 0,-1, -1, 0, 0, 1,  0, 1,-1, 0 };
volatile uint8_t  encState = 0;
volatile int32_t  encRaw   = 0;
int32_t lastDetent = 0;

void IRAM_ATTR encISR() {
  uint8_t s = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  encState = (uint8_t)(((encState << 2) | s) & 0x0F);
  encRaw += QTAB[encState];
}

// ---------- buttons ----------
static const uint32_t DEBOUNCE_MS = 25;
static const uint32_t LONG_MS     = 800;

Button bt1 = { (uint8_t)PIN_SW1, false, false, 0, 0, false };
Button bt2 = { (uint8_t)PIN_SW2, false, false, 0, 0, false };

BtnEvent pollButton(Button &b, uint32_t now) {
  bool r = (digitalRead(b.pin) == LOW);
  if (r != b.raw) { b.raw = r; b.tChange = now; }
  BtnEvent ev = BTN_NONE;
  if ((now - b.tChange) >= DEBOUNCE_MS && b.stable != b.raw) {
    b.stable = b.raw;
    if (b.stable) { b.tDown = now; b.longFired = false; }
    else if (!b.longFired) { ev = BTN_SHORT; }
  }
  if (b.stable && !b.longFired && (now - b.tDown) >= LONG_MS) {
    b.longFired = true;
    ev = BTN_LONG;
  }
  return ev;
}

// ---------- meter state ----------
Mode mode = IDLE;
Field field = F_HEADS;

int heads = 6;      // people in the room
int rate  = 120;    // dollars per person per hour

double accCents = 0.0;
uint32_t tLastAccrue = 0;
uint32_t tSettingsDirty = 0;
bool settingsDirty = false;

uint32_t tNextFrame = 0;
uint32_t tNextLog = 0;
float wavePhase = 0.0f;
float levelShown = 0.0f;

// ---------- helpers ----------
void saveSettings() {
  prefs.putInt("heads", heads);
  prefs.putInt("rate", rate);
}

void markDirty(uint32_t now) { settingsDirty = true; tSettingsDirty = now; }

void accrue(uint32_t now) {
  if (mode != RUNNING) { tLastAccrue = now; return; }
  uint32_t dt = now - tLastAccrue;
  if (dt == 0) return;
  tLastAccrue = now;
  double centsPerHour = (double)heads * (double)rate * 100.0;
  accCents += centsPerHour * ((double)dt / 3600000.0);
}

void formatMoney(char *buf, size_t n, double cents) {
  double d = cents / 100.0;
  if (d < 1000.0) snprintf(buf, n, "$%.2f", d);
  else            snprintf(buf, n, "$%.0f", d);
}

void drawSpacedLabel(lgfx::LGFXBase *g, const char *s, int cy) {
  g->setFont(&fonts::Font0);
  g->setTextSize(2);
  g->setTextDatum(middle_left);
  g->setTextColor(COL_INK);
  int glyph = 12;      // 6 px base * size 2
  int gap = 6;
  int len = (int)strlen(s);
  int total = len * glyph + (len - 1) * gap;
  int x = (SCR_W - total) / 2;
  char one[2] = { 0, 0 };
  for (int i = 0; i < len; i++) {
    one[0] = s[i];
    g->drawString(one, x, cy);
    x += glyph + gap;
  }
}

float fitSize(lgfx::LGFXBase *g, const char *s) {
  static const float sizes[] = { 2.0f, 1.7f, 1.4f, 1.2f, 1.0f, 0.85f };
  g->setFont(&fonts::FreeSansBold24pt7b);
  for (int i = 0; i < 6; i++) {
    g->setTextSize(sizes[i]);
    if (g->textWidth(s) <= SAFE_W) return sizes[i];
  }
  return 0.7f;
}

void accentFor(double cents, uint8_t &r, uint8_t &gg, uint8_t &b) {
  double d = cents / 100.0;
  if (d < 25.0)       { r = 0;   gg = 170; b = 155; }   // teal, cheap so far
  else if (d < 100.0) { r = 225; gg = 145; b = 40;  }   // amber, adding up
  else                { r = 215; gg = 65;  b = 55;  }   // red, expensive
}

void drawFluid(lgfx::LGFXBase *g, float level, double cents) {
  if (level < 0.0f) level = 0.0f;
  if (level > 1.0f) level = 1.0f;
  uint8_t r, gg, b;
  accentFor(cents, r, gg, b);
  uint16_t colDeep = g->color565(r, gg, b);
  uint16_t colSoft = g->color565((uint8_t)(r * 0.45f), (uint8_t)(gg * 0.45f), (uint8_t)(b * 0.45f));
  int span = SCR_H - 8;
  int base = SCR_H - (int)(level * span);
  for (int x = 0; x < SCR_W; x++) {
    float w1 = sinf((x * 0.021f) + wavePhase) * 3.4f;
    float w2 = sinf((x * 0.047f) - wavePhase * 0.7f) * 1.9f;
    int y = base + (int)(w1 + w2);
    if (y < 0) y = 0;
    if (y > SCR_H - 1) y = SCR_H - 1;
    g->drawFastVLine(x, y, SCR_H - y, colSoft);
    g->drawFastVLine(x, y, 2, colDeep);
  }
}

void render(uint32_t now) {
  lgfx::LGFXBase *g = useSprite ? (lgfx::LGFXBase *)&spr : (lgfx::LGFXBase *)&tft;

  float target;
  double d = accCents / 100.0;
  if (mode == IDLE)      target = 0.10f;
  else if (mode == DONE) target = 0.86f;
  else                   target = 0.10f + 0.76f * (float)(fmod(d, 100.0) / 100.0);
  levelShown += (target - levelShown) * 0.12f;

  wavePhase += (mode == RUNNING) ? 0.13f : 0.045f;
  if (wavePhase > 6.2831853f) wavePhase -= 6.2831853f;

  g->fillScreen(COL_BG);
  drawFluid(g, levelShown, accCents);

  char big[24];
  const char *label;
  if (mode == IDLE) {
    if (field == F_HEADS) { snprintf(big, sizeof(big), "%d", heads); label = "PEOPLE"; }
    else                  { snprintf(big, sizeof(big), "$%d", rate); label = "PER HOUR EACH"; }
  } else {
    formatMoney(big, sizeof(big), accCents);
    label = (mode == RUNNING) ? "BURNING" : (mode == PAUSED ? "HELD" : "TOTAL");
  }

  drawSpacedLabel(g, label, 40);

  float sz = fitSize(g, big);
  g->setFont(&fonts::FreeSansBold24pt7b);
  g->setTextSize(sz);
  g->setTextDatum(middle_center);
  g->setTextColor(COL_INK);
  g->drawString(big, SCR_W / 2, 98);

  if (useSprite) spr.pushSprite(0, 0);
}

void layoutAudit() {
  lgfx::LGFXBase *g = useSprite ? (lgfx::LGFXBase *)&spr : (lgfx::LGFXBase *)&tft;
  const char *worst[] = { "$99999.99", "$9999.99", "$2000", "64" };
  for (int i = 0; i < 4; i++) {
    g->setFont(&fonts::FreeSansBold24pt7b);
    g->setTextSize(1.0f);
    int w = g->textWidth(worst[i]);
    Serial.printf("[layout] \"%s\" at size 1.0 = %d px (safe %d)%s\n",
                  worst[i], w, SAFE_W, (w > SAFE_W) ? "  OVERFLOW, will shrink" : "");
  }
  const char *labels[] = { "PER HOUR EACH", "BURNING", "TOTAL", "PEOPLE" };
  for (int i = 0; i < 4; i++) {
    int len = (int)strlen(labels[i]);
    int total = len * 12 + (len - 1) * 6;
    Serial.printf("[layout] label \"%s\" = %d px (safe %d)%s\n",
                  labels[i], total, SAFE_W, (total > SAFE_W) ? "  OVERFLOW" : "");
  }
}

void serviceEncoder(uint32_t now) {
  noInterrupts();
  int32_t raw = encRaw;
  interrupts();
  int32_t det = raw / 4;
  int32_t delta = det - lastDetent;
  if (delta == 0) return;
  lastDetent = det;

  if (field == F_HEADS) {
    heads += (int)delta;
    if (heads < 1) heads = 1;
    if (heads > 64) heads = 64;
  } else {
    rate += (int)delta * 5;
    if (rate < 5) rate = 5;
    if (rate > 2000) rate = 2000;
  }
  markDirty(now);
  Serial.printf("[enc] %+ld  heads=%d rate=%d\n", (long)delta, heads, rate);
}

void serviceButtons(uint32_t now) {
  BtnEvent e1 = pollButton(bt1, now);
  BtnEvent e2 = pollButton(bt2, now);

  if (e1 == BTN_SHORT) {
    if (mode == IDLE)         { mode = RUNNING; accCents = 0.0; tLastAccrue = now; }
    else if (mode == RUNNING) { mode = PAUSED; }
    else if (mode == PAUSED)  { mode = RUNNING; tLastAccrue = now; }
    else                      { mode = IDLE; accCents = 0.0; }
    Serial.printf("[sw1] short, mode=%d\n", (int)mode);
  } else if (e1 == BTN_LONG) {
    mode = IDLE;
    accCents = 0.0;
    tLastAccrue = now;
    Serial.println("[sw1] long, reset");
  }

  if (e2 == BTN_SHORT) {
    field = (field == F_HEADS) ? F_RATE : F_HEADS;
    Serial.printf("[sw2] short, field=%s\n", field == F_HEADS ? "heads" : "rate");
  } else if (e2 == BTN_LONG) {
    if (mode == RUNNING || mode == PAUSED) {
      accrue(now);
      mode = DONE;
      Serial.printf("[sw2] long, meeting ended at $%.2f\n", accCents / 100.0);
    }
  }
}

void serviceSerial() {
  while (Serial.available()) { Serial.read(); }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);

  // silent build: motor is held off, never driven
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);

  encState = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);

  COL_BG  = tft.color565(9, 10, 12);
  COL_INK = tft.color565(238, 240, 245);

  spr.setColorDepth(16);
  useSprite = spr.createSprite(SCR_W, SCR_H);
  if (!useSprite) {
    spr.setPsram(true);
    useSprite = spr.createSprite(SCR_W, SCR_H);
  }
  if (!useSprite) Serial.println("[warn] sprite alloc failed, drawing direct");

  prefs.begin("lith", false);
  heads = prefs.getInt("heads", 6);
  rate  = prefs.getInt("rate", 120);
  if (heads < 1) heads = 1;
  if (heads > 64) heads = 64;
  if (rate < 5) rate = 5;
  if (rate > 2000) rate = 2000;

  tft.fillScreen(COL_BG);
  layoutAudit();
  Serial.printf("[boot] meeting-burn ready. heads=%d rate=%d\n", heads, rate);

  uint32_t now = millis();
  tLastAccrue = now;
  tNextFrame = now;
  tNextLog = now + 1000;

  noInterrupts();
  lastDetent = encRaw / 4;
  interrupts();
}

void loop() {
  uint32_t now = millis();

  serviceButtons(now);
  serviceEncoder(now);
  serviceSerial();
  accrue(now);

  if (settingsDirty && (now - tSettingsDirty) > 2500) {
    settingsDirty = false;
    saveSettings();
    Serial.println("[nvs] settings saved");
  }

  if ((int32_t)(now - tNextFrame) >= 0) {
    tNextFrame = now + 30;
    render(now);
  }

  if ((int32_t)(now - tNextLog) >= 0) {
    tNextLog = now + 1000;
    const char *ms = (mode == IDLE) ? "idle" : (mode == RUNNING) ? "running" : (mode == PAUSED) ? "paused" : "done";
    Serial.printf("[state] %s heads=%d rate=%d burned=$%.2f\n", ms, heads, rate, accCents / 100.0);
  }
}
