#include <LovyanGFX.hpp>
#define LGFX_USE_V1

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
unsigned long meetingStart;
unsigned long meetingCost = 0;
const unsigned long meetingCostInterval = 1000; // 1 second
const unsigned long meetingCostPerInterval = 1; // $1 per second

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);
  meetingStart = millis();
  ledcSetup(0, 20000, 8);
  ledcAttachPin(6, 0);
}

void loop() {
  if (millis() - meetingStart >= meetingCostInterval) {
    meetingCost += meetingCostPerInterval;
    meetingStart = millis();
    ledcWrite(0, 190);
    delay(50);
    ledcWrite(0, 0);
  }
  tft.fillScreen(0x090A0C);
  tft.setTextColor(0xEEF0F5);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("$");
  tft.print(meetingCost);
  tft.display);
  delay(1000);
}
