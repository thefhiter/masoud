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

  // Sink: window(x0, y0, x1, y1) inclusive, run(color, count), end().
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
      float yc = y + 0.5f;
      uint8_t n = 0;
      for (uint8_t s = 0; s < 2; s++) {
        if (eyes[s].on && y >= eyes[s].bx[1] && y <= eyes[s].bx[3]) n = eyeRow(eyes[s], yc, seg, n);
        if (blush[s].on && y >= blush[s].bx[1] && y <= blush[s].bx[3]) n = blushRow(blush[s], yc, seg, n);
        if (zs[s].on && y >= zs[s].bx[1] && y <= zs[s].bx[3]) n = zRow(zs[s], yc, seg, n);
      }
      emitRow(sink, seg, n, win[0], win[2] + 1);
    }
    sink.end();
  }

 private:
  struct EyeDraw {
    bool on, heart, happyCut, glints;
    float cx, cy, w, h, r, top, bottom, xl, xr, dL, dR, lidK, lidMax;
    float ecy, erx, ery, iery, ieryH, ieryG;   // happy arch ellipse (inverse radii: no divisions per row)
    float hs;                           // heart size
    float gTop, gIh;                    // gradient: top y and 1/height
    float tr, tg, tb, dr, dg, db;       // gradient: top color and (bottom - top)
    uint16_t glow, halo, glint;
    float g1x, g1y, g1rx, g1iry, g1ry, g2x, g2y, g2r, g2ir;
    int16_t bx[4];
  };
  struct BlushDraw { bool on; float cx, cy, rx, ry, iry; uint16_t color; int16_t bx[4]; };
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

  // Per-frame setup: geometry, colors and bounding boxes, so rows only
  // do additions, multiplications and the odd sqrt.
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
    d.cx = cx; d.cy = cy;
    d.glow = rgb565(e.red * 0.16f, e.green * 0.16f, e.blue * 0.16f);   // light spilling out, fading
    d.halo = rgb565(e.red * 0.46f, e.green * 0.46f, e.blue * 0.46f);
    d.glint = rgb565(e.red + (255 - e.red) * 0.85f, e.green + (255 - e.green) * 0.85f, e.blue + (255 - e.blue) * 0.85f);
    // lit from the top: a little white at the top, deeper at the bottom
    d.tr = e.red + (255 - e.red) * 0.22f; d.tg = e.green + (255 - e.green) * 0.22f; d.tb = e.blue + (255 - e.blue) * 0.22f;
    d.dr = e.red * 0.68f - d.tr; d.dg = e.green * 0.68f - d.tg; d.db = e.blue * 0.68f - d.tb;
    d.on = true;
    d.glints = e.dizzy < 0.5f && mood_ != ANGRY;   // no sparkle when dizzy or angry

    if (e.heart > 0.5f) {
      float size = w * 1.18f * e.heart * (1 + 0.08f * sinf(time_ * 10));  // pulsing heart
      d.heart = true; d.happyCut = false;
      d.hs = size;
      d.gTop = cy - size * 0.40f; d.gIh = 1 / (size * 0.85f);
      d.g1x = cx - size * 0.27f; d.g1y = cy - size * 0.17f; d.g1rx = size * 0.075f; d.g1ry = size * 0.06f;
      d.g1iry = 1 / d.g1ry; d.g2r = 0;
      setBox(d.bx, cx - size * 0.53f - GLOW, cy - size * 0.41f - GLOW, cx + size * 0.53f + GLOW, cy + size * 0.46f + GLOW);
      b.on = e.blush > 0.05f;
      b.cx = cx + (s == 0 ? -1 : 1) * size * 0.34f; b.cy = cy + size * 0.30f;
      b.rx = size * 0.2f; b.ry = 4;
    } else {
      float morph = e.heart * 2;            // shrink the eye away before the heart pops in
      w *= 1 - morph; h *= 1 - morph;
      d.heart = false;
      d.w = w; d.h = h;
      d.r = fminf(e.r * grow, fminf(w, h) * 0.5f);
      d.top = cy - h / 2; d.bottom = cy + h / 2;
      d.xl = cx - w / 2; d.xr = cx + w / 2;
      float inner = (e.lidIn + e.lidFlat) * h, outer = (e.lidOut + e.lidFlat) * h;
      d.dL = s == 0 ? outer : inner;      // left eye's inner edge is its right side
      d.dR = s == 0 ? inner : outer;
      d.lidMax = fmaxf(d.dL, d.dR);
      d.lidK = fabsf(d.dR - d.dL) < 0.01f ? 0 : (d.xr - d.xl) / (d.dR - d.dL);
      // happy: an ellipse rises from below and bites the eye into an arch, ^
      d.ery = h * 0.72f; d.erx = w * 0.30f;
      d.ecy = d.bottom + (1 - e.happy) * d.ery;
      d.happyCut = e.happy > 0.01f && d.ery > GLOW + 1;
      d.iery = d.happyCut ? 1 / d.ery : 0;
      d.ieryH = d.happyCut ? 1 / (d.ery - HALO) : 0;
      d.ieryG = d.happyCut ? 1 / (d.ery - GLOW) : 0;
      d.gTop = d.top; d.gIh = h > 1 ? 1 / h : 0;
      // glints: a big one up-left and a small one below it, drifting with the gaze
      float gx = lookX_ * 2, gy = lookY_ * 1.5f;
      d.g1x = d.xl + w * 0.31f + gx; d.g1y = d.top + h * 0.27f + gy; d.g1rx = w * 0.12f; d.g1ry = h * 0.095f;
      d.g1iry = d.g1ry > 0.5f ? 1 / d.g1ry : 0;
      d.g2x = d.xl + w * 0.52f + gx; d.g2y = d.top + h * 0.45f + gy; d.g2r = w * 0.05f;
      d.g2ir = d.g2r > 0.5f ? 1 / d.g2r : 0;
      if (h < 18 || d.g1iry == 0) d.glints = false;   // blinking: no room for sparkle
      if (w < 1 || h < 1) { d.on = false; noBox(d.bx); }
      else setBox(d.bx, d.xl - GLOW, d.top - GLOW, d.xr + GLOW, d.bottom + GLOW);
      b.on = e.blush > 0.05f && d.on;
      b.cx = cx + (s == 0 ? -1 : 1) * w * 0.12f; b.cy = d.bottom + 8;
      b.rx = w * 0.30f; b.ry = 4;
    }
    float a = e.blush * 0.55f;
    b.color = rgb565(255 * a, 90 * a, 150 * a);
    b.iry = 1 / b.ry;
    if (b.on) setBox(b.bx, b.cx - b.rx, b.cy - b.ry, b.cx + b.rx, b.cy + b.ry); else noBox(b.bx);
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

  static uint8_t add(Seg *out, uint8_t n, float a, float b, uint16_t color, uint8_t pri) {
    int16_t x0 = px(a), x1 = px(b);
    if (x1 > x0 && n < 24) { out[n].x0 = x0; out[n].x1 = x1; out[n].color = color; out[n].pri = pri; n++; }
    return n;
  }

  // Eye body grown by g px (g = 0: the eye, g = GLOW: its glow ring). Up to 2 intervals.
  static uint8_t eyeIv(const EyeDraw &d, float yc, float g, float iv[][2]) {
    float top = d.top - g, bottom = d.bottom + g;
    if (yc < top || yc >= bottom) return 0;
    float r = d.r + g, dd = 0;
    if (yc < top + r) dd = top + r - yc; else if (yc > bottom - r) dd = yc - (bottom - r);
    float inset = dd > 0 ? r - sqrtf(fmaxf(0, r * r - dd * dd)) : 0;
    float x0 = d.xl - g + inset, x1 = d.xr + g - inset;
    float rel = yc - d.top + g;          // depth below the (grown) top edge
    if (rel < d.lidMax) {                // only rows the lid can reach
      if (d.lidK == 0) {
        if (rel < d.dL) return 0;        // flat lid
      } else {
        float xs = d.xl + (rel - d.dL) * d.lidK;   // where the slanted lid crosses this row
        if (d.dR > d.dL) x1 = fminf(x1, xs); else x0 = fmaxf(x0, xs);
      }
    }
    if (x1 <= x0) return 0;
    if (d.happyCut) {
      float erx = d.erx - g, t = (yc - d.ecy) * (g == 0 ? d.iery : g == HALO ? d.ieryH : d.ieryG);
      if (erx > 0 && t > -1 && t < 1) {
        float hw = erx * sqrtf(1 - t * t);
        uint8_t n = 0;
        float a = fminf(x1, d.cx - hw), b = fmaxf(x0, d.cx + hw);
        if (a > x0) { iv[n][0] = x0; iv[n][1] = a; n++; }
        if (x1 > b) { iv[n][0] = b; iv[n][1] = x1; n++; }
        return n;
      }
    }
    iv[0][0] = x0; iv[0][1] = x1;
    return 1;
  }

  // Heart (two lobes on a V) of size s + 2g. Up to 3 intervals.
  static uint8_t heartIv(const EyeDraw &d, float yc, float g, float iv[][2]) {
    float s = d.hs + 2 * g, raw[3][2];
    uint8_t k = 0;
    float lr = s * 0.27f, ly = d.cy - s * 0.13f;
    for (int8_t side = -1; side <= 1; side += 2) {
      float dy = yc - ly;
      if (fabsf(dy) < lr) {
        float hw = sqrtf(lr * lr - dy * dy), lx = d.cx + side * s * 0.25f;
        raw[k][0] = lx - hw; raw[k][1] = lx + hw; k++;
      }
    }
    float ty0 = d.cy - s * 0.10f, ty1 = d.cy + s * 0.45f;
    if (yc >= ty0 && yc < ty1) {
      float hw = s * 0.5f * (ty1 - yc) / (ty1 - ty0);
      raw[k][0] = d.cx - hw; raw[k][1] = d.cx + hw; k++;
    }
    for (uint8_t i = 1; i < k; i++)
      for (uint8_t j = i; j > 0 && raw[j][0] < raw[j - 1][0]; j--) {
        float a = raw[j][0], b = raw[j][1];
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
  static uint8_t glintRow(Seg *out, uint8_t n, float yc, float gx, float gy, float rx, float iry,
                          const float body[][2], uint8_t nb, uint16_t color) {
    float t = (yc - gy) * iry;
    if (t <= -1 || t >= 1) return n;
    float hw = rx * sqrtf(1 - t * t), a = gx - hw, b = gx + hw;
    for (uint8_t i = 0; i < nb; i++) n = add(out, n, fmaxf(a, body[i][0]), fminf(b, body[i][1]), color, P_GLINT);
    return n;
  }

  static uint8_t eyeRow(const EyeDraw &d, float yc, Seg *out, uint8_t n) {
    float iv[3][2];
    uint8_t k = d.heart ? heartIv(d, yc, GLOW, iv) : eyeIv(d, yc, GLOW, iv);
    for (uint8_t i = 0; i < k; i++) n = add(out, n, iv[i][0], iv[i][1], d.glow, P_GLOW);
    if (!k) return n;                    // outside the glow means outside everything
    k = d.heart ? heartIv(d, yc, HALO, iv) : eyeIv(d, yc, HALO, iv);
    for (uint8_t i = 0; i < k; i++) n = add(out, n, iv[i][0], iv[i][1], d.halo, P_HALO);
    k = d.heart ? heartIv(d, yc, 0, iv) : eyeIv(d, yc, 0, iv);
    if (!k) return n;
    float t = (yc - d.gTop) * d.gIh;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    uint16_t body = rgb565(d.tr + d.dr * t, d.tg + d.dg * t, d.tb + d.db * t);
    for (uint8_t i = 0; i < k; i++) n = add(out, n, iv[i][0], iv[i][1], body, P_BODY);
    if (d.glints) {
      n = glintRow(out, n, yc, d.g1x, d.g1y, d.g1rx, d.g1iry, iv, k, d.glint);
      if (d.g2r > 0.5f) n = glintRow(out, n, yc, d.g2x, d.g2y, d.g2r, d.g2ir, iv, k, d.glint);
    }
    return n;
  }

  static uint8_t blushRow(const BlushDraw &b, float yc, Seg *out, uint8_t n) {
    float t = (yc - b.cy) * b.iry;
    if (t <= -1 || t >= 1) return n;
    float hw = b.rx * sqrtf(1 - t * t);
    return add(out, n, b.cx - hw, b.cx + hw, b.color, P_BLUSH);
  }

  static uint8_t zRow(const ZDraw &z, float yc, Seg *out, uint8_t n) {
    float s = z.s, t = fmaxf(2, s / 5), ry = yc - z.y;
    if (ry < 0 || ry >= s) return n;
    if (ry < t || ry >= s - t) return add(out, n, z.x, z.x + s, z.color, P_BODY);
    float xd = z.x + (s - t) * (1 - (ry - t) / (s - 2 * t));
    return add(out, n, xd, xd + t, z.color, P_BODY);
  }

  // Resolve overlapping runs by priority and stream the row [x0, x1).
  template <class Sink>
  static void emitRow(Sink &sink, Seg *seg, uint8_t n, int16_t x0, int16_t x1) {
    int16_t b[50];
    uint8_t nb = 0;
    b[nb++] = x0; b[nb++] = x1;
    for (uint8_t i = 0; i < n; i++) {
      if (seg[i].x0 < x0) seg[i].x0 = x0;
      if (seg[i].x1 > x1) seg[i].x1 = x1;
      if (seg[i].x1 <= seg[i].x0) continue;
      b[nb++] = seg[i].x0; b[nb++] = seg[i].x1;
    }
    for (uint8_t i = 1; i < nb; i++)
      for (uint8_t j = i; j > 0 && b[j] < b[j - 1]; j--) { int16_t t = b[j]; b[j] = b[j - 1]; b[j - 1] = t; }
    uint16_t runColor = 0;
    int16_t runLen = 0;
    for (uint8_t i = 0; i + 1 < nb; i++) {
      int16_t a = b[i], c = b[i + 1];
      if (c <= a) continue;
      uint16_t color = 0;
      uint8_t best = 0;
      for (uint8_t j = 0; j < n; j++)
        if (seg[j].pri > best && seg[j].x0 <= a && seg[j].x1 >= c) { best = seg[j].pri; color = seg[j].color; }
      if (color == runColor) runLen += c - a;
      else {
        if (runLen) sink.run(runColor, runLen);
        runColor = color; runLen = c - a;
      }
    }
    if (runLen) sink.run(runColor, runLen);
  }
};

}  // namespace emo
