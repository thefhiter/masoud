// Masoud's face: EMO-style animated robot eyes for a 160x128 screen.
//
// Plain C++ with no Arduino calls, so the same code runs on the Uno and in
// the PC preview (tools/emo_preview.cpp). Each frame is drawn row by row as
// runs of color inside one window that covers last frame's eyes and this
// frame's: every pixel is written once with its final color, so there is no
// flicker and no frame buffer (a 160x128 frame would be 40 KB; the Uno has 2).
#pragma once
#include <stdint.h>
#include <math.h>

namespace emo {

const int16_t W = 160, H = 128;

// DIZZY only comes from a shake (shake() / key d), never from the random moods.
enum Mood : uint8_t { NEUTRAL, HAPPY, SAD, ANGRY, SURPRISED, SLEEPY, LOVE, SUSPICIOUS, DIZZY, MOOD_COUNT };

struct Eye {
  float cx, cy, w, h, r;   // center, size, corner radius (px)
  float lidIn, lidOut;     // slanted top lid at the inner / outer edge (fraction of h)
  float lidFlat;           // flat top lid (fraction of h)
  float happy;             // 0..1: bottom cut into an arc, like ^
  float heart;             // 0..1: becomes a heart of this scale
  float dizzy;             // 0..1: round eyes rolling around each other
  float red, green, blue;  // 0..255
};

inline uint16_t rgb565(float r, float g, float b) {
  uint8_t R = r < 0 ? 0 : r > 255 ? 255 : (uint8_t)r;
  uint8_t G = g < 0 ? 0 : g > 255 ? 255 : (uint8_t)g;
  uint8_t B = b < 0 ? 0 : b > 255 ? 255 : (uint8_t)b;
  return ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3);
}

// Round to the nearest pixel. Cheaper than floorf on the AVR's software floats.
inline int16_t px(float v) { return (int16_t)(v + 16384.5f) - 16384; }

// Where a mood wants an eye to be (before looking around and blinking).
inline Eye moodEye(Mood m, uint8_t side) {
  Eye e = {side == 0 ? 48.0f : 112.0f, 62, 44, 52, 12, 0, 0, 0, 0, 0, 0, 0, 215, 255};
  switch (m) {
    case HAPPY:      e.h = 42; e.cy = 58; e.r = 19; e.happy = 1; break;
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

struct Span { int16_t x0, x1; uint16_t color; };   // pixels x0..x1-1

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
      blink_ += dt / (mood_ == SLEEPY ? 0.5f : 0.22f);   // ~4-5 frames at the Uno's ~20 fps
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
    Shape sh[4];
    uint8_t n = 0;
    for (uint8_t s = 0; s < 2; s++) sh[n++] = eyeShape(s);
    if (mood_ == SLEEPY || zFade() > 0.01f)
      for (uint8_t i = 0; i < 2; i++) sh[n++] = zShape(i);

    int16_t box[4] = {W, H, -1, -1};
    for (uint8_t i = 0; i < n; i++) {
      if (sh[i].x1 < sh[i].x0) continue;
      if (sh[i].x0 < box[0]) box[0] = sh[i].x0;
      if (sh[i].y0 < box[1]) box[1] = sh[i].y0;
      if (sh[i].x1 > box[2]) box[2] = sh[i].x1;
      if (sh[i].y1 > box[3]) box[3] = sh[i].y1;
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
    Span spans[12];
    for (int16_t y = win[1]; y <= win[3]; y++) {
      uint8_t c = 0;
      for (uint8_t i = 0; i < n; i++)
        if (y >= sh[i].y0 && y <= sh[i].y1) c += rowSpans(sh[i], y, spans + c);
      for (uint8_t i = 1; i < c; i++)            // few spans: insertion sort by x0
        for (uint8_t j = i; j > 0 && spans[j].x0 < spans[j - 1].x0; j--) {
          Span t = spans[j]; spans[j] = spans[j - 1]; spans[j - 1] = t;
        }
      int16_t x = win[0];
      for (uint8_t i = 0; i < c; i++) {
        int16_t a = spans[i].x0 < x ? x : spans[i].x0;
        int16_t b = spans[i].x1 > win[2] + 1 ? win[2] + 1 : spans[i].x1;
        if (b <= a) continue;
        if (a > x) sink.run(0, a - x);
        sink.run(spans[i].color, b - a);
        x = b;
      }
      if (x <= win[2]) sink.run(0, win[2] + 1 - x);
    }
    sink.end();
  }

 private:
  enum Kind : uint8_t { EYE, HEART, ZED };

  struct Shape {
    Kind kind;
    uint16_t color;
    int16_t x0, y0, x1, y1;               // bounding box, inclusive
    float cx, cy, w, h, r;                // eye: rounded rect / heart: size in w
    float top, bottom, xl, xr, dL, dR;    // eye edges and lid depth at each edge (px)
    float ecy, erx, ery;                  // eye: happy cut ellipse
    // per-frame constants so rows need no divisions (AVR floats are slow)
    float r2, lidMax, lidK, iery;
  };

  Eye eye_[2];
  Mood mood_;
  float time_, lookX_, lookY_, lookTX_, lookTY_;
  float blink_;
  int8_t blinkEye_;
  bool autoMode_;
  float manualUntil_, nextBlink_, nextLook_, nextMood_, moodUntil_, dizzyUntil_ = 0;
  float zPhase_;
  int16_t prev_[4];
  uint32_t rng_ = 0x2545F491;

  uint32_t rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
  float rndf(float a, float b) { return a + (b - a) * (rnd() % 1000) / 1000.0f; }

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
      static const float spots[][2] = {{0, 0}, {0, 0}, {-1, 0}, {1, 0}, {-0.7f, -0.8f},
                                       {0.7f, -0.8f}, {-0.6f, 0.7f}, {0.6f, 0.7f}, {0, -1}};
      uint8_t i = rnd() % (sizeof spots / sizeof spots[0]);
      look(spots[i][0], spots[i][1]);
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

  float zFade() const { return mood_ == SLEEPY ? 1 : 0; }

  Shape eyeShape(uint8_t s) {
    const Eye &e = eye_[s];
    Shape sh;
    // looking around: shift, and the eye on the side you look toward grows a bit
    float grow = 1 + 0.1f * lookX_ * (s == 1 ? 1 : -1);
    float w = e.w * grow, h = e.h * grow;
    float cx = e.cx + lookX_ * 18, cy = e.cy + lookY_ * 10;
    if (blink_ >= 0 && (blinkEye_ < 0 || blinkEye_ == s)) h *= 1 - 0.93f * sinf(3.14159f * blink_);
    if (e.happy > 0.01f) cy += sinf(time_ * 12.57f) * 1.5f * e.happy;   // happy bounce
    if (mood_ == ANGRY) cx += sinf(time_ * 50) * 0.8f;                    // trembling
    if (e.dizzy > 0.01f) {                                                // eyes roll in opposite circles
      float a = time_ * 9 + (s == 0 ? 0 : 3.14159f);
      cx += cosf(a) * 8 * e.dizzy;
      cy += sinf(a) * 8 * e.dizzy;
    }
    sh.color = rgb565(e.red, e.green, e.blue);
    sh.cx = cx; sh.cy = cy;

    if (e.heart > 0.5f) {
      float size = w * 1.18f * e.heart * (1 + 0.08f * sinf(time_ * 10));  // pulsing heart
      sh.kind = HEART;
      sh.w = size;
      sh.x0 = px(cx - size * 0.53f) - 1; sh.x1 = px(cx + size * 0.53f) + 1;
      sh.y0 = px(cy - size * 0.41f) - 1; sh.y1 = px(cy + size * 0.46f) + 1;
      return sh;
    }
    float morph = e.heart * 2;            // shrink the eye away before the heart pops in
    w *= 1 - morph; h *= 1 - morph;
    sh.kind = EYE;
    sh.w = w; sh.h = h;
    sh.r = fminf(e.r * grow, fminf(w, h) * 0.5f);
    sh.top = cy - h / 2; sh.bottom = cy + h / 2;
    sh.xl = cx - w / 2; sh.xr = cx + w / 2;
    float inner = (e.lidIn + e.lidFlat) * h, outer = (e.lidOut + e.lidFlat) * h;
    sh.dL = s == 0 ? outer : inner;      // left eye's inner edge is its right side
    sh.dR = s == 0 ? inner : outer;
    // happy: an ellipse rises from below and bites the eye into an arch, ^
    sh.ery = h * 0.72f;
    sh.erx = w * 0.30f;
    sh.ecy = sh.bottom + (1 - e.happy) * sh.ery;
    sh.r2 = sh.r * sh.r;
    sh.lidMax = fmaxf(sh.dL, sh.dR);
    sh.lidK = fabsf(sh.dR - sh.dL) < 0.01f ? 0 : (sh.xr - sh.xl) / (sh.dR - sh.dL);
    sh.iery = sh.ery > 0.01f ? 1 / sh.ery : 0;
    if (w < 1 || h < 1) { sh.x0 = 1; sh.x1 = 0; sh.y0 = 1; sh.y1 = 0; return sh; }
    sh.x0 = px(sh.xl) - 1; sh.x1 = px(sh.xr) + 1;
    sh.y0 = px(sh.top) - 1; sh.y1 = px(sh.bottom) + 1;
    return sh;
  }

  Shape zShape(uint8_t i) {
    Shape sh;
    float p = fmodf(zPhase_ + i * 0.5f, 1.0f);
    float size = 7 + p * 9;
    float a = sinf(3.14159f * p) * zFade();
    sh.kind = ZED;
    sh.w = size;
    sh.cx = 126 + p * 16 + i * 4;          // drifts up and to the right
    sh.cy = 40 - p * 32;
    sh.color = rgb565(eye_[1].red * a, eye_[1].green * a, eye_[1].blue * a);
    sh.x0 = px(sh.cx); sh.x1 = px(sh.cx + size);
    sh.y0 = px(sh.cy); sh.y1 = px(sh.cy + size);
    return sh;
  }

  static uint8_t addSpan(Span *out, uint8_t n, float a, float b, uint16_t color) {
    int16_t x0 = px(a), x1 = px(b);
    if (x1 > x0) { out[n].x0 = x0; out[n].x1 = x1; out[n].color = color; n++; }
    return n;
  }

  // Up to 3 spans of shape `sh` on row y.
  static uint8_t rowSpans(const Shape &sh, int16_t y, Span *out) {
    float yc = y + 0.5f;
    if (sh.kind == ZED) {
      float s = sh.w, t = fmaxf(2, s / 5), ry = yc - sh.cy;
      if (ry < 0 || ry >= s) return 0;
      if (ry < t || ry >= s - t) return addSpan(out, 0, sh.cx, sh.cx + s, sh.color);
      float xd = sh.cx + (s - t) * (1 - (ry - t) / (s - 2 * t));
      return addSpan(out, 0, xd, xd + t, sh.color);
    }
    if (sh.kind == HEART) {
      // two round lobes on top of a V
      float s = sh.w, iv[3][2];
      uint8_t k = 0;
      float lr = s * 0.27f, ly = sh.cy - s * 0.13f;
      for (int8_t side = -1; side <= 1; side += 2) {
        float d = yc - ly;
        if (fabsf(d) < lr) {
          float hw = sqrtf(lr * lr - d * d), lx = sh.cx + side * s * 0.25f;
          iv[k][0] = lx - hw; iv[k][1] = lx + hw; k++;
        }
      }
      float ty0 = sh.cy - s * 0.10f, ty1 = sh.cy + s * 0.45f;
      if (yc >= ty0 && yc < ty1) {
        float hw = s * 0.5f * (ty1 - yc) / (ty1 - ty0);
        iv[k][0] = sh.cx - hw; iv[k][1] = sh.cx + hw; k++;
      }
      for (uint8_t i = 1; i < k; i++)
        for (uint8_t j = i; j > 0 && iv[j][0] < iv[j - 1][0]; j--) {
          float a = iv[j][0], b = iv[j][1];
          iv[j][0] = iv[j - 1][0]; iv[j][1] = iv[j - 1][1]; iv[j - 1][0] = a; iv[j - 1][1] = b;
        }
      uint8_t n = 0;
      float a = 0, b = -1;
      for (uint8_t i = 0; i < k; i++) {
        if (b < a) { a = iv[i][0]; b = iv[i][1]; }
        else if (iv[i][0] <= b) { if (iv[i][1] > b) b = iv[i][1]; }
        else { n = addSpan(out, n, a, b, sh.color); a = iv[i][0]; b = iv[i][1]; }
      }
      if (b >= a) n = addSpan(out, n, a, b, sh.color);
      return n;
    }
    // eye: rounded rectangle, minus the lid above a line, minus the happy ellipse
    if (yc < sh.top || yc >= sh.bottom) return 0;
    float inset = 0, d = 0;
    if (yc < sh.top + sh.r) d = sh.top + sh.r - yc;
    else if (yc > sh.bottom - sh.r) d = yc - (sh.bottom - sh.r);
    if (d > 0) inset = sh.r - sqrtf(fmaxf(0, sh.r2 - d * d));
    float x0 = sh.xl + inset, x1 = sh.xr - inset;
    float dy = yc - sh.top;
    if (dy < sh.lidMax) {                // only rows the lid can reach
      if (sh.lidK == 0) {
        if (dy < sh.dL) return 0;        // flat lid
      } else {
        float xs = sh.xl + (dy - sh.dL) * sh.lidK;   // where the slanted lid crosses this row
        if (sh.dR > sh.dL) x1 = fminf(x1, xs); else x0 = fmaxf(x0, xs);
      }
    }
    if (x1 <= x0) return 0;
    float t = (yc - sh.ecy) * sh.iery;
    if (sh.iery > 0 && t > -1 && t < 1) {
      float hw = sh.erx * sqrtf(1 - t * t);
      uint8_t n = addSpan(out, 0, x0, fminf(x1, sh.cx - hw), sh.color);
      return addSpan(out, n, fmaxf(x0, sh.cx + hw), x1, sh.color);
    }
    return addSpan(out, 0, x0, x1, sh.color);
  }
};

}  // namespace emo
