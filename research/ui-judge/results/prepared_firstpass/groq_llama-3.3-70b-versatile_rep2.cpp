#include <LovyanGFX.hpp>
#define LGFX_USE_V1

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
public:
  LGFX() {
    { auto cfg = _bus.config();
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
    { auto cfg = _panel.config();
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
    { auto cfg = _light.config();
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
unsigned long meetingStart;
unsigned long lastUpdate;
const unsigned long updateInterval = 1000;
const float hourlyRate = 100.0;
const float costPerSecond = hourlyRate / 3600.0;
float currentCost = 0.0;

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);
  meetingStart = millis();
  lastUpdate = meetingStart;
}

void loop() {
  unsigned long now = millis();
  if (now - lastUpdate >= updateInterval) {
    lastUpdate = now;
    currentCost += costPerSecond * (now - meetingStart) / 1000.0;
    meetingStart = now;
    tft.fillScreen(0x090A0C);
    tft.setTextColor(0xEEF0F2);
    tft.setTextSize(4);
    char costStr[10];
    dtostrf(currentCost, 6, 2, costStr);
    tft.setCursor(50, 80);
    tft.print("$");
    tft.print(costStr);
    tft.display);
  }
}
