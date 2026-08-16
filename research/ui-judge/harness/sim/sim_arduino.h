// Host stand-ins for the Arduino / ESP32 surface that an Oldowan-generated
// sketch touches, so an arbitrary knapp .ino can be compiled and run on a PC.
//
// This is a generalisation of lith's own tools/sim/sim_arduino.h. That one only
// had to carry the stock firmware, whose peripherals could all be dead stubs
// (buttons never press, the encoder never turns). Here the whole point is to
// drive a build to six journey states, so the inputs are scriptable: pins hold
// real state, interrupts really fire, and the encoder emits real quadrature.
//
// millis() stays virtual time the harness sets, for the same reason as before:
// every animation is a function of it, so frames must be reproducible.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <math.h>
#include <string>
#include <map>

// ---------------------------------------------------------------- time
extern uint32_t g_sim_millis;
static inline uint32_t millis() { return g_sim_millis; }
static inline uint32_t micros() { return g_sim_millis * 1000u; }

// delay() advances virtual time rather than sleeping. A sketch that delays
// inside loop() is pacing itself, and if that did not move the clock the
// harness would spin forever without the UI ever changing.
void sim_advance(uint32_t ms);
static inline void delay(uint32_t ms) { sim_advance(ms); }
static inline void delayMicroseconds(uint32_t us) { sim_advance(us / 1000u); }
static inline void yield() {}

// ---------------------------------------------------------------- gpio
#define LOW      0
#define HIGH     1
#define INPUT    0
#define OUTPUT   1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define RISING   1
#define FALLING  2
#define CHANGE   3
#define IRAM_ATTR
#define ICACHE_RAM_ATTR

#define SIM_NPINS 64
extern uint8_t  g_pin[SIM_NPINS];              // current level, HIGH = released
extern void   (*g_isr[SIM_NPINS])();
extern uint8_t  g_isr_mode[SIM_NPINS];

static inline void pinMode(int, int) {}
static inline int  digitalRead(int p) {
  return (p >= 0 && p < SIM_NPINS) ? g_pin[p] : HIGH;
}
static inline void digitalWrite(int, int) {}
static inline int  analogRead(int) { return 0; }
static inline int  digitalPinToInterrupt(int p) { return p; }
static inline void attachInterrupt(int p, void (*fn)(), int mode) {
  if (p >= 0 && p < SIM_NPINS) { g_isr[p] = fn; g_isr_mode[p] = (uint8_t)mode; }
}
static inline void detachInterrupt(int p) {
  if (p >= 0 && p < SIM_NPINS) g_isr[p] = nullptr;
}
static inline void noInterrupts() {}
static inline void interrupts() {}

// Set a pin and fire whatever ISR the sketch attached to it, honouring the
// edge it asked for -- an encoder decoded in an ISR sees the same edges it
// would on the device.
void sim_set_pin(int p, int level);

// ---------------------------------------------------------------- maths
#ifndef PI
#define PI 3.1415926535897932384626433832795f
#endif
template <typename T, typename L, typename H>
static inline T constrain(T v, L lo, H hi) {
  return v < (T)lo ? (T)lo : v > (T)hi ? (T)hi : v;
}
static inline long map(long x, long a, long b, long c, long d) {
  return b == a ? c : (x - a) * (d - c) / (b - a) + c;
}
// AVR-era float formatter the ESP32 core still provides; sketches reach for it
// when they want a fixed number of decimals without printf.
static inline char *dtostrf(double val, signed char width, unsigned char prec,
                            char *out) {
  char fmt[24];
  snprintf(fmt, sizeof fmt, "%%%d.%uf", (int)width, (unsigned)prec);
  sprintf(out, fmt, val);
  return out;
}

static inline long sim_random(long hi) { return hi ? (long)(rand() % hi) : 0; }
static inline long sim_random(long lo, long hi) {
  return hi > lo ? lo + (long)(rand() % (hi - lo)) : lo;
}
#define random sim_random
static inline void randomSeed(unsigned) {}

// ---------------------------------------------------------------- ledc / pwm
// The motor is a counter: silent builds are part of what is being studied
// ("silent, just the light" is turn 2 of the brief), so how much a build
// buzzes is worth recording even though it cannot be seen in a frame.
extern uint32_t g_motor_writes;
extern uint32_t g_motor_last_duty;
static inline void ledcWrite(int pin, uint32_t duty) {
  if (duty) { g_motor_writes++; g_motor_last_duty = duty; }
  (void)pin;
}
static inline bool ledcAttach(int, uint32_t, uint8_t) { return true; }
static inline bool ledcAttachChannel(int, uint32_t, uint8_t, uint8_t) { return true; }
static inline void ledcSetup(int, uint32_t, uint8_t) {}
static inline void ledcAttachPin(int, int) {}
static inline void ledcDetachPin(int) {}

// ---------------------------------------------------------------- critical
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static inline void portENTER_CRITICAL(portMUX_TYPE *) {}
static inline void portEXIT_CRITICAL(portMUX_TYPE *) {}
static inline void portENTER_CRITICAL_ISR(portMUX_TYPE *) {}
static inline void portEXIT_CRITICAL_ISR(portMUX_TYPE *) {}

// ---------------------------------------------------------------- esp misc
#define SPI2_HOST 1
#define SPI3_HOST 2
#define SPI_DMA_CH_AUTO 3
typedef int esp_err_t;
#define ESP_OK 0
struct EspClass {
  uint32_t getFreeHeap() { return 300000; }
  uint32_t getMinFreeHeap() { return 280000; }
  uint32_t getPsramSize() { return 0; }
  void restart() {}
};
static EspClass ESP;

// ---------------------------------------------------------------- preferences
// In-memory, so a build that persists a running total still reads back what it
// wrote inside one journey.
class Preferences {
  std::map<std::string, double> _v;
public:
  bool begin(const char *, bool = false) { return true; }
  void end() {}
  void clear() { _v.clear(); }
  uint32_t getUInt(const char *k, uint32_t d = 0) { auto i = _v.find(k); return i == _v.end() ? d : (uint32_t)i->second; }
  int32_t  getInt (const char *k, int32_t d = 0)  { auto i = _v.find(k); return i == _v.end() ? d : (int32_t)i->second; }
  float    getFloat(const char *k, float d = 0)   { auto i = _v.find(k); return i == _v.end() ? d : (float)i->second; }
  double   getDouble(const char *k, double d = 0) { auto i = _v.find(k); return i == _v.end() ? d : i->second; }
  bool     getBool(const char *k, bool d = false) { auto i = _v.find(k); return i == _v.end() ? d : i->second != 0; }
  uint8_t  getUChar(const char *k, uint8_t d = 0) { auto i = _v.find(k); return i == _v.end() ? d : (uint8_t)i->second; }
  int8_t   getChar(const char *k, int8_t d = 0)   { auto i = _v.find(k); return i == _v.end() ? d : (int8_t)i->second; }
  uint16_t getUShort(const char *k, uint16_t d = 0) { auto i = _v.find(k); return i == _v.end() ? d : (uint16_t)i->second; }
  int16_t  getShort(const char *k, int16_t d = 0) { auto i = _v.find(k); return i == _v.end() ? d : (int16_t)i->second; }
  uint64_t getULong64(const char *k, uint64_t d = 0) { auto i = _v.find(k); return i == _v.end() ? d : (uint64_t)i->second; }
  int64_t  getLong64(const char *k, int64_t d = 0) { auto i = _v.find(k); return i == _v.end() ? d : (int64_t)i->second; }
  uint32_t getULong(const char *k, uint32_t d = 0) { return getUInt(k, d); }
  int32_t  getLong(const char *k, int32_t d = 0)   { return getInt(k, d); }
  size_t putUInt(const char *k, uint32_t v) { _v[k] = v; return 4; }
  size_t putInt (const char *k, int32_t v)  { _v[k] = v; return 4; }
  size_t putFloat(const char *k, float v)   { _v[k] = v; return 4; }
  size_t putDouble(const char *k, double v) { _v[k] = v; return 8; }
  size_t putBool(const char *k, bool v)     { _v[k] = v; return 1; }
  size_t putUChar(const char *k, uint8_t v) { _v[k] = v; return 1; }
  size_t putChar(const char *k, int8_t v)   { _v[k] = v; return 1; }
  size_t putUShort(const char *k, uint16_t v) { _v[k] = v; return 2; }
  size_t putShort(const char *k, int16_t v) { _v[k] = v; return 2; }
  size_t putULong(const char *k, uint32_t v) { _v[k] = v; return 4; }
  size_t putLong(const char *k, int32_t v)  { _v[k] = v; return 4; }
  size_t putULong64(const char *k, uint64_t v) { _v[k] = (double)v; return 8; }
  size_t putLong64(const char *k, int64_t v) { _v[k] = (double)v; return 8; }
  bool isKey(const char *k) { return _v.count(k) != 0; }
  bool remove(const char *k) { return _v.erase(k) != 0; }
};

// ---------------------------------------------------------------- serial
#define F(x) x
class SimSerial {
public:
  void begin(uint32_t = 115200) {}
  void end() {}
  operator bool() const { return true; }
  int  available() { return 0; }
  int  read() { return -1; }
  void flush() {}
  void println() { fputs("\n", stderr); }
  void println(const char *s) { fprintf(stderr, "%s\n", s); }
  void println(const std::string &s) { fprintf(stderr, "%s\n", s.c_str()); }
  void println(int v) { fprintf(stderr, "%d\n", v); }
  void println(unsigned v) { fprintf(stderr, "%u\n", v); }
  void println(long v) { fprintf(stderr, "%ld\n", v); }
  void println(unsigned long v) { fprintf(stderr, "%lu\n", v); }
  void println(double v) { fprintf(stderr, "%f\n", v); }
  void println(float v) { fprintf(stderr, "%f\n", v); }
  void print(const char *s) { fputs(s, stderr); }
  void print(const std::string &s) { fputs(s.c_str(), stderr); }
  void print(int v) { fprintf(stderr, "%d", v); }
  void print(unsigned v) { fprintf(stderr, "%u", v); }
  void print(long v) { fprintf(stderr, "%ld", v); }
  void print(unsigned long v) { fprintf(stderr, "%lu", v); }
  void print(double v) { fprintf(stderr, "%f", v); }
  void print(float v) { fprintf(stderr, "%f", v); }
  void print(char c) { fputc(c, stderr); }
  // Arduino's two-argument forms: print(value, decimals) for floats and
  // print(value, BASE) for integers. Both are common in generated diagnostics,
  // and a sketch that would build for the device must build here.
  void print(double v, int digits) { fprintf(stderr, "%.*f", digits, v); }
  void println(double v, int digits) { fprintf(stderr, "%.*f\n", digits, v); }
  void print(float v, int digits) { fprintf(stderr, "%.*f", digits, (double)v); }
  void println(float v, int digits) { fprintf(stderr, "%.*f\n", digits, (double)v); }
  void print(int v, int) { fprintf(stderr, "%d", v); }
  void println(int v, int) { fprintf(stderr, "%d\n", v); }
  void print(unsigned v, int) { fprintf(stderr, "%u", v); }
  void println(unsigned v, int) { fprintf(stderr, "%u\n", v); }
  void print(long v, int) { fprintf(stderr, "%ld", v); }
  void println(long v, int) { fprintf(stderr, "%ld\n", v); }
  void print(unsigned long v, int) { fprintf(stderr, "%lu", v); }
  void println(unsigned long v, int) { fprintf(stderr, "%lu\n", v); }
  void write(uint8_t c) { fputc(c, stderr); }
  template <typename... A> int printf(const char *fmt, A... a) {
    return fprintf(stderr, fmt, a...);
  }
};
static SimSerial Serial;

// ---------------------------------------------------------------- String
// Arduino's String, not std::string: sketches build display text with
// `String(cost, 2)` and `"$" + String(n)`, neither of which std::string can do.
class String {
  std::string _s;
public:
  String() {}
  String(const char *s) : _s(s ? s : "") {}
  String(const std::string &s) : _s(s) {}
  String(char c) : _s(1, c) {}
  String(int v) { char b[24]; snprintf(b, sizeof b, "%d", v); _s = b; }
  String(unsigned v) { char b[24]; snprintf(b, sizeof b, "%u", v); _s = b; }
  String(long v) { char b[32]; snprintf(b, sizeof b, "%ld", v); _s = b; }
  String(unsigned long v) { char b[32]; snprintf(b, sizeof b, "%lu", v); _s = b; }
  String(double v, int digits = 2) {
    char b[64]; snprintf(b, sizeof b, "%.*f", digits, v); _s = b;
  }
  String(float v, int digits = 2) {
    char b[64]; snprintf(b, sizeof b, "%.*f", digits, (double)v); _s = b;
  }

  const char *c_str() const { return _s.c_str(); }
  size_t length() const { return _s.size(); }
  bool   isEmpty() const { return _s.empty(); }
  char   charAt(size_t i) const { return i < _s.size() ? _s[i] : '\0'; }
  char   operator[](size_t i) const { return charAt(i); }
  int    toInt() const { return atoi(_s.c_str()); }
  double toDouble() const { return atof(_s.c_str()); }
  float  toFloat() const { return (float)atof(_s.c_str()); }
  int    indexOf(char c) const { auto p = _s.find(c); return p == std::string::npos ? -1 : (int)p; }
  String substring(size_t a) const { return String(_s.substr(a > _s.size() ? _s.size() : a)); }
  String substring(size_t a, size_t b) const {
    if (a > _s.size()) a = _s.size();
    if (b > _s.size()) b = _s.size();
    return String(b > a ? _s.substr(a, b - a) : std::string());
  }
  void   trim() {
    size_t a = _s.find_first_not_of(" \t\r\n");
    size_t b = _s.find_last_not_of(" \t\r\n");
    _s = (a == std::string::npos) ? "" : _s.substr(a, b - a + 1);
  }
  // Converts to const char*, not to std::string. LovyanGFX's drawString and
  // textWidth take a const char*, and sketches pass String straight into them
  // (`sp.drawString(String(count), x, y)`); on the device the Arduino core's
  // own String does the same. Offering both conversions would make every
  // Serial.print(String) ambiguous, so this is the one.
  operator const char *() const { return _s.c_str(); }

  String &operator+=(const String &o) { _s += o._s; return *this; }
  String &operator+=(const char *o) { _s += (o ? o : ""); return *this; }
  friend String operator+(String a, const String &b) { a += b; return a; }
  friend String operator+(String a, const char *b) { a += b; return a; }
  friend String operator+(const char *a, const String &b) { return String(a) + b; }
  friend bool operator==(const String &a, const String &b) { return a._s == b._s; }
  friend bool operator!=(const String &a, const String &b) { return a._s != b._s; }
};
