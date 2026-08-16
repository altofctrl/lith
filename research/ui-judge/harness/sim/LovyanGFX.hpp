// Shim for <LovyanGFX.hpp>, placed ahead of the real one on the include path.
//
// A generated sketch opens with the device_profile display_init snippet
// verbatim: it declares `class LGFX : public lgfx::LGFX_Device` holding a
// Panel_ST7789 on a Bus_SPI with a Light_PWM, configures all three from real
// pin numbers, and calls setPanel(). All of that has to compile and run
// unchanged, because the object of study is the sketch as the model wrote it.
//
// Only one thing is actually replaced: where the pixels go. Panel_ST7789 here
// is a real lgfx::Panel_FrameBufferBase -- a genuine Panel_Device -- backed by
// RAM instead of an SPI bus. Everything else is the library the device runs:
// the real LGFX_Device, the real drawing and clipping, the real glyph
// rasteriser, the real RGB565 quantisation, and the real rotation maths, which
// still reads the sketch's own offset_rotation out of its own panel config.
// What lands in the buffer is the sketch's pixels, not a lookalike.
//
// The bus and the backlight become inert config holders: there is no SPI to
// drive and no LED to dim, and neither shows up in a frame.
#pragma once

#ifndef LGFX_USE_V1
#define LGFX_USE_V1
#endif

#include "lgfx/v1/LGFXBase.hpp"
#include "lgfx/v1/LGFX_Sprite.hpp"
#include "lgfx/v1/lgfx_fonts.hpp"
#include "lgfx/v1/panel/Panel_FrameBufferBase.hpp"

// The panel in its own memory orientation, straight from device_profile.json:
// 170 wide by 320 tall, which setRotation(1) turns into the 320x170 landscape
// the owner looks at.
#define SIM_PANEL_MEM_W 170
#define SIM_PANEL_MEM_H 320

namespace lgfx {
inline namespace v1 {

// ------------------------------------------------------- inert peripherals
struct sim_bus_cfg {
  int spi_host = 0, spi_mode = 0;
  uint32_t freq_write = 40000000, freq_read = 16000000;
  bool spi_3wire = false, use_lock = true;
  int dma_channel = 0;
  int pin_sclk = -1, pin_mosi = -1, pin_miso = -1, pin_dc = -1;
  int i2c_port = 0, pin_scl = -1, pin_sda = -1, i2c_addr = 0;
  int prefix_cmd = 0, prefix_data = 0, prefix_len = 0;
};
struct sim_light_cfg {
  int pin_bl = -1;
  bool invert = false;
  uint32_t freq = 44100;
  uint8_t pwm_channel = 7;
};

class Bus_SPI {
  sim_bus_cfg _c;
public:
  sim_bus_cfg config() const { return _c; }
  void config(const sim_bus_cfg &c) { _c = c; }
};
using Bus_I2C = Bus_SPI;

class Light_PWM {
  sim_light_cfg _c;
public:
  sim_light_cfg config() const { return _c; }
  void config(const sim_light_cfg &c) { _c = c; }
};

// ------------------------------------------------------------- the panel
// Tracked so the harness can report a build that never called init(), or that
// forgot setRotation and would therefore have drawn portrait on a landscape
// panel -- both real defects, and both invisible in a frame.
extern bool    g_sim_rotation_set;
extern uint8_t g_sim_rotation;
extern bool    g_sim_init_called;

class Panel_FB;
// The harness cannot know what the sketch called its display object -- `tft` is
// only a convention and some builds use `lcd` or `display` -- so the panel
// announces itself instead of being looked up by name.
extern void sim_register_panel(Panel_FB *);

class Panel_FB : public Panel_FrameBufferBase {
public:
  Panel_FB() {
    _cfg.memory_width  = _cfg.panel_width  = SIM_PANEL_MEM_W;
    _cfg.memory_height = _cfg.panel_height = SIM_PANEL_MEM_H;
    sim_register_panel(this);
  }
  ~Panel_FB() { _free_lines(); }

  // setBus/setLight take the inert holders above rather than a real IBus/ILight,
  // so the display_init snippet's `_panel.setBus(&_bus)` still compiles.
  template <typename T> void setBus(T *) {}
  template <typename T> void setLight(T *) {}

  // Does what Panel_FrameBufferBase::init does, minus its call up to
  // Panel_Device::init. That base does the one thing this panel cannot: it
  // drives the reset line, the backlight and then `_bus->init()`. There is no
  // bus here -- setBus above is a no-op, so `_bus` is null and the call is a
  // straight dereference of nothing. Everything Panel_Device::init would
  // otherwise do (rst_control, the DMA buffer reserve, the two delays) is
  // hardware bring-up with no bearing on a pixel, so skipping it loses nothing
  // that could show up in a frame.
  bool init(bool use_reset) override {
    (void)use_reset;
    // The sketch has configured the panel by now, so size the buffer from what
    // it actually asked for rather than from the default.
    if (_cfg.memory_width  < _cfg.panel_width)  _cfg.memory_width  = _cfg.panel_width;
    if (_cfg.memory_height < _cfg.panel_height) _cfg.memory_height = _cfg.panel_height;
    _alloc_lines();
    g_sim_init_called = true;

    _range_mod.top    = INT16_MAX;
    _range_mod.left   = INT16_MAX;
    _range_mod.right  = 0;
    _range_mod.bottom = 0;
    setInvert(_invert);
    _in_init = true;
    setRotation(_rotation);
    _in_init = false;
    return true;
  }

  // Read-back needs the panel's own depth and palette; both are protected on
  // Panel_Device, and the harness reads through readRect exactly as
  // LGFXBase::readPixel does.
  color_depth_t read_depth() const { return _read_depth; }
  bool          has_buffer() const { return _lines_buffer != nullptr; }

  // The rotation flag has to mean "the sketch asked for one", so the
  // re-assertion init makes of the rotation already in place does not count.
  void setRotation(uint_fast8_t r) override {
    if (!_in_init) {
      g_sim_rotation_set = true;
      g_sim_rotation = (uint8_t)r;
    }
    Panel_FrameBufferBase::setRotation(r);
  }

  // The remaining routes to the bus, which is not here.
  void initBus() override {}
  void releaseBus() override {}
  void writeCommand(uint32_t, uint_fast8_t) override {}
  void writeData(uint32_t, uint_fast8_t) override {}

  uint16_t mem_w() const { return _cfg.memory_width; }
  uint16_t mem_h() const { return _cfg.memory_height; }

private:
  uint16_t _alloc_h = 0;
  bool     _in_init = false;

  void _free_lines() {
    if (!_lines_buffer) return;
    for (uint16_t i = 0; i < _alloc_h; i++) free(_lines_buffer[i]);
    free(_lines_buffer);
    _lines_buffer = nullptr;
    _alloc_h = 0;
  }
  void _alloc_lines() {
    _free_lines();
    _alloc_h = _cfg.memory_height;
    size_t stride = (size_t)_cfg.memory_width * ((_write_depth & color_depth_t::bit_mask) >> 3);
    if (stride == 0) stride = (size_t)_cfg.memory_width * 2;
    _lines_buffer = (uint8_t **)calloc(_alloc_h, sizeof(uint8_t *));
    for (uint16_t i = 0; i < _alloc_h; i++)
      _lines_buffer[i] = (uint8_t *)calloc(1, stride);
  }
};

// Every panel type a generated sketch might reach for lands on the same RAM
// framebuffer; the driver chip is exactly the part that cannot exist here.
using Panel_ST7789  = Panel_FB;
using Panel_ST7735  = Panel_FB;
using Panel_ST7735S = Panel_FB;
using Panel_ST7796  = Panel_FB;
using Panel_ILI9341 = Panel_FB;
using Panel_GC9A01  = Panel_FB;

}  // namespace v1
}  // namespace lgfx

using lgfx::LGFX_Sprite;
using lgfx::LovyanGFX;
using lgfx::LGFX_Device;
using lgfx::textdatum_t;
using lgfx::color_depth_t;
