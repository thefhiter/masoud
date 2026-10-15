// Boot screen: Masoud's winking face on Lucky Yellow, above the "masoud"
// wordmark. One pixel at a time with integer math; plain C++ like
// emo_face.h, so the PC preview draws exactly what the robot draws.
#pragma once
#include <stdint.h>
#include "brand_splash.h"

namespace splash {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t YELLOW = rgb(255, 198, 26), NIGHT = rgb(10, 14, 23), CYAN = rgb(0, 215, 255);

// Inside a rounded rectangle [x0, x1) x [y0, y1) with corner radius r?
// Works in half pixels so the test is centered on each pixel.
inline bool roundRect(int16_t x, int16_t y, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t r) {
  int32_t px = 2 * x + 1, py = 2 * y + 1, R = 2 * r;
  if (px < 2 * x0 || px > 2 * x1 || py < 2 * y0 || py > 2 * y1) return false;
  int32_t dx = 0, dy = 0;
  if (px < 2 * x0 + R) dx = 2 * x0 + R - px; else if (px > 2 * x1 - R) dx = px - (2 * x1 - R);
  if (py < 2 * y0 + R) dy = 2 * y0 + R - py; else if (py > 2 * y1 - R) dy = py - (2 * y1 - R);
  return dx * dx + dy * dy <= R * R;
}

// The wink: an upside-down U centered at ex, ey (radius 10, 6 px thick, 9 px legs).
inline bool wink(int16_t x, int16_t y, int16_t ex, int16_t ey) {
  int32_t ux = 2 * x + 1 - 2 * ex, uy = 2 * y + 1 - 2 * ey;
  int32_t ax = ux < 0 ? -ux : ux;
  if (uy <= 0) { int32_t r2 = ux * ux + uy * uy; return r2 <= 400 && r2 >= 64; }
  return uy <= 18 && ax >= 8 && ax <= 20;
}

inline uint16_t pixel(int16_t x, int16_t y) {
  const int16_t wx = (160 - WORDMARK_W) / 2, wy = 92;
  if (y >= wy && y < wy + WORDMARK_H && x >= wx && x < wx + WORDMARK_W) {
    uint8_t b = pgm_read_byte(&WORDMARK[(y - wy) * WORDMARK_STRIDE + (x - wx) / 8]);
    return (b & (0x80 >> ((x - wx) & 7))) ? NIGHT : YELLOW;
  }
  if (roundRect(x, y, 42, 14, 118, 80, 22)) {                 // the head
    if (roundRect(x, y, 56, 32, 78, 62, 8)) return CYAN;      // open eye
    if (wink(x, y, 95, 46)) return CYAN;                      // winking eye
    return NIGHT;
  }
  return YELLOW;
}

}  // namespace splash
