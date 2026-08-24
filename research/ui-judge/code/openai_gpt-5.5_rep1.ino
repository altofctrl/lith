#include <Arduino.h>

#define PIN_TFT_SCLK 12
#define PIN_TFT_MOSI 11
#define PIN_TFT_DC 13
#define PIN_TFT_CS 10
#define PIN_TFT_RST 9
#define PIN_TFT_BLK 8
#define PIN_SW1 1
#define PIN_SW2 2
#define PIN_ENC_A 4
#define PIN_ENC_B 5
#define PIN_MOTOR 6

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
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
// usage: LGFX tft; tft.init(); tft.setRotation(1); tft.setBrightness(180);

LGFX tft;
LGFX_Sprite canvas(&tft);
bool spriteOk = false;

const uint16_t COL_BG = 0x0841;
const uint16_t COL_INK = 0xEF3D;
const uint16_t COL_DIM = 0x6B6D;
const uint16_t COL_AMBER = 0xFBE0;
const uint16_t COL_TEAL = 0x05B6;

volatile int32_t encRaw = 0;
volatile uint8_t encPrev = 0;
const int8_t ENC_LUT[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

struct ButtonState {
  uint8_t pin;
  bool stablePressed;
  bool lastReading;
  bool longSent;
  uint32_t changedAt;
  uint32_t pressedAt;
};

ButtonState sw1 = {PIN_SW1, false, false, false, 0, 0};
ButtonState sw2 = {PIN_SW2, false, false, false, 0, 0};

const uint32_t DEBOUNCE_MS = 25;
const uint32_t LONG_MS = 800;
const uint32_t FRAME_MS = 33;

uint32_t nextFrameAt = 0;
uint32_t lastLogAt = 0;
int32_t lastDetent = 0;

int rateDollarsPerHour = 200;
bool running = false;
bool everStarted = false;
uint64_t accumulatedMs = 0;
uint32_t startedAtMs = 0;

void IRAM_ATTR encoderISR() {
  uint8_t a = digitalRead(PIN_ENC_A) ? 1 : 0;
  uint8_t b = digitalRead(PIN_ENC_B) ? 1 : 0;
  uint8_t now = (a << 1) | b;
  uint8_t idx = (encPrev << 2) | now;
  encRaw += ENC_LUT[idx & 0x0F];
  encPrev = now;
}

uint64_t currentElapsedMs(uint32_t now) {
  uint64_t total = accumulatedMs;
  if (running) total += (uint32_t)(now - startedAtMs);
  return total;
}

uint64_t currentCostCents(uint32_t now) {
  uint64_t elapsed = currentElapsedMs(now);
  uint64_t rateCents = (uint64_t)rateDollarsPerHour * 100ULL;
  return (rateCents * elapsed) / 3600000ULL;
}

void formatCost(uint64_t cents, char *buf, size_t len) {
  if (cents < 100000ULL) {
    snprintf(buf, len, "$%llu.%02llu", cents / 100ULL, cents % 100ULL);
  } else {
    snprintf(buf, len, "$%llu", (cents + 50ULL) / 100ULL);
  }
}

void formatRate(char *buf, size_t len) {
  snprintf(buf, len, "$%d/h", rateDollarsPerHour);
}

void setRunning(bool on, uint32_t now) {
  if (on == running) return;
  if (on) {
    running = true;
    everStarted = true;
    startedAtMs = now;
    Serial.println("state running");
  } else {
    accumulatedMs = currentElapsedMs(now);
    running = false;
    Serial.println("state paused");
  }
}

void resetMeeting() {
  running = false;
  everStarted = false;
  accumulatedMs = 0;
  startedAtMs = 0;
  Serial.println("state idle reset");
}

void onShortPress(uint8_t pin, uint32_t now) {
  if (pin == PIN_SW1) {
    setRunning(!running, now);
  } else if (pin == PIN_SW2) {
    if (!running && everStarted) resetMeeting();
  }
}

void onLongPress(uint8_t pin, uint32_t now) {
  (void)now;
  if (pin == PIN_SW1 || pin == PIN_SW2) resetMeeting();
}

void pollButton(ButtonState &b, uint32_t now) {
  bool reading = digitalRead(b.pin) == LOW;
  if (reading != b.lastReading) {
    b.lastReading = reading;
    b.changedAt = now;
  }

  if ((uint32_t)(now - b.changedAt) >= DEBOUNCE_MS && reading != b.stablePressed) {
    b.stablePressed = reading;
    if (b.stablePressed) {
      b.pressedAt = now;
      b.longSent = false;
    } else {
      if (!b.longSent) onShortPress(b.pin, now);
    }
  }

  if (b.stablePressed && !b.longSent && (uint32_t)(now - b.pressedAt) >= LONG_MS) {
    b.longSent = true;
    onLongPress(b.pin, now);
  }
}

void handleEncoder() {
  int32_t raw;
  noInterrupts();
  raw = encRaw;
  interrupts();

  int32_t detent = raw / 4;
  int32_t delta = detent - lastDetent;
  if (delta == 0) return;
  lastDetent = detent;

  if (!running && !everStarted) {
    rateDollarsPerHour += (int)delta * 10;
    if (rateDollarsPerHour < 10) rateDollarsPerHour = 10;
    if (rateDollarsPerHour > 5000) rateDollarsPerHour = 5000;
    Serial.printf("rate %d dollars_per_hour\n", rateDollarsPerHour);
  }
}

void handleSerial(uint32_t now) {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 's' || c == ' ') setRunning(!running, now);
    else if (c == 'r') resetMeeting();
    else if (c == '+') {
      rateDollarsPerHour += 10;
      if (rateDollarsPerHour > 5000) rateDollarsPerHour = 5000;
    } else if (c == '-') {
      rateDollarsPerHour -= 10;
      if (rateDollarsPerHour < 10) rateDollarsPerHour = 10;
    }
  }
}

void drawSpacedLabel(LGFX_Device *g, const char *label, int y) {
  char spaced[40];
  size_t n = 0;
  for (size_t i = 0; label[i] && n + 2 < sizeof(spaced); i++) {
    spaced[n++] = label[i];
    if (label[i + 1]) spaced[n++] = ' ';
  }
  spaced[n] = 0;
  g->setTextColor(COL_INK, COL_BG);
  g->setTextFont(2);
  g->setTextSize(1);
  g->setTextDatum(middle_center);
  g->drawString(spaced, 160, y);
}

void drawCenteredFocal(LGFX_Device *g, const char *text, int y) {
  g->setTextColor(COL_INK, COL_BG);
  g->setTextDatum(middle_center);
  g->setTextFont(7);
  g->setTextSize(1);
  int w = g->textWidth(text);
  if (w > 304) {
    g->setTextFont(4);
    g->setTextSize(2);
    w = g->textWidth(text);
  }
  if (w > 304) {
    g->setTextFont(4);
    g->setTextSize(1);
  }
  g->drawString(text, 160, y);
}

void drawAccent(LGFX_Device *g, uint32_t now) {
  if (running) {
    uint8_t pulse = (uint8_t)((now / 18) % 80);
    if (pulse > 40) pulse = 80 - pulse;
    int brightness = 174 + pulse;
    if (brightness > 220) brightness = 220;
    tft.setBrightness(brightness);
    int w = 70 + (int)((now / 35) % 180);
    if (w > 160) w = 250 - w;
    g->fillRect(160 - w / 2, 160, w, 4, COL_AMBER);
  } else if (everStarted) {
    tft.setBrightness(115);
    g->fillRect(128, 160, 64, 4, COL_DIM);
  } else {
    tft.setBrightness(155);
    g->fillRect(148, 160, 24, 4, COL_TEAL);
  }
}

void render(uint32_t now) {
  if ((int32_t)(now - nextFrameAt) < 0) return;
  nextFrameAt = now + FRAME_MS;

  LGFX_Device *g = spriteOk ? (LGFX_Device *)&canvas : (LGFX_Device *)&tft;
  g->fillScreen(COL_BG);

  char focal[32];
  const char *label;
  if (!everStarted && !running) {
    formatRate(focal, sizeof(focal));
    label = "hourly rate";
  } else {
    formatCost(currentCostCents(now), focal, sizeof(focal));
    label = running ? "meeting cost" : "paused cost";
  }

  drawSpacedLabel(g, label, 30);
  drawCenteredFocal(g, focal, 92);
  drawAccent(g, now);

  if (spriteOk) canvas.pushSprite(0, 0);
}

void logStatus(uint32_t now) {
  if ((uint32_t)(now - lastLogAt) < 2000) return;
  lastLogAt = now;
  char cost[32];
  formatCost(currentCostCents(now), cost, sizeof(cost));
  Serial.printf("status rate=%d/hr elapsed_ms=%llu cost=%s running=%d\n",
                rateDollarsPerHour,
                currentElapsedMs(now),
                cost,
                running ? 1 : 0);
}

void layoutAudit() {
  char buf[32];
  const char *tests[] = {"$999.99", "$10000", "$5000/h"};
  tft.setTextFont(7);
  tft.setTextSize(1);
  for (int i = 0; i < 3; i++) {
    int w = tft.textWidth(tests[i]);
    snprintf(buf, sizeof(buf), "layout %s width=%d", tests[i], w);
    Serial.println(buf);
    if (w > 304) Serial.println("layout overflow would downshift font");
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);

  encPrev = ((digitalRead(PIN_ENC_A) ? 1 : 0) << 1) | (digitalRead(PIN_ENC_B) ? 1 : 0);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encoderISR, CHANGE);

  tft.init();
  tft.setRotation(1);
  tft.setBrightness(155);
  tft.fillScreen(COL_BG);

  canvas.setColorDepth(16);
  spriteOk = canvas.createSprite(320, 170) != nullptr;
  if (!spriteOk) Serial.println("sprite allocation failed, drawing direct");

  layoutAudit();
  Serial.println("meeting cost meter ready");
  Serial.println("sw1 start pause, hold reset. sw2 resets while paused. encoder edits rate before start.");
}

void loop() {
  uint32_t now = millis();
  handleSerial(now);
  pollButton(sw1, now);
  pollButton(sw2, now);
  handleEncoder();
  render(now);
  logStatus(now);
}
