#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>

// --- Display Setup (from hardware ground truth) ---
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

// --- Pin Assignments (from hardware ground truth) ---
constexpr int PIN_SW1 = 1;
constexpr int PIN_SW2 = 2;
constexpr int PIN_ENC_A = 4;
constexpr int PIN_ENC_B = 5;
constexpr int PIN_MOTOR = 6;

// --- Motor (haptic) setup ---
constexpr int MOTOR_PWM_FREQ = 20000;
constexpr int MOTOR_PWM_CHANNEL = 1;
constexpr int MOTOR_DUTY_TAP = 255;  // crisp tap
constexpr int MOTOR_DUTY_BUZZ = 190; // gentle buzz

// --- UI Colors ---
constexpr uint16_t COLOR_BG = (9<<11)|(10<<5)|(12); // near-black RGB565
constexpr uint16_t COLOR_ACCENT = 0; // placeholder, dynamic
constexpr uint16_t COLOR_INK = (238>>3<<11)|(240>>2<<5)|(245>>3); // off-white ink, not used here

// --- State ---
enum MeterState { IDLE, RUNNING, PAUSED };
volatile MeterState meterState = IDLE;
unsigned long meterStartMillis = 0;
unsigned long meterElapsedMillis = 0;

// --- Button Debounce/Long Press ---
struct ButtonState {
  bool pressed = false;
  bool lastPhysical = false;
  unsigned long lastChange = 0;
  unsigned long pressedAt = 0;
  bool longPressSent = false;
};
ButtonState sw1;

constexpr unsigned long DEBOUNCE_MS = 25;
constexpr unsigned long LONGPRESS_MS = 800;

// --- Motor Pulse ---
unsigned long motorPulseUntil = 0;
uint8_t motorPulseDuty = 0;

void motorPulse(uint8_t duty, unsigned long ms) {
  motorPulseDuty = duty;
  motorPulseUntil = millis() + ms;
  ledcWrite(MOTOR_PWM_CHANNEL, duty);
}

void motorService() {
  if (motorPulseDuty && millis() > motorPulseUntil) {
    motorPulseDuty = 0;
    ledcWrite(MOTOR_PWM_CHANNEL, 0);
  }
}

// --- Button Handling ---
void pollButtonSW1() {
  bool physical = !digitalRead(PIN_SW1); // active low
  unsigned long now = millis();
  if (physical != sw1.lastPhysical) {
    sw1.lastPhysical = physical;
    sw1.lastChange = now;
  }
  if ((now - sw1.lastChange) > DEBOUNCE_MS) {
    if (physical != sw1.pressed) {
      sw1.pressed = physical;
      if (physical) {
        sw1.pressedAt = now;
        sw1.longPressSent = false;
        motorPulse(MOTOR_DUTY_BUZZ, 30); // haptic on down
      } else {
        // Button released
        if (!sw1.longPressSent) {
          // Short press
          onSW1ShortPress();
        }
      }
    }
    // Long press
    if (sw1.pressed && !sw1.longPressSent && (now - sw1.pressedAt > LONGPRESS_MS)) {
      sw1.longPressSent = true;
      onSW1LongPress();
    }
  }
}

// --- Button Actions ---
void onSW1ShortPress() {
  if (meterState == IDLE || meterState == PAUSED) {
    meterState = RUNNING;
    meterStartMillis = millis();
    // don't reset elapsed here, so pause/resume works
    motorPulse(MOTOR_DUTY_TAP, 40);
  } else if (meterState == RUNNING) {
    meterState = PAUSED;
    meterElapsedMillis += millis() - meterStartMillis;
    motorPulse(MOTOR_DUTY_TAP, 40);
  }
}

void onSW1LongPress() {
  meterState = IDLE;
  meterElapsedMillis = 0;
  motorPulse(MOTOR_DUTY_BUZZ, 120);
}

// --- Animation ---
LGFX tft;
LGFX_Sprite sprite;
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 170;

unsigned long lastFrame = 0;
constexpr unsigned long FRAME_MS = 30; // ~33 fps

// HSV to RGB565 (simple, not gamma-corrected)
uint16_t hsv2rgb565(float h, float s, float v) {
  float r, g, b;
  int i = int(h * 6);
  float f = h * 6 - i;
  float p = v * (1 - s);
  float q = v * (1 - f * s);
  float t = v * (1 - (1 - f) * s);
  switch(i % 6) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    case 5: r = v, g = p, b = q; break;
  }
  uint8_t R = uint8_t(r * 255);
  uint8_t G = uint8_t(g * 255);
  uint8_t B = uint8_t(b * 255);
  return ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3);
}

void drawAmbientAnimation(unsigned long t, MeterState state, unsigned long elapsed) {
  sprite.fillScreen(COLOR_BG);
  if (state == IDLE) {
    // Calm: just a faint dark vignette
    for (int y = 0; y < SCREEN_H; ++y) {
      float dy = (y - SCREEN_H/2) / float(SCREEN_H/2);
      float v = 1.0f - 0.08f * dy * dy;
      uint16_t c = hsv2rgb565(0.62f, 0.08f, v * 0.15f); // very faint blue
      sprite.drawFastHLine(0, y, SCREEN_W, c);
    }
    return;
  }
  // RUNNING or PAUSED: animated color wave
  float baseHue = 0.10f + 0.25f * fmod((elapsed + (state == RUNNING ? (t - meterStartMillis) : 0)) / 60000.0f, 1.0f); // hue shifts gently over time
  float speed = 0.5f + 0.5f * sinf((elapsed + (state == RUNNING ? (t - meterStartMillis) : 0)) / 4000.0f); // gentle breathing
  for (int y = 0; y < SCREEN_H; ++y) {
    float wave = sinf(2.0f * 3.14159f * (y / float(SCREEN_H) + speed * t / 8000.0f));
    float h = fmod(baseHue + 0.08f * wave, 1.0f);
    float s = 0.25f + 0.15f * wave;
    float v = 0.18f + 0.12f * (0.5f + 0.5f * wave);
    uint16_t c = hsv2rgb565(h, s, v);
    sprite.drawFastHLine(0, y, SCREEN_W, c);
  }
  // If PAUSED, overlay a subtle dimming
  if (state == PAUSED) {
    sprite.fillRect(0, 0, SCREEN_W, SCREEN_H, hsv2rgb565(baseHue, 0.10f, 0.08f));
  }
}

// --- Setup/Loop ---
void setup() {
  Serial.begin(115200);
  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  // Motor (LEDC)
  ledcAttachPin(PIN_MOTOR, MOTOR_PWM_CHANNEL);
  ledcSetup(MOTOR_PWM_CHANNEL, MOTOR_PWM_FREQ, 8);
  ledcWrite(MOTOR_PWM_CHANNEL, 0);
  // Display
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);
  sprite.setColorDepth(16);
  if (!sprite.createSprite(SCREEN_W, SCREEN_H)) {
    sprite.setPsram(true);
    sprite.createSprite(SCREEN_W, SCREEN_H);
  }
  sprite.setPaletteGrayscale();
  sprite.fillScreen(COLOR_BG);
  tft.fillScreen(COLOR_BG);
  tft.pushSprite(&sprite, 0, 0);
}

void loop() {
  unsigned long now = millis();
  // Drain serial input
  while (Serial.available()) Serial.read();
  pollButtonSW1();
  motorService();
  // Animation timing
  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    unsigned long elapsed = meterElapsedMillis;
    if (meterState == RUNNING) {
      elapsed += now - meterStartMillis;
    }
    drawAmbientAnimation(now, meterState, elapsed);
    tft.pushSprite(&sprite, 0, 0);
  }
}
