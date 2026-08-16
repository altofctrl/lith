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

LGFX tft;
static LGFX_Sprite sprite(&tft);
static bool sprite_ok = false;

// --- pins ---
constexpr int PIN_SW1 = 1;
constexpr int PIN_SW2 = 2;
constexpr int PIN_ENC_A = 4;
constexpr int PIN_ENC_B = 5;

// --- encoder interrupts ---
volatile int32_t enc_raw = 0;
static int32_t last_enc_val = 0;

void IRAM_ATTR enc_isr() {
  static uint8_t prev = 0;
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  uint8_t state = (a << 1) | b;
  static const int8_t table[16] = {0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
  int8_t step = table[(prev << 2) | state];
  if (step != 0) enc_raw += step;
  prev = state;
}

void setup_encoder() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), enc_isr, CHANGE);
}

// --- button debounce ---
struct Button {
  uint8_t pin;
  bool last_stable = true;
  bool current = true;
  uint32_t stable_since = 0;
  uint32_t press_start = 0;
  bool short_press_flag = false;
  bool long_hold_flag = false;

  void init(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    current = digitalRead(pin);
    last_stable = current;
  }

  void update(uint32_t now) {
    bool raw = digitalRead(pin);
    if (raw != current) {
      current = raw;
      stable_since = now;
    } else if (now - stable_since >= 25) {
      if (raw != last_stable) {
        last_stable = raw;
        if (!raw) {
          press_start = now;
          short_press_flag = false;
          long_hold_flag = false;
        } else {
          if (now - press_start < 800) {
            short_press_flag = true;
          }
          press_start = 0;
        }
      } else if (!raw && press_start > 0 && (now - press_start >= 800)) {
        if (!long_hold_flag) {
          long_hold_flag = true;
        }
      }
    }
  }

  bool short_press() {
    bool tmp = short_press_flag;
    short_press_flag = false;
    return tmp;
  }

  bool long_hold() {
    bool tmp = long_hold_flag;
    long_hold_flag = false;
    return tmp;
  }
};

Button sw1;

// --- state machine ---
enum class State : uint8_t {
  IDLE,
  RUNNING,
  PAUSED
};
State state = State::IDLE;
float rate_per_hour = 150.0f;
float cost = 0.0f;
uint32_t timer_start = 0;
uint32_t elapsed_saved = 0;

// --- display helpers ---
static uint32_t last_render = 0;
constexpr int render_interval = 33;

void draw_idle() {
  sprite.fillScreen(0x0841);
  sprite.setTextColor(0xEEF0F5);
  sprite.setTextDatum(TC_DATUM);
  sprite.setTextSize(3);
  char buf[32];
  snprintf(buf, sizeof(buf), "$%.0f/h", rate_per_hour);
  sprite.setCursor(160, 85);
  sprite.print(buf);
  sprite.setTextSize(1);
  sprite.setCursor(160, 110);
  sprite.print("RATE");
}

void draw_cost(float current_cost, bool breathing) {
  sprite.fillScreen(0x0841);
  sprite.setTextColor(0xEEF0F5);
  sprite.setTextDatum(TC_DATUM);
  sprite.setTextSize(3);
  char buf[32];
  snprintf(buf, sizeof(buf), "$%.2f", current_cost);
  sprite.setCursor(160, 85);
  sprite.print(buf);
  sprite.setTextSize(1);
  sprite.setCursor(160, 110);
  sprite.print("COST");

  if (breathing) {
    uint32_t elapsed = (millis() - timer_start) / 1000;
    float breath = sin(elapsed * 2.0f) * 0.5f + 0.5f;
    int r = 4 + (int)(breath * 6);
    sprite.fillCircle(210, 20, r, 0xEEF0F5);
  }
}

void render(uint32_t now) {
  if (!sprite_ok) return;
  if (now - last_render < render_interval) return;
  last_render = now;

  switch (state) {
    case State::IDLE:
      draw_idle();
      break;
    case State::RUNNING: {
      uint32_t elapsed_now = (now - timer_start) / 1000;
      float total_seconds = elapsed_saved + elapsed_now;
      float current = (rate_per_hour / 3600.0f) * total_seconds;
      draw_cost(current, true);
      break;
    }
    case State::PAUSED:
      draw_cost(cost, false);
      break;
  }
  sprite.pushSprite(0,0);
}

void update_cost() {
  uint32_t now = millis();
  if (state == State::RUNNING) {
    uint32_t elapsed_now = (now - timer_start) / 1000;
    float total_seconds = elapsed_saved + elapsed_now;
    cost = (rate_per_hour / 3600.0f) * total_seconds;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Meeting cost meter ===");

  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);

  if (!sprite.createSprite(320, 170)) {
    Serial.println("Failed to allocate sprite");
    sprite_ok = false;
  } else {
    sprite_ok = true;
    sprite.setFont(&fonts::Font0);
  }

  sw1.init(PIN_SW1);
  setup_encoder();

  state = State::IDLE;
  elapsed_saved = 0;
  cost = 0;
}

void loop() {
  uint32_t now = millis();

  noInterrupts();
  int32_t current_enc = enc_raw;
  interrupts();
  int32_t delta = current_enc - last_enc_val;
  last_enc_val = current_enc;
  if (delta != 0) {
    int32_t detents = delta / 4;
    for (int32_t i = 0; i < abs(detents); i++) {
      if (detents > 0) rate_per_hour += 5;
      else rate_per_hour -= 5;
      if (rate_per_hour < 0) rate_per_hour = 0;
    }
    if (state == State::IDLE) {
      Serial.printf("Rate: $%.0f/h\n", rate_per_hour);
    }
  }

  sw1.update(now);

  if (sw1.short_press()) {
    switch (state) {
      case State::IDLE:
        state = State::RUNNING;
        timer_start = now;
        elapsed_saved = 0;
        Serial.println("Started meeting");
        break;
      case State::RUNNING:
        elapsed_saved += (now - timer_start) / 1000;
        cost = (rate_per_hour / 3600.0f) * elapsed_saved;
        state = State::PAUSED;
        Serial.println("Paused");
        break;
      case State::PAUSED:
        state = State::RUNNING;
        timer_start = now;
        Serial.println("Resumed");
        break;
    }
  }

  if (sw1.long_hold()) {
    state = State::IDLE;
    cost = 0;
    elapsed_saved = 0;
    timer_start = 0;
    Serial.println("Reset");
  }

  update_cost();
  render(now);
}
