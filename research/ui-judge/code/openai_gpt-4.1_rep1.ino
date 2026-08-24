#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ==== Display setup (per hardware ground truth) ====
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

// ==== Pins ====
constexpr int PIN_SW1 = 1;
constexpr int PIN_SW2 = 2;
constexpr int PIN_ENC_A = 4;
constexpr int PIN_ENC_B = 5;
constexpr int PIN_MOTOR = 6;

// ==== Encoder (interrupt-driven, detent counting) ====
volatile int encRaw = 0;
int lastEncDetent = 0;
void IRAM_ATTR encISR() {
  static uint8_t state = 0;
  state = (state << 2) | ((digitalRead(PIN_ENC_B) << 1) | digitalRead(PIN_ENC_A));
  // quadrature: 0b00, 0b01, 0b11, 0b10
  static const int8_t table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  encRaw += table[state & 0x0F];
}

int readEncoderDetents() {
  noInterrupts();
  int raw = encRaw;
  interrupts();
  int detents = raw / 4;
  return detents;
}

// ==== Button handling (debounced, short/long press) ====
struct Button {
  int pin;
  bool lastState = true;
  uint32_t lastChange = 0;
  bool pressed = false;
  bool longPressHandled = false;
  uint32_t downTime = 0;
};
Button btnSW1{PIN_SW1};
Button btnSW2{PIN_SW2};

void pollButton(Button &btn, void (*onShort)(), void (*onLong)()) {
  bool state = digitalRead(btn.pin);
  uint32_t now = millis();
  if (state != btn.lastState) {
    btn.lastChange = now;
    btn.lastState = state;
  }
  // Debounce
  if ((now - btn.lastChange) > 25) {
    if (!state && !btn.pressed) { // pressed (active low)
      btn.pressed = true;
      btn.downTime = now;
      btn.longPressHandled = false;
      // haptic tap on press
      motorPulse(80, 30);
    }
    if (state && btn.pressed) { // released
      btn.pressed = false;
      if (!btn.longPressHandled && (now - btn.downTime) < 800) {
        onShort();
      }
    }
    if (btn.pressed && !btn.longPressHandled && (now - btn.downTime) >= 800) {
      btn.longPressHandled = true;
      onLong();
    }
  }
}

// ==== Motor/haptic ====
uint32_t motorUntil = 0;
void motorPulse(uint8_t duty, uint16_t ms) {
  ledcAttachPin(PIN_MOTOR, 0);
  ledcWriteTone(0, 20000);
  ledcWrite(0, duty);
  motorUntil = millis() + ms;
}
void motorService() {
  if (motorUntil && millis() > motorUntil) {
    ledcWrite(0, 0);
    motorUntil = 0;
  }
}

// ==== Meeting cost meter state ====
enum State { IDLE, RUNNING, PAUSED };
State state = IDLE;

int attendeeCount = 3;
const int attendeeMin = 1;
const int attendeeMax = 20;

float cost = 0.0f; // dollars
uint32_t lastCostUpdate = 0;
const float costPerMinutePerPerson = 1.0f;

// ==== Color helpers ====
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// Interpolate from green to red
uint16_t costBarColor(float norm) {
  // norm: 0.0 (green) to 1.0 (red)
  uint8_t r = 40 + (200 * norm);
  uint8_t g = 200 - (170 * norm);
  uint8_t b = 40;
  return rgb565(r, g, b);
}

// ==== UI layout ====
const uint16_t BG = rgb565(9,10,12);
const uint16_t INK = rgb565(238,240,245);

void drawIdle(LGFX_Sprite &sp, int attendeeCount) {
  sp.fillScreen(BG);
  // Small label
  sp.setTextColor(INK);
  sp.setTextDatum(middle_center);
  sp.setFont(&fonts::Font0);
  sp.setTextSize(1);
  sp.drawString("ATTENDEES", 160, 40);
  // Big number
  sp.setTextSize(4);
  sp.drawString(String(attendeeCount), 160, 90);
  // Subtle hint bar
  sp.setTextSize(1);
  sp.setTextColor(INK, BG);
  sp.drawString("press SW1 to start", 160, 150);
}

void drawBar(LGFX_Sprite &sp, float cost, float norm) {
  sp.fillScreen(BG);
  // Fluid bar: full-bleed, 24px high, 20px margin
  int x0 = 20, x1 = 320-20, y = 85, h = 24;
  int w = x1 - x0;
  int fillw = (int)(w * norm);
  uint16_t col = costBarColor(norm);
  sp.fillRoundRect(x0, y, fillw, h, 12, col);
  // Draw faint outline for rest
  if (fillw < w) {
    sp.drawRoundRect(x0+fillw, y, w-fillw, h, 12, INK);
  }
}

void drawPaused(LGFX_Sprite &sp, float cost, float norm) {
  drawBar(sp, cost, norm);
  sp.setTextColor(INK);
  sp.setTextDatum(middle_center);
  sp.setFont(&fonts::Font0);
  sp.setTextSize(1);
  sp.drawString("PAUSED", 160, 40);
}

// ==== Button actions ====
void onSW1Short() {
  if (state == IDLE) {
    state = RUNNING;
    lastCostUpdate = millis();
    // haptic: longer tap on start
    motorPulse(180, 80);
  } else if (state == RUNNING) {
    state = PAUSED;
    // haptic: double tap
    motorPulse(120, 40);
    delay(60);
    motorPulse(120, 40);
  } else if (state == PAUSED) {
    state = RUNNING;
    lastCostUpdate = millis();
    // haptic: short tap
    motorPulse(80, 30);
  }
}
void onSW1Long() {
  // Reset
  state = IDLE;
  cost = 0.0f;
  // haptic: crisp tap
  motorPulse(255, 30);
}
void onSW2Short() {}
void onSW2Long() {}

// ==== Setup/loop ====
void setup() {
  Serial.begin(115200);
  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_MOTOR, OUTPUT);
  ledcSetup(0, 20000, 8);
  ledcWrite(0, 0);
  attachInterrupt(PIN_ENC_A, encISR, CHANGE);
  attachInterrupt(PIN_ENC_B, encISR, CHANGE);
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);
}

void loop() {
  static uint32_t tNextRender = 0;
  static int lastDetent = 0;
  static int lastAttendee = attendeeCount;
  static float maxCost = 100.0f; // for color/bar normalization

  uint32_t now = millis();

  // Serial input drain
  while (Serial.available()) Serial.read();

  // Encoder: only editable in IDLE
  int detent = readEncoderDetents();
  if (state == IDLE && detent != lastDetent) {
    attendeeCount += (detent - lastDetent);
    if (attendeeCount < attendeeMin) attendeeCount = attendeeMin;
    if (attendeeCount > attendeeMax) attendeeCount = attendeeMax;
    lastDetent = detent;
  }

  // Cost update
  if (state == RUNNING) {
    float rate = costPerMinutePerPerson * attendeeCount / 60.0f; // $/sec
    cost += rate * (now - lastCostUpdate);
    lastCostUpdate = now;
    if (cost > maxCost) maxCost = cost + 10.0f; // auto-expand range
  } else {
    lastCostUpdate = now;
  }

  // Poll buttons
  pollButton(btnSW1, onSW1Short, onSW1Long);
  pollButton(btnSW2, onSW2Short, onSW2Long);

  // Haptic
  motorService();

  // Render
  if (now >= tNextRender) {
    tNextRender = now + 33;
    LGFX_Sprite sp(&tft);
    if (!sp.createSprite(320, 170)) {
      tft.fillScreen(BG);
      return;
    }
    float norm = cost / maxCost;
    if (norm > 1.0f) norm = 1.0f;
    if (state == IDLE) {
      drawIdle(sp, attendeeCount);
    } else if (state == RUNNING) {
      drawBar(sp, cost, norm);
    } else if (state == PAUSED) {
      drawPaused(sp, cost, norm);
    }
    tft.pushSprite(0, 0);
    sp.deleteSprite();
  }
}
