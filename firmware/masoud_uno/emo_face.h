// Masoud's face: EMO-style animated robot eyes for a 160x128 screen.
//
// The "glow" look: each eye is lit from the top (a vertical gradient), sits in
// a soft glow ring, and carries two glints that drift as he looks around. He
// blushes when happy or in love.
//
// Plain C++ with no Arduino calls, so the same code runs on the Uno and in
// the PC preview (tools/emo_preview.cpp). Each frame is drawn row by row as
// runs of color inside one window that covers last frame's face and this
// frame's: every pixel is written once with its final color, so there is no
// flicker and no frame buffer (a 160x128 frame would be 40 KB; the Uno has 2).
// Overlapping layers (glow, blush, eye, glint) are resolved per row by
// priority, so each pixel still gets exactly one color.
#pragma once
#include <stdint.h>
#include <math.h>
#include <avr/pgmspace.h>   // on the PC: tools/shim

namespace emo {

const int16_t W = 160, H = 128;
const float GLOW = 4;   // outer edge of the glow, px
const float HALO = 2;   // the brighter inner part of the glow, px

// DIZZY only comes from a shake (shake() / key d), never from the random moods.
enum Mood : uint8_t { NEUTRAL, HAPPY, SAD, ANGRY, SURPRISED, SLEEPY, LOVE, SUSPICIOUS, DIZZY, MOOD_COUNT };

struct Eye {
  float cx, cy, w, h, r;   // center, size, corner radius (px)
  float lidIn, lidOut;     // slanted top lid at the inner / outer edge (fraction of h)
  float lidFlat;           // flat top lid (fraction of h)
  float happy;             // 0..1: bottom cut into an arch, like ^
  float heart;             // 0..1: becomes a heart of this scale
  float dizzy;             // 0..1: round eyes rolling around each other
  float blush;             // 0..1: pink cheek under the eye
  float red, green, blue;  // 0..255
};

inline uint8_t clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

inline uint16_t rgb565(float r, float g, float b) {
  return ((clamp8(r) & 0xF8) << 8) | ((clamp8(g) & 0xFC) << 3) | (clamp8(b) >> 3);
}

// Round to the nearest pixel. Cheaper than floorf on the AVR's software floats.
inline int16_t px(float v) { return (int16_t)(v + 16384.5f) - 16384; }

// Where a mood wants an eye to be (before looking around and blinking).
inline Eye moodEye(Mood m, uint8_t side) {
  Eye e = {side == 0 ? 48.0f : 112.0f, 62, 44, 52, 12, 0, 0, 0, 0, 0, 0, 0, 0, 215, 255};
  switch (m) {
    case HAPPY:      e.h = 42; e.cy = 58; e.r = 19; e.happy = 1; e.blush = 1; break;
    case SAD:        e.w = 42; e.h = 46; e.cy = 70; e.lidOut = 0.42f;
                     e.red = 40; e.green = 130; e.blue = 255; break;
    case ANGRY:      e.w = 46; e.h = 46; e.lidIn = 0.48f;
                     e.red = 255; e.green = 45; e.blue = 25; break;
    case SURPRISED:  e.w = 52; e.h = 62; e.r = 18; break;
    case SLEEPY:     e.cy = 66; e.lidFlat = 0.62f; e.red = 0; e.green = 140; e.blue = 175; break;
    case LOVE:       e.heart = 1; e.red = 255; e.green = 60; e.blue = 150; break;
    case SUSPICIOUS: e.lidFlat = side == 0 ? 0.52f : 0.18f; break;
    case DIZZY:      e.w = 36; e.h = 36; e.r = 18; e.dizzy = 1; break;
    default: break;
  }
  return e;
}

// One colored run on a row; when runs overlap, the higher priority wins.
struct Seg { int16_t x0, x1; uint16_t color; uint8_t pri; };
enum Layer : uint8_t { P_GLOW = 1, P_HALO = 2, P_BLUSH = 3, P_BODY = 4, P_GLINT = 5 };

class Face {
 public:
  Face() { reset(); }

  void reset() {
    for (uint8_t s = 0; s < 2; s++) { eye_[s] = moodEye(NEUTRAL, s); eye_[s].h = 3; }
    mood_ = NEUTRAL;
    time_ = 0; lookX_ = lookY_ = lookTX_ = lookTY_ = 0;
    blink_ = -1; blinkEye_ = -1;
    autoMode_ = true; manualUntil_ = 0;
    nextBlink_ = 6; nextLook_ = 6; nextMood_ = 12; moodUntil_ = 0;
    zPhase_ = 0;
    invalidate();
  }

  // Next frame repaints the whole screen (after a panel reset).
  void invalidate() { prev_[0] = 0; prev_[1] = 0; prev_[2] = W - 1; prev_[3] = H - 1; }

  Mood mood() const { return mood_; }

  void setMood(Mood m) { mood_ = m; if (m == SLEEPY) zPhase_ = 0; }
  void look(float x, float y) { lookTX_ = x; lookTY_ = y; }
  void blink() { if (blink_ < 0) { blink_ = 0; blinkEye_ = -1; } }
  void wink() { if (blink_ < 0) { blink_ = 0; blinkEye_ = 1; } }
  // The shake sensor fired: dizzy for 3 s, then back to calm.
  void shake() { setMood(DIZZY); dizzyUntil_ = time_ + 3; }
  void setAuto(bool on) { autoMode_ = on; }
  // 0..1 voice level while he speaks: the eyes squash gently in time with it.
  void setTalk(float level) { talk_ = level; }
  bool autoMode() const { return autoMode_; }

  // Serial-style one-letter control. Returns false for unknown letters.
  bool command(char c) {
    switch (c) {
      case 'n': setMood(NEUTRAL); break;
      case 'h': setMood(HAPPY); break;
      case 's': setMood(SAD); break;
      case 'a': setMood(ANGRY); break;
      case 'u': setMood(SURPRISED); break;
      case 'z': setMood(SLEEPY); break;
      case 'l': setMood(LOVE); break;
      case '?': setMood(SUSPICIOUS); break;
      case 'd': shake(); break;
      case 'b': blink(); break;
      case 'w': wink(); break;
      case '4': look(-1, 0); break;
      case '6': look(1, 0); break;
      case '8': look(0, -1); break;
      case '2': look(0, 1); break;
      case '5': look(0, 0); break;
      case 'x': setAuto(!autoMode_); return true;
      default: return false;
    }
    manualUntil_ = time_ + 15;   // hands off for a while after a command
    return true;
  }

  void update(float dt) {
    time_ += dt;
    intro();
    if (autoMode_ && time_ > 5) behave();
    if (mood_ == DIZZY && time_ > dizzyUntil_) setMood(NEUTRAL);

    if (blink_ >= 0) {
      blink_ += dt / (mood_ == SLEEPY ? 0.5f : 0.22f);   // ~4-5 frames at the Uno's frame rate
      if (blink_ >= 1) blink_ = -1;
    }
    float k = 1 - expf(-dt * 11);
    lookX_ += (lookTX_ - lookX_) * (1 - expf(-dt * 9));
    lookY_ += (lookTY_ - lookY_) * (1 - expf(-dt * 9));
    for (uint8_t s = 0; s < 2; s++) {
      Eye t = moodEye(mood_, s);
      float *a = &eye_[s].cx;
      const float *b = &t.cx;
      for (uint8_t i = 0; i < sizeof(Eye) / sizeof(float); i++) a[i] += (b[i] - a[i]) * k;
    }
    if (mood_ == SLEEPY) zPhase_ += dt * 0.45f;
  }

  // Sink: window(x0, y0, x1, y1) inclusive, row() before each row (a moment for
  // other work, e.g. audio), run(color, count), end().
  template <class Sink>
  void render(Sink &sink) {
    EyeDraw eyes[2];
    BlushDraw blush[2];
    ZDraw zs[2];
    int16_t box[4] = {W, H, -1, -1};
    for (uint8_t s = 0; s < 2; s++) {
      prepareEye(s, eyes[s], blush[s]);
      prepareZ(s, zs[s]);
      grow(box, eyes[s].bx); grow(box, blush[s].bx); grow(box, zs[s].bx);
    }

    int16_t win[4] = {box[0], box[1], box[2], box[3]};
    if (prev_[2] >= prev_[0]) {
      if (prev_[0] < win[0]) win[0] = prev_[0];
      if (prev_[1] < win[1]) win[1] = prev_[1];
      if (prev_[2] > win[2]) win[2] = prev_[2];
      if (prev_[3] > win[3]) win[3] = prev_[3];
    }
    if (win[0] < 0) win[0] = 0;
    if (win[1] < 0) win[1] = 0;
    if (win[2] > W - 1) win[2] = W - 1;
    if (win[3] > H - 1) win[3] = H - 1;
    for (uint8_t i = 0; i < 4; i++) prev_[i] = box[i];
    if (win[2] < win[0] || win[3] < win[1]) return;

    sink.window(win[0], win[1], win[2], win[3]);
    Seg seg[24];
    for (int16_t y = win[1]; y <= win[3]; y++) {
      sink.row();
      int16_t yc = 2 * y + 1;             // this row's center, in half pixels
      uint8_t n = 0;
      for (uint8_t s = 0; s < 2; s++)       // left eye then right eye: already in x order
        if (eyes[s].on && y >= eyes[s].bx[1] && y <= eyes[s].bx[3]) n = eyeRuns(eyes[s], yc, seg, n);
      uint8_t eyeRunCount = n;
      for (uint8_t s = 0; s < 2; s++) {     // extras go only where no eye run is
        if (blush[s].on && y >= blush[s].bx[1] && y <= blush[s].bx[3]) n = blushRow(blush[s], yc, seg, n);
        if (zs[s].on && y >= zs[s].bx[1] && y <= zs[s].bx[3]) n = zRow(zs[s], y + 0.5f, seg, n);
      }
      n = clipExtras(seg, eyeRunCount, n);
      emitRow(sink, seg, n, win[0], win[2] + 1);
    }
    sink.end();
  }

 private:
  // Everything a row needs, in half pixels (pixel centers are odd numbers).
  struct EyeDraw {
    bool on, heart, happyCut, glints;
    int16_t cx, cy, top, bot, xl, xr, r, dL, dR, lidMax;
    int32_t lidK;                         // Q8: x per unit of lid depth
    int16_t ecy, erx, ery;                // happy arch ellipse
    uint16_t eratio[3];                   // its rx / ry (Q8), per layer
    int16_t hs;                           // heart size
    uint16_t tk[3];                       // heart V slope (Q8), per layer
    int16_t gTop; uint16_t gInv;          // gradient: t8 = (y - gTop) * gInv >> 8
    uint8_t tr, tg, tb; int16_t dr, dg, db;
    uint16_t glow, halo, glint;
    int16_t g1x, g1y, g1rx, g1ry, g2x, g2y, g2r;
    uint16_t g1ratio;
    int16_t bx[4];                        // bounding box in pixels
  };
  struct BlushDraw { bool on; int16_t cx, cy, rx, ry; uint16_t ratio, color; int16_t bx[4]; };
  struct ZDraw { bool on; float x, y, s; uint16_t color; int16_t bx[4]; };

  Eye eye_[2];
  Mood mood_;
  float time_, lookX_, lookY_, lookTX_, lookTY_;
  float blink_;
  int8_t blinkEye_;
  bool autoMode_;
  float manualUntil_, nextBlink_, nextLook_, nextMood_, moodUntil_, dizzyUntil_ = 0;
  float zPhase_, talk_ = 0;
  int16_t prev_[4];
  uint32_t rng_ = 0x2545F491;

  uint32_t rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
  float rndf(float a, float b) { return a + (b - a) * (rnd() % 1000) / 1000.0f; }

  static void grow(int16_t *box, const int16_t *b) {
    if (b[2] < b[0]) return;
    if (b[0] < box[0]) box[0] = b[0];
    if (b[1] < box[1]) box[1] = b[1];
    if (b[2] > box[2]) box[2] = b[2];
    if (b[3] > box[3]) box[3] = b[3];
  }
  static void setBox(int16_t *b, float x0, float y0, float x1, float y1) {
    b[0] = px(x0) - 1; b[1] = px(y0) - 1; b[2] = px(x1) + 1; b[3] = px(y1) + 1;
  }
  static void noBox(int16_t *b) { b[0] = 1; b[1] = 1; b[2] = 0; b[3] = 0; }

  // Wake-up: eyes open, look left and right, then a happy hello.
  void intro() {
    if (time_ > 5) return;
    if (time_ > 1.3f && time_ < 1.9f) look(-0.9f, 0);
    else if (time_ > 1.9f && time_ < 2.5f) look(0.9f, 0);
    else if (time_ > 2.5f && time_ < 2.7f) look(0, 0);
    if (time_ > 2.8f && time_ < 3.0f) setMood(HAPPY);
    if (time_ > 4.6f) setMood(NEUTRAL);
  }

  void behave() {
    if (time_ > nextBlink_) {
      blink();
      nextBlink_ = time_ + (rnd() % 5 == 0 ? 0.35f : rndf(1.8f, 5.0f));  // sometimes a double blink
    }
    if (time_ < manualUntil_) return;
    if (time_ > nextLook_ && mood_ != SLEEPY) {
      // where he glances, in tenths; kept in flash to save the Uno's RAM
      static const int8_t spots[][2] PROGMEM = {{0, 0}, {0, 0}, {-10, 0}, {10, 0}, {-7, -8},
                                                {7, -8}, {-6, 7}, {6, 7}, {0, -10}};
      uint8_t i = rnd() % (sizeof spots / sizeof spots[0]);
      look((int8_t)pgm_read_byte(&spots[i][0]) * 0.1f, (int8_t)pgm_read_byte(&spots[i][1]) * 0.1f);
      nextLook_ = time_ + rndf(1.0f, 3.5f);
    }
    if (mood_ != NEUTRAL && time_ > moodUntil_) {
      setMood(NEUTRAL);
      nextMood_ = time_ + rndf(6, 12);
    } else if (mood_ == NEUTRAL && time_ > nextMood_) {
      setMood((Mood)(1 + rnd() % (SUSPICIOUS)));   // HAPPY..SUSPICIOUS
      if (mood_ == SUSPICIOUS) look(rnd() % 2 ? 1 : -1, 0);
      if (mood_ == SAD || mood_ == SLEEPY) look(0, 0.5f);
      moodUntil_ = time_ + rndf(3.5f, 6);
    }
  }

  // ---- drawing in half-pixel integers ----------------------------------------
  // The Uno has no FPU: one float sqrt costs ~30 us and the glow face needs
  // hundreds per frame. So each frame the float geometry is converted once to
  // half-pixel integers (pixel centers are the odd numbers), and rows use only
  // integer adds, multiplies and a 16-bit integer square root.

  static int16_t h2(float v) { return (int16_t)(v * 2 + 16384.5f) - 16384; }   // round(2 * v)

  // Integer square root, rounded to nearest (not floored: shapes keep their size).
  static uint8_t isqrt16(uint16_t n) {
    uint16_t res = 0, bit = 1u << 14, m = n;
    while (bit > m) bit >>= 2;
    while (bit) {
      if (m >= res + bit) { m -= res + bit; res = (res >> 1) + bit; } else res >>= 1;
      bit >>= 2;
    }
    return (uint8_t)(m > res ? res + 1 : res);   // m = n - res^2; round up past res + 0.5
  }

  // Half-width of an ellipse at vertical offset dy (half px), or -1 outside.
  // ratio = rx / ry in Q8, so rows need no division.
  static int16_t ellipseHW(int16_t dy, int16_t ry, uint16_t ratio) {
    if (ry <= 0 || dy <= -ry || dy >= ry) return -1;
    uint16_t s = isqrt16((uint16_t)((int32_t)ry * ry - (int32_t)dy * dy));
    return (int16_t)(((uint32_t)s * ratio) >> 8);
  }

  static uint16_t q8(int16_t num, int16_t den) { return den > 0 && num > 0 ? (uint16_t)(((int32_t)num << 8) / den) : 0; }

  // Per-frame setup: float geometry and colors -> half-pixel integers.
  void prepareEye(uint8_t s, EyeDraw &d, BlushDraw &b) {
    const Eye &e = eye_[s];
    // looking around: shift, and the eye on the side you look toward grows a bit
    float grow = 1 + 0.1f * lookX_ * (s == 1 ? 1 : -1);
    float w = e.w * grow, h = e.h * grow;
    float cx = e.cx + lookX_ * 18, cy = e.cy + lookY_ * 10;
    if (blink_ >= 0 && (blinkEye_ < 0 || blinkEye_ == s)) h *= 1 - 0.93f * sinf(3.14159f * blink_);
    h *= 1 - 0.14f * talk_; w *= 1 + 0.04f * talk_;                      // talking
    if (e.happy > 0.01f) cy += sinf(time_ * 12.57f) * 1.5f * e.happy;   // happy bounce
    if (mood_ == ANGRY) cx += sinf(time_ * 50) * 0.8f;                    // trembling
    if (e.dizzy > 0.01f) {                                                // eyes roll in opposite circles
      float a = time_ * 9 + (s == 0 ? 0 : 3.14159f);
      cx += cosf(a) * 8 * e.dizzy;
      cy += sinf(a) * 8 * e.dizzy;
    }
    d.cx = h2(cx); d.cy = h2(cy);
    d.glow = rgb565(e.red * 0.16f, e.green * 0.16f, e.blue * 0.16f);   // light spilling out, fading
    d.halo = rgb565(e.red * 0.46f, e.green * 0.46f, e.blue * 0.46f);
    d.glint = rgb565(e.red + (255 - e.red) * 0.85f, e.green + (255 - e.green) * 0.85f, e.blue + (255 - e.blue) * 0.85f);
    // lit from the top: a little white at the top, deeper at the bottom
    d.tr = clamp8(e.red + (255 - e.red) * 0.22f); d.tg = clamp8(e.green + (255 - e.green) * 0.22f);
    d.tb = clamp8(e.blue + (255 - e.blue) * 0.22f);
    d.dr = (int16_t)clamp8(e.red * 0.68f) - d.tr; d.dg = (int16_t)clamp8(e.green * 0.68f) - d.tg;
    d.db = (int16_t)clamp8(e.blue * 0.68f) - d.tb;
    d.on = true;
    d.glints = e.dizzy < 0.5f && mood_ != ANGRY;   // no sparkle when dizzy or angry

    if (e.heart > 0.5f) {
      float size = w * 1.18f * e.heart * (1 + 0.08f * sinf(time_ * 10));  // pulsing heart
      d.heart = true; d.happyCut = false;
      d.hs = h2(size);
      for (uint8_t l = 0; l < 3; l++) {   // V slope per layer: half-width at the top / height
        int16_t sz = d.hs + 2 * (int16_t)(l * 4);
        d.tk[l] = q8(sz >> 1, ((sz * 115) >> 8) + ((sz * 26) >> 8));
      }
      d.gTop = h2(cy - size * 0.40f); d.gInv = (uint16_t)(65535 / (h2(size * 0.85f) | 1));
      d.g1x = h2(cx - size * 0.27f); d.g1y = h2(cy - size * 0.17f);
      d.g1rx = h2(size * 0.075f) + 1; d.g1ry = h2(size * 0.06f) + 1; d.g1ratio = q8(d.g1rx, d.g1ry); d.g2r = 0;
      setBox(d.bx, cx - size * 0.53f - GLOW, cy - size * 0.41f - GLOW, cx + size * 0.53f + GLOW, cy + size * 0.46f + GLOW);
      b.on = e.blush > 0.05f;
      b.cx = h2(cx + (s == 0 ? -1 : 1) * size * 0.34f); b.cy = h2(cy + size * 0.30f);
      b.rx = h2(size * 0.2f);
    } else {
      float morph = e.heart * 2;            // shrink the eye away before the heart pops in
      w *= 1 - morph; h *= 1 - morph;
      d.heart = false;
      float r = fminf(e.r * grow, fminf(w, h) * 0.5f);
      float top = cy - h / 2, bottom = cy + h / 2;
      d.r = h2(r); d.top = h2(top); d.bot = h2(bottom); d.xl = h2(cx - w / 2); d.xr = h2(cx + w / 2);
      float inner = (e.lidIn + e.lidFlat) * h, outer = (e.lidOut + e.lidFlat) * h;
      d.dL = h2(s == 0 ? outer : inner);    // left eye's inner edge is its right side
      d.dR = h2(s == 0 ? inner : outer);
      d.lidMax = d.dL > d.dR ? d.dL : d.dR;
      d.lidK = d.dR == d.dL ? 0 : ((int32_t)(d.xr - d.xl) << 8) / (d.dR - d.dL);
      // happy: an ellipse rises from below and bites the eye into an arch, ^
      float ery = h * 0.72f;
      d.ery = h2(ery); d.erx = h2(w * 0.30f);
      d.ecy = h2(bottom + (1 - e.happy) * ery);
      d.happyCut = e.happy > 0.01f && ery > GLOW + 1;
      for (uint8_t l = 0; l < 3; l++) d.eratio[l] = q8(d.erx - (int16_t)(l * 4), d.ery - (int16_t)(l * 4));
      d.gTop = d.top; d.gInv = (uint16_t)(65535 / ((d.bot - d.top) | 1));
      // glints: a big one up-left and a small one below it, drifting with the gaze
      float gx = lookX_ * 2, gy = lookY_ * 1.5f;
      d.g1x = h2(cx - w / 2 + w * 0.31f + gx); d.g1y = h2(top + h * 0.27f + gy);
      d.g1rx = h2(w * 0.12f) + 1; d.g1ry = h2(h * 0.095f) + 1; d.g1ratio = q8(d.g1rx, d.g1ry);   // +1: keep small shapes their size
      d.g2x = h2(cx - w / 2 + w * 0.52f + gx); d.g2y = h2(top + h * 0.45f + gy); d.g2r = h2(w * 0.05f) + 1;
      if (h < 18 || d.g1ry < 1) d.glints = false;   // blinking: no room for sparkle
      if (w < 1 || h < 1) { d.on = false; noBox(d.bx); }
      else setBox(d.bx, cx - w / 2 - GLOW, top - GLOW, cx + w / 2 + GLOW, bottom + GLOW);
      b.on = e.blush > 0.05f && d.on;
      b.cx = h2(cx + (s == 0 ? -1 : 1) * w * 0.12f); b.cy = h2(bottom + 8);
      b.rx = h2(w * 0.30f);
    }
    float a = e.blush * 0.55f;
    b.color = rgb565(255 * a, 90 * a, 150 * a);
    b.ry = 9;                               // 4 px (+1 half px, like the glints)
    b.ratio = q8(b.rx, b.ry);
    if (b.on) setBox(b.bx, b.cx * 0.5f - b.rx * 0.5f, b.cy * 0.5f - 4, b.cx * 0.5f + b.rx * 0.5f, b.cy * 0.5f + 4);
    else noBox(b.bx);
  }

  void prepareZ(uint8_t i, ZDraw &z) {
    z.on = mood_ == SLEEPY;
    if (!z.on) { noBox(z.bx); return; }
    float p = fmodf(zPhase_ + i * 0.5f, 1.0f);
    z.s = 7 + p * 9;
    float a = sinf(3.14159f * p);
    z.x = 126 + p * 16 + i * 4;          // drifts up and to the right
    z.y = 40 - p * 32;
    z.color = rgb565(eye_[1].red * a, eye_[1].green * a, eye_[1].blue * a);
    setBox(z.bx, z.x, z.y, z.x + z.s, z.y + z.s);
  }

  // A run given in half pixels -> pixels (round half up, like px()).
  static uint8_t add2(Seg *out, uint8_t n, int16_t a, int16_t b, uint16_t color, uint8_t pri) {
    int16_t x0 = (a + 1) >> 1, x1 = (b + 1) >> 1;
    if (x1 > x0 && n < 24) { out[n].x0 = x0; out[n].x1 = x1; out[n].color = color; out[n].pri = pri; n++; }
    return n;
  }
  static uint8_t addF(Seg *out, uint8_t n, float a, float b, uint16_t color, uint8_t pri) {
    return add2(out, n, h2(a), h2(b), color, pri);
  }

  // Eye body grown by layer l (0 body, 1 halo, 2 glow), half pixels. Up to 2 intervals.
  static uint8_t eyeIv(const EyeDraw &d, int16_t yc, uint8_t l, int16_t iv[][2]) {
    int16_t g = (int16_t)(l * 4), top = d.top - g, bot = d.bot + g;
    if (yc < top || yc >= bot) return 0;
    int16_t r = d.r + g, dd = 0;
    if (yc < top + r) dd = top + r - yc; else if (yc > bot - r) dd = yc - (bot - r);
    int16_t inset = dd > 0 ? r - isqrt16((uint16_t)(r * r - dd * dd)) : 0;
    int16_t x0 = d.xl - g + inset, x1 = d.xr + g - inset;
    int16_t rel = yc - d.top + g;          // depth below the (grown) top edge
    if (rel < d.lidMax) {                  // only rows the lid can reach
      if (d.lidK == 0) {
        if (rel < d.dL) return 0;          // flat lid
      } else {
        int16_t xs = d.xl + (int16_t)(((int32_t)(rel - d.dL) * d.lidK) >> 8);   // slanted lid crosses here
        if (d.dR > d.dL) { if (xs < x1) x1 = xs; } else if (xs > x0) x0 = xs;
      }
    }
    if (x1 <= x0) return 0;
    if (d.happyCut) {
      int16_t hw = ellipseHW(yc - d.ecy, d.ery - g, d.eratio[l]);
      if (hw >= 0 && d.erx - g > 0) {
        uint8_t n = 0;
        int16_t a = d.cx - hw < x1 ? d.cx - hw : x1, b = d.cx + hw > x0 ? d.cx + hw : x0;
        if (a > x0) { iv[n][0] = x0; iv[n][1] = a; n++; }
        if (x1 > b) { iv[n][0] = b; iv[n][1] = x1; n++; }
        return n;
      }
    }
    iv[0][0] = x0; iv[0][1] = x1;
    return 1;
  }

  // Heart (two lobes on a V) grown by layer l, half pixels. Up to 3 intervals.
  static uint8_t heartIv(const EyeDraw &d, int16_t yc, uint8_t l, int16_t iv[][2]) {
    int16_t s = d.hs + 2 * (int16_t)(l * 4), raw[3][2];
    uint8_t k = 0;
    int16_t lr = (s * 69) >> 8, ly = d.cy - ((s * 33) >> 8), lx = s >> 2;   // 0.27, 0.13, 0.25 of s
    int16_t dy = yc - ly;
    if (dy > -lr && dy < lr) {
      int16_t hw = isqrt16((uint16_t)(lr * lr - dy * dy));
      raw[k][0] = d.cx - lx - hw; raw[k][1] = d.cx - lx + hw; k++;
      raw[k][0] = d.cx + lx - hw; raw[k][1] = d.cx + lx + hw; k++;
    }
    int16_t ty0 = d.cy - ((s * 26) >> 8), ty1 = d.cy + ((s * 115) >> 8);   // 0.10, 0.45 of s
    if (yc >= ty0 && yc < ty1) {
      int16_t hw = (int16_t)(((int32_t)(ty1 - yc) * d.tk[l]) >> 8);
      raw[k][0] = d.cx - hw; raw[k][1] = d.cx + hw; k++;
    }
    for (uint8_t i = 1; i < k; i++)
      for (uint8_t j = i; j > 0 && raw[j][0] < raw[j - 1][0]; j--) {
        int16_t a = raw[j][0], b = raw[j][1];
        raw[j][0] = raw[j - 1][0]; raw[j][1] = raw[j - 1][1]; raw[j - 1][0] = a; raw[j - 1][1] = b;
      }
    uint8_t n = 0;
    for (uint8_t i = 0; i < k; i++) {
      if (n && raw[i][0] <= iv[n - 1][1]) { if (raw[i][1] > iv[n - 1][1]) iv[n - 1][1] = raw[i][1]; }
      else { iv[n][0] = raw[i][0]; iv[n][1] = raw[i][1]; n++; }
    }
    return n;
  }

  // Glint ellipse clipped to the eye body intervals.
  static uint8_t glintRow(Seg *out, uint8_t n, int16_t yc, int16_t gx, int16_t gy, int16_t ry, uint16_t ratio,
                          const int16_t body[][2], uint8_t nb, uint16_t color) {
    int16_t hw = ellipseHW(yc - gy, ry, ratio);
    if (hw < 0) return n;
    for (uint8_t i = 0; i < nb; i++) {
      int16_t a = gx - hw > body[i][0] ? gx - hw : body[i][0], b = gx + hw < body[i][1] ? gx + hw : body[i][1];
      n = add2(out, n, a, b, color, P_GLINT);
    }
    return n;
  }

  // Append [a, b) half pixels as a pixel run, if it is not empty.
  static uint8_t put(Seg *out, uint8_t n, int16_t a, int16_t b, uint16_t color) {
    return a < b ? add2(out, n, a, b, color, 1) : n;
  }

  // Body interval [a, b) with the glints punched in.
  static uint8_t bodyRuns(const EyeDraw &d, int16_t yc, int16_t a, int16_t b, uint16_t body, Seg *out, uint8_t n) {
    int16_t g[2][2];
    uint8_t k = 0;
    if (d.glints) {
      int16_t hw = ellipseHW(yc - d.g1y, d.g1ry, d.g1ratio);
      if (hw >= 0) { g[k][0] = d.g1x - hw; g[k][1] = d.g1x + hw; k++; }
      if (d.g2r > 0) {
        hw = ellipseHW(yc - d.g2y, d.g2r, 256);
        if (hw >= 0) { g[k][0] = d.g2x - hw; g[k][1] = d.g2x + hw; k++; }
      }
      if (k == 2 && g[1][0] < g[0][0]) { int16_t t0 = g[0][0], t1 = g[0][1]; g[0][0] = g[1][0]; g[0][1] = g[1][1]; g[1][0] = t0; g[1][1] = t1; }
      if (k == 2 && g[1][0] <= g[0][1]) { if (g[1][1] > g[0][1]) g[0][1] = g[1][1]; k = 1; }   // overlapping glints merge
    }
    int16_t x = a;
    for (uint8_t i = 0; i < k; i++) {
      int16_t ga = g[i][0] > a ? g[i][0] : a, gb = g[i][1] < b ? g[i][1] : b;
      if (gb <= ga) continue;
      n = put(out, n, x, ga, body);
      n = put(out, n, ga, gb, d.glint);
      x = gb;
    }
    return put(out, n, x, b, body);
  }

  // One eye's row as disjoint runs, left to right. The layers are nested
  // (glow around halo around body around glints), so each is emitted around
  // the next one in a single pass: no overlap to resolve afterwards.
  static uint8_t eyeRuns(const EyeDraw &d, int16_t yc, Seg *out, uint8_t n) {
    int16_t G[3][2], Hl[3][2], B[3][2];
    uint8_t ng = d.heart ? heartIv(d, yc, 2, G) : eyeIv(d, yc, 2, G);
    if (!ng) return n;                   // outside the glow means outside everything
    uint8_t nh = d.heart ? heartIv(d, yc, 1, Hl) : eyeIv(d, yc, 1, Hl);
    uint8_t nb = d.heart ? heartIv(d, yc, 0, B) : eyeIv(d, yc, 0, B);
    uint16_t body = 0;
    if (nb) {
      int16_t t = yc - d.gTop;
      uint16_t t8 = t <= 0 ? 0 : (uint16_t)(((uint32_t)t * d.gInv) >> 8);
      if (t8 > 255) t8 = 255;
      uint8_t R = d.tr + (int16_t)(((int32_t)d.dr * t8) >> 8);
      uint8_t Gc = d.tg + (int16_t)(((int32_t)d.dg * t8) >> 8);
      uint8_t Bc = d.tb + (int16_t)(((int32_t)d.db * t8) >> 8);
      body = ((R & 0xF8) << 8) | ((Gc & 0xFC) << 3) | (Bc >> 3);
    }
    uint8_t ih = 0, ib = 0;
    for (uint8_t i = 0; i < ng; i++) {
      int16_t ga = G[i][0], gb = G[i][1], x = ga;
      for (; ih < nh && Hl[ih][0] < gb; ih++) {        // halo pieces inside this glow piece
        int16_t ha = Hl[ih][0] > x ? Hl[ih][0] : x, hb = Hl[ih][1] < gb ? Hl[ih][1] : gb;
        if (hb <= ha) continue;
        n = put(out, n, x, ha, d.glow);
        int16_t hx = ha;
        for (; ib < nb && B[ib][0] < hb; ib++) {       // body pieces inside this halo piece
          int16_t ba = B[ib][0] > hx ? B[ib][0] : hx, bb = B[ib][1] < hb ? B[ib][1] : hb;
          if (bb <= ba) continue;
          n = put(out, n, hx, ba, d.halo);
          n = bodyRuns(d, yc, ba, bb, body, out, n);
          hx = bb;
        }
        n = put(out, n, hx, hb, d.halo);
        x = hb;
      }
      n = put(out, n, x, gb, d.glow);
    }
    return n;
  }

  // Blush and Z runs (after the eye runs in seg) keep only the parts that
  // fall outside every eye run; result: all runs disjoint.
  static uint8_t clipExtras(Seg *seg, uint8_t eyeRuns, uint8_t n) {
    uint8_t out = eyeRuns;
    for (uint8_t i = eyeRuns; i < n; i++) {
      int16_t a = seg[i].x0, b = seg[i].x1;
      uint16_t c = seg[i].color;
      for (uint8_t j = 0; j < eyeRuns && a < b; j++) {
        if (seg[j].x1 <= a || seg[j].x0 >= b) continue;
        if (seg[j].x0 > a && out < 24) { seg[out].x0 = a; seg[out].x1 = seg[j].x0; seg[out].color = c; seg[out].pri = 1; out++; }
        a = seg[j].x1;
      }
      if (a < b && out < 24) { seg[out].x0 = a; seg[out].x1 = b; seg[out].color = c; seg[out].pri = 1; out++; }
    }
    return out;
  }

  static uint8_t blushRow(const BlushDraw &b, int16_t yc, Seg *out, uint8_t n) {
    int16_t hw = ellipseHW(yc - b.cy, b.ry, b.ratio);
    return hw < 0 ? n : add2(out, n, b.cx - hw, b.cx + hw, b.color, P_BLUSH);
  }

  static uint8_t zRow(const ZDraw &z, float yc, Seg *out, uint8_t n) {
    float s = z.s, t = fmaxf(2, s / 5), ry = yc - z.y;
    if (ry < 0 || ry >= s) return n;
    if (ry < t || ry >= s - t) return addF(out, n, z.x, z.x + s, z.color, P_BODY);
    float xd = z.x + (s - t) * (1 - (ry - t) / (s - 2 * t));
    return addF(out, n, xd, xd + t, z.color, P_BODY);
  }

  // Stream the row [x0, x1) from disjoint runs, black in the gaps.
  // Eye runs arrive in x order already; the few extras get sorted in.
  template <class Sink>
  static void emitRow(Sink &sink, Seg *seg, uint8_t n, int16_t x0, int16_t x1) {
    for (uint8_t i = 1; i < n; i++)
      for (uint8_t j = i; j > 0 && seg[j].x0 < seg[j - 1].x0; j--) { Seg t = seg[j]; seg[j] = seg[j - 1]; seg[j - 1] = t; }
    uint16_t runColor = 0;
    int16_t runLen = 0, x = x0;
    for (uint8_t i = 0; i <= n; i++) {
      int16_t a = i < n ? seg[i].x0 : x1, b = i < n ? seg[i].x1 : x1;
      a = a < x0 ? x0 : a > x1 ? x1 : a;   // clip to the window, both ends
      b = b < x0 ? x0 : b > x1 ? x1 : b;
      if (a > x) {                          // black gap before this run
        if (runColor == 0) runLen += a - x;
        else { if (runLen) sink.run(runColor, runLen); runColor = 0; runLen = a - x; }
        x = a;
      }
      if (i == n || b <= x) continue;
      uint16_t c = seg[i].color;
      if (c == runColor) runLen += b - x;
      else { if (runLen) sink.run(runColor, runLen); runColor = c; runLen = b - x; }
      x = b;
    }
    if (runLen) sink.run(runColor, runLen);
  }
};

}  // namespace emo
