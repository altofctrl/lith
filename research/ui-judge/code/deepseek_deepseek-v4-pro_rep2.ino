#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>

// --- Hardware pins (from lith ground truth) ---
#define PIN_SW1     1
#define PIN_SW2     2
#define PIN_ENC_A   4
#define PIN_ENC_B   5
// Motor not used in this silent build

// --- Display class (verbatim from display_init snippet) ---
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 50000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = -1;
      cfg.pin_dc = 13;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 10;
      cfg.pin_rst = 9;
      cfg.pin_busy = -1;
      cfg.panel_width = 170;
      cfg.panel_height = 320;
      cfg.offset_x = 35;
      cfg.offset_y = 0;
      cfg.offset_rotation = 2;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 8;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX tft;
LGFX_Sprite canvas(&tft);

// --- Colours (RGB565) ---
#define BG_COLOUR     0x1086   // 9,10,12
#define INK_COLOUR    0xEFB5   // 238,240,245

// --- Encoder (interrupt-driven quadrature) ---
volatile int32_t encRaw = 0;
static int8_t encTable[16] = {0,1,-1,0,-1,0,0,1,1,0,0,-1,0,-1,1,0};
static uint8_t encState = 0;
void IRAM_ATTR encISR() {
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  encState = ((encState << 2) | (a << 1) | b) & 0x0F;
  encRaw += encTable[encState];
}

// --- Button state machines ---
struct Button {
  uint8_t pin;
  bool lastStable = HIGH;
  uint32_t changeMs = 0;
  uint32_t pressStart = 0;
  bool longPressed = false;
  bool shortPress = false;
  bool longPress = false;
} btnSW1{ PIN_SW1 }, btnSW2{ PIN_SW2 };

void pollButton(Button &b) {
  bool raw = digitalRead(b.pin);
  uint32_t now = millis();
  if (raw != b.lastStable) {
    if (now - b.changeMs > 25) {
      b.lastStable = raw;
      b.changeMs = now;
      if (raw == LOW) {             // active low press
        b.pressStart = now;
        b.longPressed = false;
      }
    }
  }
  b.shortPress = false;
  b.longPress  = false;
  if (b.lastStable == LOW) {
    if (!b.longPressed && (now - b.pressStart > 800)) {
      b.longPress = true;
      b.longPressed = true;
    }
  } else {
    if (!b.longPressed && (b.pressStart > 0) && (now - b.pressStart < 800)) {
      b.shortPress = true;
    }
  }
  if (b.lastStable == HIGH) {
    b.pressStart = 0;
  }
}

// --- App state ---
enum State { IDLE, RUNNING, PAUSED } appState = IDLE;
Preferences prefs;
int rateDollars = 150;            // $/hr, persisted in NVS
uint64_t totalCents = 0;          // accumulated cost in cents (0.01 USD)
uint32_t runStartMs = 0;          // when current running segment began (ms)
uint64_t prePauseCents = 0;      // totalCents at last pause

// --- Layout tuning ---
int labelY = 30;
int numberY = 100;

void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);
  
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);
  canvas.setColorDepth(16);
  canvas.createSprite(320, 170);
  
  prefs.begin("meeting", false);
  rateDollars = prefs.getInt("rate", 150);
  if (rateDollars < 10) rateDollars = 10;
  if (rateDollars > 2000) rateDollars = 2000;
  prefs.putInt("rate", rateDollars);
  prefs.end();
  
  Serial.printf("Meeting cost meter, rate $%d/hr\n", rateDollars);
}

int32_t lastDetent = 0;          // detents from encoder
int32_t readEncoderDetent() {
  int32_t raw;
  noInterrupts();
  raw = encRaw;
  interrupts();
  int32_t det = raw / 4;         // 4 edges per detent
  int32_t delta = 0;
  if (det != lastDetent) {
    delta = det - lastDetent;
    lastDetent = det;
  }
  return delta;
}

void updateCost() {
  if (appState == RUNNING) {
    uint32_t now = millis();
    uint32_t elapsed = now - runStartMs;
    uint64_t newCents = prePauseCents + (uint64_t)elapsed * rateDollars * 100ULL / 3600000ULL;
    totalCents = newCents;
  }
}

void handleButtons() {
  pollButton(btnSW1);
  pollButton(btnSW2);
  
  if (btnSW1.shortPress) {
    switch (appState) {
      case IDLE:   appState = RUNNING; runStartMs = millis(); prePauseCents = 0; totalCents = 0; Serial.println("Meeting started"); break;
      case RUNNING: appState = PAUSED; prePauseCents = totalCents; Serial.println("Paused"); break;
      case PAUSED: appState = RUNNING; runStartMs = millis(); Serial.println("Resumed"); break;
    }
  }
  if (btnSW1.longPress) {
    if (appState != IDLE) {
      appState = IDLE; totalCents = 0; Serial.println("Reset to idle");
    }
  }
  if (btnSW2.shortPress) {
    appState = IDLE; totalCents = 0; Serial.println("Stopped");
  }
  
  if (appState == IDLE) {
    int32_t delta = readEncoderDetent();
    if (delta != 0) {
      rateDollars += delta;
      if (rateDollars < 10) rateDollars = 10;
      if (rateDollars > 2000) rateDollars = 2000;
      prefs.begin("meeting", false);
      prefs.putInt("rate", rateDollars);
      prefs.end();
      Serial.printf("Rate adjusted to $%d/hr\n", rateDollars);
    }
  }
}

void drawCenteredText(LGFX_Sprite &spr, const char *text, int y, float scale, uint16_t col) {
  int textW = spr.textWidth(text) * scale;
  int x = (320 - textW) / 2;
  spr.setTextSize(scale);
  spr.setCursor(x, y);
  spr.setTextColor(col);
  spr.print(text);
}

void render() {
  canvas.fillSprite(BG_COLOUR);
  canvas.setFont(&fonts::Font2);   // built-in small proportional font
  canvas.setTextDatum(textdatum_t::top_left);
  
  if (appState == IDLE) {
    char buf[20];
    snprintf(buf, sizeof(buf), "RATE/HOUR");
    canvas.setTextSize(1);
    int labelW = canvas.textWidth(buf);
    canvas.setCursor((320 - labelW) / 2, labelY);
    canvas.setTextColor(INK_COLOUR);
    canvas.print(buf);
    
    snprintf(buf, sizeof(buf), "$%d", rateDollars);
    drawCenteredText(canvas, buf, numberY+20, 3.5f, INK_COLOUR);
  } else {
    char label[12] = "COST";
    if (appState == PAUSED) strcpy(label, "PAUSED");
    canvas.setTextSize(1);
    int labelW = canvas.textWidth(label);
    canvas.setCursor((320 - labelW) / 2, labelY);
    canvas.setTextColor(INK_COLOUR);
    canvas.print(label);
    
    char cost[15];
    uint64_t dollars = totalCents / 100;
    uint64_t cents = totalCents % 100;
    snprintf(cost, sizeof(cost), "$%llu.%02llu", dollars, cents);
    drawCenteredText(canvas, cost, numberY+10, 2.5f, INK_COLOUR);
  }
  
  canvas.pushSprite(0, 0);
}

void loop() {
  static uint32_t nextRender = 0;
  handleButtons();
  updateCost();
  
  uint32_t now = millis();
  if (now >= nextRender) {
    nextRender = now + 33;   // ~30 fps
    render();
  }
  
  // Drain serial
  while (Serial.available()) {
    Serial.read();
  }
}
