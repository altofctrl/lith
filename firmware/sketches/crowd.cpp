// =====================================================================
// Lith CROWD — hide in the crowd, find the other player. ESP32-S3-Zero.
//   Flash this to two or more Liths on the same bench; they find each
//   other over ESP-NOW broadcast with no pairing step.
//
// Archived sketch, built in place of src/ via:  pio run -e crowd -t upload
//
// THE GAME
//   The screen is a crowd of identical men walking back and forth. One of
//   them is you (blue). One of them is another Lith, driven by a person
//   doing exactly what you are doing. The rest are NPCs.
//
//   Hold SW1 / SW2 to walk left / right. Let go and you stop. Men bounce
//   off the side walls, and every start and stop kicks a little random
//   vertical drift - which is your camouflage, because the NPCs start and
//   stop on their own random timers and drift the same way.
//
//   Turn the encoder to sweep a red highlight through the other men. Press
//   BOTH switches together to accuse whoever is lit. Right, and you win.
//   Wrong, and it costs one of your three lives. Get accused correctly by
//   the other player and you are out. Score is how long you survived.
//
// WHY THE NPCs ARE NOT SYNCED
//   Each Lith runs its own crowd; only real players cross the network.
//   Two screens therefore show different NPCs, which costs nothing: your
//   job is to pick the player out of the crowd in front of YOU. Syncing
//   the NPCs would need a shared clock and a shared RNG for no gain.
//
// CONTROLS
//   SW1 hold          -> walk left        SW2 hold -> walk right
//   Encoder           -> sweep the red highlight through the other men
//   SW1 + SW2         -> accuse the highlighted man (and start a round)
//
// SERIAL COMMANDS (115200)
//   s start round   p list peers   i info   ? help
//
// =====================================================================

#define LGFX_USE_V1
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <LovyanGFX.hpp>
#include "walkman.h"

// ------------------------- pin map -----------------------------------
static constexpr int PIN_TFT_SCLK = 12;
static constexpr int PIN_TFT_MOSI = 11;
static constexpr int PIN_TFT_DC   = 13;
static constexpr int PIN_TFT_CS   = 10;
static constexpr int PIN_TFT_RST  =  9;
static constexpr int PIN_TFT_BLK  =  8;

static constexpr int PIN_SW1      =  1;
static constexpr int PIN_SW2      =  2;
static constexpr int PIN_ENC_A    =  4;
static constexpr int PIN_ENC_B    =  5;
static constexpr int PIN_MOTOR    =  6;

// ------------------------- tuning ------------------------------------
static constexpr int SCR_W = 320;
static constexpr int SCR_H = 170;

// 2 = draw the 64x64 art at half size, which is what makes a CROWD fit on
// a 320x170 panel. The downsample ORs each 2x2 block rather than sampling,
// so the thin line-art strokes survive instead of dropping out. Set to 1
// for full-size men and a much emptier screen.
static constexpr int DRAW_SCALE = 2;
static constexpr int DW = WALK_FRAME_WIDTH  / DRAW_SCALE;
static constexpr int DH = WALK_FRAME_HEIGHT / DRAW_SCALE;

static constexpr int HUD_H    = 16;
static constexpr int PLAY_TOP = HUD_H;
static constexpr int PLAY_BOT = SCR_H - DH;

static constexpr int NPC_COUNT  = 10;
static constexpr int MAX_REMOTE = 4;
static constexpr int MAX_MEN    = 1 + NPC_COUNT + MAX_REMOTE;

static constexpr float WALK_SPEED = 1.15f;   // px per tick
static constexpr float DRIFT_KICK = 1.30f;   // vertical kick on start/stop
static constexpr float DRIFT_DECAY = 0.90f;

static constexpr uint32_t NPC_MIN_MS   = 450;   // how long an NPC holds a
static constexpr uint32_t NPC_MAX_MS   = 2600;  // walk or a pause
static constexpr uint32_t SELECT_MS    = 2500;  // red highlight lifetime
static constexpr uint32_t STATE_TX_MS  = 50;    // broadcast rate
static constexpr uint32_t PEER_GONE_MS = 2500;
static constexpr uint32_t OVER_MS      = 4000;  // win/lose banner
static constexpr uint8_t  START_LIVES  = 3;

static constexpr uint32_t ENC_STEPS_PER_DETENT = 4;
static constexpr uint32_t MOTOR_PWM_FREQ = 20000;
static constexpr uint8_t  MOTOR_PWM_BITS = 8;
static constexpr int      MOTOR_LEDC_CH  = 0;
static constexpr uint32_t DEBOUNCE_MS    = 25;

// ------------------------- display driver ----------------------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 50000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = PIN_TFT_SCLK;
      cfg.pin_mosi    = PIN_TFT_MOSI;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = PIN_TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs          = PIN_TFT_CS;
      cfg.pin_rst         = PIN_TFT_RST;
      cfg.pin_busy        = -1;
      cfg.panel_width     = 170;
      cfg.panel_height    = 320;
      cfg.offset_x        = 35;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 2;
      cfg.readable        = false;
      cfg.invert          = true;
      cfg.rgb_order       = false;
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = PIN_TFT_BLK;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX        tft;
LGFX_Sprite frame(&tft);
bool displayOK      = false;
bool useFrameBuffer = false;

// ------------------------- palette -----------------------------------
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static const uint16_t COL_BG    = RGB565(  0,  0,  0);
static const uint16_t COL_NPC   = RGB565(235,238,245);   // the crowd
static const uint16_t COL_SELF  = RGB565( 70,150,255);   // you
static const uint16_t COL_PICK  = RGB565(255, 60, 60);   // highlighted
static const uint16_t COL_HUD   = RGB565( 18, 20, 26);
static const uint16_t COL_TEXT  = RGB565(225,230,240);
static const uint16_t COL_DIM   = RGB565(120,130,148);
static const uint16_t COL_WARN  = RGB565(255,160, 50);
static const uint16_t COL_OK    = RGB565( 80,210,130);

// ------------------------- encoder -----------------------------------
static const int8_t QUAD[16] = { 0, -1,  1,  0,
                                 1,  0,  0, -1,
                                -1,  0,  0,  1,
                                 0,  1, -1,  0 };
volatile uint8_t  encState = 0;
volatile int32_t  encRaw   = 0;
int32_t encDetentLast = 0;

void IRAM_ATTR encISR() {
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  encState = ((encState << 2) | (a << 1) | b) & 0x0F;
  encRaw  += QUAD[encState];
}

// ------------------------- buttons -----------------------------------
static const int BTN_COUNT = 2;
static const int BTN_L = 0;
static const int BTN_R = 1;

struct Button {
  int      pin;
  bool     level;
  bool     rawLast;
  uint32_t tChange;
};

Button btns[BTN_COUNT] = {
  {PIN_SW1, false, false, 0},
  {PIN_SW2, false, false, 0}
};

void pollButton(int idx) {
  Button &b = btns[idx];
  uint32_t now = millis();
  bool raw = (digitalRead(b.pin) == LOW);
  if (raw != b.rawLast) { b.rawLast = raw; b.tChange = now; }
  if ((now - b.tChange) >= DEBOUNCE_MS) b.level = raw;
}

// ------------------------- haptics -----------------------------------
// The same non-blocking step sequencer the other Lith sketches use: each
// step is a duty held for a number of ms, walked against millis(), so a
// pattern never blocks the frame.
struct HapStep { uint8_t duty; uint16_t ms; };

static const int HAP_MAX = 8;
HapStep  hapSeq[HAP_MAX];
int      hapLen  = 0;
int      hapIdx  = 0;
uint32_t hapNext = 0;
uint8_t  hapNow  = 0;

static const HapStep HAP_SWEEP[]  = {{ 60,  10}};
static const HapStep HAP_ACCUSE[] = {{200,  40}};
static const HapStep HAP_RIGHT[]  = {{255,  60}, {0, 60}, {255, 170}};
static const HapStep HAP_WRONG[]  = {{255,  50}, {0, 45}, {255, 50}, {0, 45}, {255, 50}};
static const HapStep HAP_TAUNT[]  = {{ 70,  25}};    // someone else guessed wrong
static const HapStep HAP_CAUGHT[] = {{255, 320}};

void motorRaw(uint8_t duty) {
  hapNow = duty;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR, duty);
#else
  ledcWrite(MOTOR_LEDC_CH, duty);
#endif
}

void motorInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_MOTOR, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
#else
  ledcSetup(MOTOR_LEDC_CH, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
  ledcAttachPin(PIN_MOTOR, MOTOR_LEDC_CH);
#endif
  motorRaw(0);
}

void hapticPlay(const HapStep *seq, int n) {
  if (n > HAP_MAX) n = HAP_MAX;
  memcpy(hapSeq, seq, n * sizeof(HapStep));
  hapLen = n; hapIdx = 0; hapNext = millis();
}
#define HAPTIC(p) hapticPlay(p, sizeof(p) / sizeof(p[0]))

void hapticService() {
  uint32_t now = millis();
  while (hapIdx < hapLen && (int32_t)(now - hapNext) >= 0) {
    motorRaw(hapSeq[hapIdx].duty);
    hapNext = now + hapSeq[hapIdx].ms;
    hapIdx++;
  }
  if (hapLen && hapIdx >= hapLen && (int32_t)(now - hapNext) >= 0) {
    motorRaw(0);
    hapLen = 0;
  }
}

// ------------------------- the crowd ---------------------------------
struct Man {
  uint16_t id;
  float    x, y;
  float    tx, ty;      // remote target, lerped toward
  float    vy;
  int8_t   dir;         // -1 left, +1 right
  bool     moving;
  bool     remote;      // a real player on another Lith
  bool     used;
  uint8_t  frame;
  uint32_t frameNext;
  uint32_t toggleAt;    // NPC's next start/stop decision
  uint32_t lastSeen;
};

Man      men[MAX_MEN];
int      menCount = 0;
uint16_t myId     = 0;

float frand(float lo, float hi) {
  return lo + (float)(esp_random() % 10000) / 10000.0f * (hi - lo);
}
uint32_t urand(uint32_t lo, uint32_t hi) { return lo + esp_random() % (hi - lo + 1); }

// every start and stop kicks the man up or down a little - the tell that
// makes a human indistinguishable from an NPC at a glance
void kickDrift(Man &m) { m.vy = frand(-DRIFT_KICK, DRIFT_KICK); }

void placeMan(Man &m) {
  m.x = frand(0, SCR_W - DW);
  m.y = frand(PLAY_TOP, PLAY_BOT);
  m.tx = m.x; m.ty = m.y;
  m.vy = 0;
  m.dir = (esp_random() & 1) ? 1 : -1;
  m.moving = (esp_random() & 1);
  m.frame = esp_random() % WALK_FRAME_COUNT;
  m.frameNext = millis() + WALK_FRAME_DELAY;
  m.toggleAt = millis() + urand(NPC_MIN_MS, NPC_MAX_MS);
}

void buildCrowd() {
  menCount = 0;

  Man &me = men[menCount++];
  memset(&me, 0, sizeof(Man));
  me.id = myId; me.used = true; me.remote = false;
  placeMan(me);
  me.moving = false;

  for (int i = 0; i < NPC_COUNT; i++) {
    Man &m = men[menCount++];
    memset(&m, 0, sizeof(Man));
    m.id = 0x8000 | i; m.used = true; m.remote = false;
    placeMan(m);
  }
}

int findMan(uint16_t id) {
  for (int i = 0; i < menCount; i++) if (men[i].used && men[i].id == id) return i;
  return -1;
}

// ------------------------- game state --------------------------------
static const uint8_t GS_LOBBY = 0;
static const uint8_t GS_PLAY  = 1;
static const uint8_t GS_WIN   = 2;
static const uint8_t GS_LOSE  = 3;

uint8_t  gs        = GS_LOBBY;
uint8_t  lives     = START_LIVES;
uint32_t roundStart = 0;
uint32_t roundMs    = 0;      // survival time, frozen at the end
uint32_t overAt     = 0;
char     overLine[28] = "";

int      selIdx   = -1;       // index into men[], -1 = nothing lit
uint32_t selUntil = 0;
bool     bothLatch = false;   // both keys are down and already acted on

float fps = 0;
uint32_t lastFrame = 0;

// ------------------------- networking --------------------------------
// Broadcast, so there is no pairing step and no host: every Lith shouts
// its man's position and listens for everyone else's.
static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static const uint8_t MSG_STATE  = 1;
static const uint8_t MSG_RESULT = 2;   // "I caught <target>"
static const uint8_t MSG_MISS   = 3;   // "I guessed wrong" - pure tension
static const uint8_t MSG_START  = 4;

struct __attribute__((packed)) Msg {
  uint8_t  magic;      // 'L'
  uint8_t  ver;
  uint8_t  type;
  uint16_t id;
  int16_t  x, y;
  int8_t   dir;
  uint8_t  moving;
  uint8_t  frame;
  uint8_t  lives;
  uint16_t target;
};

// The receive callback runs in the WiFi task, so it may not touch the
// crowd or the display - it only drops the packet in a ring for loop().
static const int INBOX_N = 12;
volatile Msg  inbox[INBOX_N];
volatile int  inHead = 0, inTail = 0;
portMUX_TYPE  netMux = portMUX_INITIALIZER_UNLOCKED;
bool netOK = false;

void pushInbox(const uint8_t *data, int len) {
  if (len != (int)sizeof(Msg)) return;
  portENTER_CRITICAL_ISR(&netMux);
  int nh = (inHead + 1) % INBOX_N;
  if (nh != inTail) {
    memcpy((void *)&inbox[inHead], data, sizeof(Msg));
    inHead = nh;
  }
  portEXIT_CRITICAL_ISR(&netMux);
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info; pushInbox(data, len);
}
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac; pushInbox(data, len);
}
#endif

bool netInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  // pin the channel so two Liths that never joined an AP still agree
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  myId = ((uint16_t)mac[4] << 8) | mac[5];
  if (myId == 0) myId = 1;

  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 1;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) return false;
  return true;
}

void netSend(uint8_t type, uint16_t target) {
  if (!netOK) return;
  Msg m = {};
  m.magic  = 'L';
  m.ver    = 1;
  m.type   = type;
  m.id     = myId;
  m.x      = (int16_t)men[0].x;
  m.y      = (int16_t)men[0].y;
  m.dir    = men[0].dir;
  m.moving = men[0].moving ? 1 : 0;
  m.frame  = men[0].frame;
  m.lives  = lives;
  m.target = target;
  esp_now_send(BCAST, (const uint8_t *)&m, sizeof(m));
}

int peerCount() {
  int n = 0;
  for (int i = 0; i < menCount; i++) if (men[i].used && men[i].remote) n++;
  return n;
}

// ------------------------- round flow --------------------------------
void startRound() {
  buildCrowd();
  lives      = START_LIVES;
  roundStart = millis();
  roundMs    = 0;
  selIdx     = -1;
  selUntil   = 0;
  gs         = GS_PLAY;
  Serial.printf("round start, id %04X\n", myId);
}

void endRound(bool won, const char *why) {
  roundMs = millis() - roundStart;
  gs      = won ? GS_WIN : GS_LOSE;
  overAt  = millis() + OVER_MS;
  strncpy(overLine, why, sizeof(overLine) - 1);
  overLine[sizeof(overLine) - 1] = '\0';
  if (won) HAPTIC(HAP_RIGHT);
  else     HAPTIC(HAP_CAUGHT);
}

void accuse() {
  if (selIdx < 0 || selIdx >= menCount || millis() > selUntil) return;
  Man &t = men[selIdx];

  if (t.remote) {
    netSend(MSG_RESULT, t.id);              // tell them they are caught
    endRound(true, "you found them");
  } else {
    lives--;
    netSend(MSG_MISS, 0);
    if (lives == 0) endRound(false, "out of lives");
    else            HAPTIC(HAP_WRONG);
  }
  selIdx = -1;
}

// ------------------------- inbox drain -------------------------------
void netService() {
  Msg m;
  while (true) {
    portENTER_CRITICAL(&netMux);
    bool have = (inTail != inHead);
    if (have) { memcpy(&m, (const void *)&inbox[inTail], sizeof(Msg)); inTail = (inTail + 1) % INBOX_N; }
    portEXIT_CRITICAL(&netMux);
    if (!have) break;

    if (m.magic != 'L' || m.ver != 1 || m.id == myId) continue;

    if (m.type == MSG_START && gs != GS_PLAY) { startRound(); continue; }

    if (m.type == MSG_RESULT) {
      if (m.target == myId && gs == GS_PLAY) endRound(false, "they found you");
      continue;
    }

    if (m.type == MSG_MISS) { if (gs == GS_PLAY) HAPTIC(HAP_TAUNT); continue; }

    if (m.type != MSG_STATE) continue;

    // a remote player's man: adopt it into the crowd, or update it
    int idx = findMan(m.id);
    if (idx < 0) {
      if (menCount >= MAX_MEN) continue;
      idx = menCount++;
      memset(&men[idx], 0, sizeof(Man));
      men[idx].id = m.id;
      men[idx].used = true;
      men[idx].remote = true;
      men[idx].x = m.x; men[idx].y = m.y;
    }
    Man &r = men[idx];
    r.tx = m.x; r.ty = m.y;
    r.dir = m.dir ? m.dir : 1;
    r.moving = m.moving;
    r.frame = m.frame % WALK_FRAME_COUNT;
    r.lastSeen = millis();
  }

  // drop players who have gone quiet
  for (int i = 1; i < menCount; i++) {
    if (!men[i].used || !men[i].remote) continue;
    if (millis() - men[i].lastSeen > PEER_GONE_MS) {
      for (int j = i; j < menCount - 1; j++) men[j] = men[j + 1];
      menCount--;
      if (selIdx >= i) selIdx = -1;
      i--;
    }
  }
}

// ------------------------- simulation --------------------------------
void stepMan(Man &m, bool isSelf) {
  uint32_t now = millis();

  if (m.remote) {
    // 20 Hz packets into a 40 Hz render: ease toward the reported spot so
    // the other player does not visibly jump between updates
    m.x += (m.tx - m.x) * 0.35f;
    m.y += (m.ty - m.y) * 0.35f;
  } else {
    if (!isSelf && now >= m.toggleAt) {
      m.moving = !m.moving;
      if (esp_random() % 5 == 0) m.dir = -m.dir;
      m.toggleAt = now + urand(NPC_MIN_MS, NPC_MAX_MS);
      kickDrift(m);
    }

    if (m.moving) m.x += m.dir * WALK_SPEED;

    m.y  += m.vy;
    m.vy *= DRIFT_DECAY;

    if (m.x < 0)           { m.x = 0;           m.dir =  1; kickDrift(m); }
    if (m.x > SCR_W - DW)  { m.x = SCR_W - DW;  m.dir = -1; kickDrift(m); }
    if (m.y < PLAY_TOP)    { m.y = PLAY_TOP;    m.vy = -m.vy * 0.5f; }
    if (m.y > PLAY_BOT)    { m.y = PLAY_BOT;    m.vy = -m.vy * 0.5f; }
  }

  if (m.moving && now >= m.frameNext) {
    m.frame = (m.frame + 1) % WALK_FRAME_COUNT;
    m.frameNext = now + WALK_FRAME_DELAY;
  }
}

void stepSelf() {
  Man &me = men[0];
  bool l = btns[BTN_L].level, r = btns[BTN_R].level;
  bool wasMoving = me.moving;

  // both keys is the accuse gesture, not a walk in two directions at once
  if (l && r) me.moving = false;
  else if (l) { me.moving = true; me.dir = -1; }
  else if (r) { me.moving = true; me.dir =  1; }
  else        me.moving = false;

  if (me.moving != wasMoving) kickDrift(me);
  stepMan(me, true);
}

// ------------------------- rendering ---------------------------------
// Blits one 64x64 1-bit frame at DRAW_SCALE, optionally mirrored so a man
// faces the way he is walking. Empty blocks are skipped, which is most of
// the bitmap, so a dozen men still fit inside a frame.
void drawMan(LGFX_Sprite *g, int x, int y, uint8_t fr, bool mirror, uint16_t col) {
  if (x >= SCR_W || x + DW <= 0) return;
  const uint8_t *bm = walkFrames[fr];

  for (int sy = 0; sy < WALK_FRAME_HEIGHT; sy += DRAW_SCALE) {
    int dy = y + sy / DRAW_SCALE;
    if (dy < 0 || dy >= SCR_H) continue;

    for (int sx = 0; sx < WALK_FRAME_WIDTH; sx += DRAW_SCALE) {
      bool on = false;
      for (int by = 0; by < DRAW_SCALE && !on; by++) {
        const uint8_t *row = bm + (sy + by) * (WALK_FRAME_WIDTH / 8);
        for (int bx = 0; bx < DRAW_SCALE && !on; bx++) {
          int px = sx + bx;
          if (row[px >> 3] & (0x80 >> (px & 7))) on = true;
        }
      }
      if (!on) continue;

      int col_i = sx / DRAW_SCALE;
      int dx = x + (mirror ? (DW - 1 - col_i) : col_i);
      if (dx < 0 || dx >= SCR_W) continue;
      g->drawPixel(dx, dy, col);
    }
  }
}

void drawHud(LGFX_Sprite *g) {
  char buf[40];
  g->fillRect(0, 0, SCR_W, HUD_H, COL_HUD);
  g->setFont(&fonts::Font0);
  g->setTextDatum(textdatum_t::top_left);

  // lives
  for (int i = 0; i < START_LIVES; i++) {
    uint16_t c = (i < lives) ? COL_PICK : RGB565(50, 54, 64);
    g->fillCircle(9 + i * 12, 8, 4, c);
  }

  g->setTextColor(COL_DIM, COL_HUD);
  snprintf(buf, sizeof(buf), "id %04X", myId);
  g->drawString(buf, 50, 5);

  int peers = peerCount();
  g->setTextColor(peers ? COL_OK : COL_WARN, COL_HUD);
  snprintf(buf, sizeof(buf), "%d peer%s", peers, peers == 1 ? "" : "s");
  g->drawString(buf, 108, 5);

  uint32_t s = (gs == GS_PLAY ? millis() - roundStart : roundMs) / 1000;
  g->setTextColor(COL_TEXT, COL_HUD);
  g->setTextDatum(textdatum_t::top_right);
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  g->drawString(buf, SCR_W - 6, 5);

  if (selIdx >= 0 && millis() <= selUntil) {
    g->setTextColor(COL_PICK, COL_HUD);
    g->setTextDatum(textdatum_t::top_center);
    g->drawString("LOCKED - BOTH KEYS TO ACCUSE", SCR_W / 2 - 20, 5);
  }
}

void drawBanner(LGFX_Sprite *g, const char *big, const char *small_, uint16_t c) {
  char buf[40];
  g->fillRect(30, 46, SCR_W - 60, 78, COL_HUD);
  g->drawRect(30, 46, SCR_W - 60, 78, c);

  g->setFont(&fonts::Font4);
  g->setTextColor(c, COL_HUD);
  g->setTextDatum(textdatum_t::middle_center);
  g->drawString(big, SCR_W / 2, 68);

  g->setFont(&fonts::Font2);
  g->setTextColor(COL_TEXT, COL_HUD);
  g->drawString(small_, SCR_W / 2, 92);

  g->setFont(&fonts::Font0);
  g->setTextColor(COL_DIM, COL_HUD);
  snprintf(buf, sizeof(buf), "survived %lu.%lus",
           (unsigned long)(roundMs / 1000), (unsigned long)((roundMs % 1000) / 100));
  g->drawString(buf, SCR_W / 2, 111);
}

void render(LGFX_Sprite *g) {
  g->fillSprite(COL_BG);

  bool lit = (selIdx >= 0 && millis() <= selUntil);
  for (int i = 0; i < menCount; i++) {
    if (!men[i].used) continue;
    uint16_t c = COL_NPC;
    if (i == 0)             c = COL_SELF;
    else if (lit && i == selIdx) c = COL_PICK;
    drawMan(g, (int)men[i].x, (int)men[i].y, men[i].frame, men[i].dir < 0, c);
  }

  drawHud(g);

  if (gs == GS_LOBBY) {
    int peers = peerCount();
    drawBanner(g, "LITH CROWD",
               peers ? "both keys to start" : "waiting for a peer",
               peers ? COL_OK : COL_WARN);
  } else if (gs == GS_WIN) {
    drawBanner(g, "FOUND THEM", overLine, COL_OK);
  } else if (gs == GS_LOSE) {
    drawBanner(g, "CAUGHT", overLine, COL_PICK);
  }
}

// ------------------------- input -------------------------------------
void onDetent(int delta) {
  if (gs != GS_PLAY || menCount < 2) return;

  // sweep through everyone but yourself
  int cur = (selIdx >= 1) ? selIdx : (delta > 0 ? 0 : 1);
  int n = menCount - 1;                       // selectable men
  int rel = ((cur - 1) + delta) % n;
  if (rel < 0) rel += n;
  selIdx   = 1 + rel;
  selUntil = millis() + SELECT_MS;
  HAPTIC(HAP_SWEEP);
}

void handleButtons() {
  bool both = btns[BTN_L].level && btns[BTN_R].level;

  if (both && !bothLatch) {
    bothLatch = true;
    if (gs == GS_PLAY) {
      HAPTIC(HAP_ACCUSE);
      accuse();
    } else if (gs == GS_LOBBY) {
      netSend(MSG_START, 0);
      startRound();
    }
  }
  if (!both) bothLatch = false;
}

void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    switch (c) {
      case 's': netSend(MSG_START, 0); startRound(); break;
      case 'p':
        Serial.printf("me %04X | men %d | peers %d\n", myId, menCount, peerCount());
        for (int i = 0; i < menCount; i++)
          Serial.printf("  [%2d] %04X %-6s x%4d y%4d %s\n", i, men[i].id,
                        i == 0 ? "SELF" : (men[i].remote ? "PLAYER" : "npc"),
                        (int)men[i].x, (int)men[i].y, men[i].moving ? "walk" : "idle");
        break;
      case 'i':
        Serial.printf("display %s | esp-now %s | frames %d | %2.0f fps | heap %lu\n",
                      displayOK ? "OK" : "FAILED", netOK ? "up" : "DOWN",
                      (int)WALK_FRAME_COUNT, fps, (unsigned long)ESP.getFreeHeap());
        break;
      case '?': Serial.println(F("s start | p peers | i info")); break;
      default: break;
    }
  }
}

// ------------------------- setup -------------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println(F("\n=== Lith CROWD ==="));

  pinMode(PIN_SW1,   INPUT_PULLUP);
  pinMode(PIN_SW2,   INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);

  encState = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  motorInit();

  netOK = netInit();
  Serial.printf("id %04X  esp-now %s\n", myId, netOK ? "up" : "FAILED");

  displayOK = tft.init();
  if (!displayOK) {
    Serial.println(F("ERROR: tft.init() failed - continuing in serial-only mode"));
  } else {
    tft.setRotation(1);
    tft.setBrightness(255);
    tft.fillScreen(COL_BG);

    frame.setColorDepth(16);
    frame.setPsram(false);
    if (frame.createSprite(SCR_W, SCR_H) == nullptr) {
      frame.setPsram(true);
      if (frame.createSprite(SCR_W, SCR_H) == nullptr)
        Serial.println(F("frame buffer alloc failed - the crowd will tear"));
      else { useFrameBuffer = true; Serial.println(F("frame buffer in PSRAM")); }
    } else { useFrameBuffer = true; Serial.println(F("frame buffer in SRAM")); }
  }

  buildCrowd();
  Serial.printf("%d walk frames, %dx%d each, drawn at 1/%d\n",
                (int)WALK_FRAME_COUNT, WALK_FRAME_WIDTH, WALK_FRAME_HEIGHT, DRAW_SCALE);
  Serial.println(F("hold SW1/SW2 to walk, encoder to sweep, both keys to accuse"));
  lastFrame = millis();
}

// ------------------------- loop --------------------------------------
void loop() {
  handleSerial();
  pollButton(BTN_L);
  pollButton(BTN_R);
  handleButtons();
  netService();

  noInterrupts();
  int32_t raw = encRaw;
  interrupts();
  int32_t det = raw / (int32_t)ENC_STEPS_PER_DETENT;
  if (det != encDetentLast) {
    int delta = (int)(det - encDetentLast);
    encDetentLast = det;
    onDetent(delta);
  }

  if (gs == GS_PLAY) {
    stepSelf();
    for (int i = 1; i < menCount; i++) if (men[i].used) stepMan(men[i], false);
  } else {
    // the crowd keeps milling about behind the lobby and result banners
    for (int i = 0; i < menCount; i++) if (men[i].used) stepMan(men[i], false);
    if ((gs == GS_WIN || gs == GS_LOSE) && millis() >= overAt) gs = GS_LOBBY;
  }

  hapticService();

  static uint32_t txNext = 0;
  if (millis() >= txNext) { txNext = millis() + STATE_TX_MS; netSend(MSG_STATE, 0); }

  static uint32_t tNext = 0;
  uint32_t now = millis();
  if (now >= tNext) {
    tNext = now + 25;
    uint32_t dt = now - lastFrame;
    lastFrame = now;
    if (dt) fps = fps * 0.85f + (1000.0f / dt) * 0.15f;

    if (displayOK) {
      LGFX_Sprite *g = &frame;
      if (useFrameBuffer) { render(g); frame.pushSprite(0, 0); }
    }
  }
}
