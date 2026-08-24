#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>

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
Preferences prefs;

const int PIN_SW1 = 1;
const int PIN_ENC_A = 4;
const int PIN_ENC_B = 5;
const int PIN_MOTOR = 6;

volatile int encoderPos = 0;
int lastEncoderPos = 0;
unsigned long meetingStartTime = 0;
unsigned long meetingDuration = 0;
bool meetingActive = false;

void IRAM_ATTR onEncoderChange() {
  if (digitalRead(PIN_ENC_A) == digitalRead(PIN_ENC_B)) {
    encoderPos++;
  } else {
    encoderPos--;
  }
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);
  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), onEncoderChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), onEncoderChange, CHANGE);
  prefs.begin("meeting_cost", false);
  meetingDuration = prefs.getUInt("duration", 1);
}

void loop() {
  if (digitalRead(PIN_SW1) == LOW) {
    if (!meetingActive) {
      meetingStartTime = millis();
      meetingActive = true;
    } else {
      meetingActive = false;
      prefs.putUInt("duration", meetingDuration);
    }
    delay(800); // debounce
  }

  if (meetingActive) {
    unsigned long elapsed = (millis() - meetingStartTime) / 60000; // minutes
    unsigned long totalCost = elapsed * meetingDuration;
    tft.fillScreen(0x000000);
    tft.setTextSize(6);
    tft.setTextColor(0xEEEEEE);
    tft.setCursor(10, 60);
    tft.printf("Cost: $%lu", totalCost);
    tft.setTextSize(2);
    tft.setCursor(10, 130);
    tft.printf("Duration: %lu min", elapsed);
  } else {
    tft.fillScreen(0x000000);
    tft.setTextSize(6);
    tft.setTextColor(0xEEEEEE);
    tft.setCursor(10, 60);
    tft.printf("Meeting Cost Meter");
  }
  delay(100);
}