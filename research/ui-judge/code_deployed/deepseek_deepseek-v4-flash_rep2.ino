#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// --- Hardware pins (lith v1) ---
#define PIN_TFT_SCLK 12
#define PIN_TFT_MOSI 11
#define PIN_TFT_DC   13
#define PIN_TFT_CS   10
#define PIN_TFT_RST  9
#define PIN_TFT_BLK  8
#define PIN_SW1      1
#define PIN_SW2      2
#define PIN_ENC_A    4
#define PIN_ENC_B    5
#define PIN_MOTOR    6

// --- Display init (verbatim) ---
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

LGFX tft;
LGFX_Sprite sprite(&tft);

// --- Simple RGB565 ---
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const uint16_t BG = rgb565(9, 10, 12);
const uint16_t INK = rgb565(238, 240, 245);
const float RATE_PER_HOUR = 100.0;
const float MAX_COST = 500.0;

// --- State ---
enum AppState { ST_IDLE, ST_RUNNING, ST_PAUSED };
AppState appState = ST_IDLE;
unsigned long accumulatedMs = 0;
unsigned long stateStartMs = 0;
float currentCost = 0;

// --- Encoder stays interrupt-driven even if unused ---
volatile int encRaw = 0;
const int ENC_LOOKUP[4] = {0, 1, -1, 0};

void IRAM_ATTR encISR() {
  static uint8_t last = 0;
  uint8_t ab = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  uint8_t idx = (last << 2) | ab;
  encRaw += ENC_LOOKUP[idx & 0x03];
  if (idx & 0x04) last = ab;
}

// --- Debounced button ---
struct Button {
  uint8_t pin;
  bool stable;
  bool previous;
  bool down;
  bool pressed;
  bool longFired;
  bool longPressed;
  unsigned long lastChange;
  unsigned long downSince;

  Button(uint8_t p) : pin(p), stable(HIGH), previous(HIGH), down(false),
                       pressed(false), longFired(false), longPressed(false),
                       lastChange(0), downSince(0) {}

  void poll() {
    bool raw = digitalRead(pin);
    if (raw != previous) { lastChange = millis(); previous = raw; }
    if (millis() - lastChange < 25) return;
    if (raw != stable) {
      stable = raw;
      if (stable == LOW) { down = true; downSince = millis(); longFired = false; }
      else {
        if (down && !longFired && millis() - downSince < 800) pressed = true;
        down = false;
      }
    } else if (stable == LOW && down && !longFired && millis() - downSince >= 800) {
      longFired = true;
      longPressed = true;
    }
  }

  void clear() { pressed = false; longPressed = false; }
};

Button sw1(PIN_SW1);
Button sw2(PIN_SW2);

// --- Color helpers ---
uint16_t hueColor(float hue) {
  hue = fmod(hue, 360.0);
  float h = hue / 60.0;
  int i = (int)h;
  float f = h - i;
  uint8_t q = (uint8_t)(255 * (1.0 - f));
  uint8_t r2 = (uint8_t)(255 * f);
  uint8_t r, g, b;
  switch (i % 6) {
    case 0: r = 255; g = r2;  b = 0;   break;
    case 1: r = q;   g = 255; b = 0;   break;
    case 2: r = 0;   g = 255; b = r2;  break;
    case 3: r = 0;   g = q;   b = 255; break;
    case 4: r = r2;  g = 0;   b = 255; break;
    default: r = 255; g = 0;   b = q;  break;
  }
  return rgb565(r, g, b);
}

uint16_t costColor(float cost) {
  float t = constrain(cost / MAX_COST, 0.0, 1.0);
  float hue = (1.0 - t) * 120.0;
  return hueColor(hue);
}

uint16_t scaleColor(uint16_t col, float f) {
  uint8_t r = (uint8_t)(((col >> 11) & 0x1F) * f);
  uint8_t g = (uint8_t)(((col >> 5) & 0x3F) * f);
  uint8_t b = (uint8_t)((col & 0x1F) * f);
  return (r << 11) | (g << 5) | b;
}

// --- Meeting control ---
void startMeeting() {
  appState = ST_RUNNING;
  stateStartMs = millis();
  accumulatedMs = 0;
  currentCost = 0;
  Serial.println("[meeting] started");
}

void togglePause() {
  if (appState == ST_RUNNING) {
    accumulatedMs += millis() - stateStartMs;
    appState = ST_PAUSED;
    Serial.println("[meeting] paused");
  } else if (appState == ST_PAUSED) {
    stateStartMs = millis();
    appState = ST_RUNNING;
    Serial.println("[meeting] resumed");
  }
}

void endMeeting() {
  if (appState != ST_IDLE) {
    Serial.printf("[meeting] ended, cost $%.2f\n", currentCost);
  }
  appState = ST_IDLE;
  stateStartMs = 0;
  accumulatedMs = 0;
  currentCost = 0;
}

void updateCost() {
  if (appState == ST_RUNNING) {
    unsigned long total = accumulatedMs + (millis() - stateStartMs);
    currentCost = RATE_PER_HOUR * (total / 3600000.0);
  } else if (appState == ST_PAUSED) {
    currentCost = RATE_PER_HOUR * (accumulatedMs / 3600000.0);
  }
}

// --- Render: one light only ---
unsigned long tNext = 0;
const unsigned long FRAME_MS = 30;

void render() {
  if (millis() < tNext) return;
  tNext = millis() + FRAME_MS;
  if (!sprite.getBuffer()) return;

  sprite.fillSprite(BG);
  const int cx = 160, cy = 85;

  if (appState == ST_IDLE) {
    sprite.fillCircle(cx, cy, 10, scaleColor(INK, 0.25));
  } else if (appState == ST_RUNNING) {
    float progress = constrain(currentCost / MAX_COST, 0.0, 1.0);
    float radius = 16 + progress * 52;
    uint16_t col = costColor(currentCost);
    sprite.fillCircle(cx, cy, (int)(radius * 1.35), scaleColor(col, 0.18));
    sprite.fillCircle(cx, cy, (int)(radius * 1.15), scaleColor(col, 0.45));
    sprite.fillCircle(cx, cy, (int)radius, col);
  } else {
    float progress = constrain(currentCost / MAX_COST, 0.0, 1.0);
    int radius = 16 + (int)(progress * 52);
    uint16_t col = costColor(currentCost);
    sprite.drawCircle(cx, cy, radius, col);
    sprite.drawCircle(cx, cy, radius - 6, scaleColor(col, 0.5));
  }

  sprite.pushSprite(0, 0);
}

void drainSerial() {
  while (Serial.available()) Serial.read();
}

// --- Setup ---
void setup() {
  Serial.begin(115200);

  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);

  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);

  sprite.setColorDepth(16);
  sprite.createSprite(320, 170);

  Serial.println("[lith] meeting-glow ready");
}

// --- Loop ---
void loop() {
  drainSerial();
  sw1.poll();
  sw2.poll();

  if (sw1.pressed) {
    sw1.clear();
    if (appState == ST_IDLE) startMeeting();
    else togglePause();
  }
  if (sw1.longPressed) {
    sw1.clear();
    endMeeting();
  }
  if (sw2.pressed) sw2.clear();
  if (sw2.longPressed) sw2.clear();

  updateCost();
  render();
}
