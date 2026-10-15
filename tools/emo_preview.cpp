// PC preview of firmware/masoud_uno/emo_face.h - the exact code the Uno runs.
//
//   python -m ziglang cc -x c++ -std=c++17 -O2 -fno-exceptions -fno-rtti
//       -fno-threadsafe-statics -Itools/shim tools/emo_preview.cpp -o emo_preview.exe
// (zig cc + those flags needs no C++ runtime, so it builds in seconds)
//   emo_preview.exe out_dir
//
// Writes out_dir/<mood>.ppm stills and out_dir/frames.raw (25 fps RGB565
// frames of a scripted run), and checks every frame that the partial
// redraw gives the same picture as repainting the whole screen.
#include <stdio.h>
#include <string.h>

#include <avr/pgmspace.h>   // tools/shim on the PC

#include "../firmware/masoud_uno/emo_face.h"
#include "../firmware/masoud_uno/splash.h"

struct Screen {
  uint16_t fb[emo::H][emo::W];
  int16_t x0, y0, x1, y1, x, y;
  long pixels = 0, maxWindow = 0, windows = 0, windowArea = 0;

  Screen() { memset(fb, 0, sizeof fb); }
  void window(int16_t a, int16_t b, int16_t c, int16_t d) {
    x0 = a; y0 = b; x1 = c; y1 = d; x = a; y = b;
    long area = (long)(c - a + 1) * (d - b + 1);
    windows++; windowArea += area;
    if (area > maxWindow) maxWindow = area;
  }
  void run(uint16_t color, uint16_t n) {
    while (n--) {
      if (y > y1) { fprintf(stderr, "run past window\n"); return; }
      fb[y][x] = color;
      pixels++;
      if (++x > x1) { x = x0; y++; }
    }
  }
  void end() {
    if (y != y1 + 1 || x != x0) fprintf(stderr, "window not filled exactly (%d,%d)\n", x, y);
  }
};

static const char *outDir = ".";

static void savePPM(const Screen &s, const char *name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s.ppm", outDir, name);
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6 %d %d 255\n", emo::W, emo::H);
  for (int y = 0; y < emo::H; y++)
    for (int x = 0; x < emo::W; x++) {
      uint16_t c = s.fb[y][x];
      unsigned char rgb[3] = {(unsigned char)((c >> 8) & 0xF8), (unsigned char)((c >> 3) & 0xFC),
                              (unsigned char)((c << 3) & 0xF8)};
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}

int main(int argc, char **argv) {
  if (argc > 1) outDir = argv[1];
  emo::Face face;
  static Screen screen;           // persistent, like the real panel
  char rawPath[512];
  snprintf(rawPath, sizeof rawPath, "%s/frames.raw", outDir);
  FILE *raw = fopen(rawPath, "wb");
  const float dt = 1.0f / 25;
  long frame = 0, mismatches = 0;

  {
    static Screen boot;
    boot.window(0, 0, emo::W - 1, emo::H - 1);
    for (int16_t y = 0; y < emo::H; y++)
      for (int16_t x = 0; x < emo::W; x++) boot.run(splash::pixel(x, y), 1);
    boot.end();
    savePPM(boot, "splash");
  }

  auto step = [&](float seconds) {
    for (int i = 0; i < (int)(seconds * 25 + 0.5f); i++) {
      face.update(dt);
      emo::Face ref = face;       // same state, but repaint everything
      ref.invalidate();
      face.render(screen);
      static Screen full;
      memset(full.fb, 0, sizeof full.fb);
      ref.render(full);
      if (memcmp(full.fb, screen.fb, sizeof full.fb) != 0) mismatches++;
      fwrite(screen.fb, 1, sizeof screen.fb, raw);
      frame++;
    }
  };

  step(5.2f);                                 // wake-up intro
  savePPM(screen, "neutral");
  face.setAuto(false);
  const char *names[] = {"neutral", "happy", "sad", "angry", "surprised", "sleepy", "love", "suspicious", "dizzy"};
  const char keys[] = "nhsauzl?d";
  for (int m = 1; m < 9; m++) {
    face.command(keys[m]);
    if (m == 7) face.look(1, 0);
    step(m == 8 ? 1.0f : 1.6f);                // dizzy wears off after 3 s
    savePPM(screen, names[m]);
    face.command('n');
    face.look(0, 0);
    step(0.8f);
  }
  face.look(-1, 0); step(0.8f); savePPM(screen, "look_left");
  face.look(0, 0); step(0.5f);
  face.blink(); step(0.08f); savePPM(screen, "blink");
  step(0.5f);
  face.wink(); step(0.08f); savePPM(screen, "wink");
  step(0.5f);
  face.setAuto(true);
  step(25);                                   // a stretch of autonomous behavior
  fclose(raw);

  printf("frames %ld, partial-vs-full mismatches %ld\n", frame, mismatches);
  printf("window: avg %ld px, max %ld px (full screen %d)\n", screen.windowArea / screen.windows,
         screen.maxWindow, emo::W * emo::H);
  return mismatches ? 1 : 0;
}
