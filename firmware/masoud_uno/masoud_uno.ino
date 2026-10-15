// Masoud, the lucky desk buddy: prototype face on an Arduino Uno and the
// Waveshare 1.8" LCD for micro:bit (ST7735S, 160x128).
// Boots on the Masoud logo, then the eyes live on their own: they blink,
// look around and now and then show a mood. Each mood has a Derja name,
// printed on Serial when it changes. Drive him over Serial 115200 with one
// letter at a time:
//   n Labes (calm)   h Mrigel! (happy)   s Mghboun (sad)   a Harissa! (angry)
//   u Ya mimti! (surprised)   z Na3sen (sleepy)   l Mghrom (in love)
//   ? Chkoun?! (suspicious)   d Dawkha! (dizzy: what a shake will trigger)
//   b blink  w wink  4/6/8/2 look left/right/up/down  5 center  x autonomous on/off
//
// Wiring: SCK=D13, MOSI=D11, CS=D10, DC=D9, RST=D8, VCC=3.3V (see README).

#include <SPI.h>
#include "emo_face.h"
#include "splash.h"

#define TFT_CS  10
#define TFT_DC   9
#define TFT_RST  8

const int16_t XOFF = 1, YOFF = 2;   // panel RAM offset in landscape
const SPISettings SPI_FAST(8000000, MSBFIRST, SPI_MODE0);

// cmd, nArgs, args..., delay ms (255 = 500 ms). Adafruit "green tab" setup,
// RGB order (verified on this panel).
const uint8_t INIT[] PROGMEM = {
  0x01, 0, 150,
  0x11, 0, 255,
  0xB1, 3, 0x01, 0x2C, 0x2D, 0,
  0xB2, 3, 0x01, 0x2C, 0x2D, 0,
  0xB3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D, 0,
  0xB4, 1, 0x07, 0,
  0xC0, 3, 0xA2, 0x02, 0x84, 0,
  0xC1, 1, 0xC5, 0,
  0xC2, 2, 0x0A, 0x00, 0,
  0xC3, 2, 0x8A, 0x2A, 0,
  0xC4, 2, 0x8A, 0xEE, 0,
  0xC5, 1, 0x0E, 0,
  0x20, 0, 0,
  0x36, 1, 0x60, 0,
  0x3A, 1, 0x05, 0,
  0xE0, 16, 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10, 0,
  0xE1, 16, 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10, 0,
  0x13, 0, 10,
  0x29, 0, 100,
};

inline void spiWrite(uint8_t b) {
  SPDR = b;
  while (!(SPSR & _BV(SPIF))) {}
}

void command(uint8_t c) {
  digitalWrite(TFT_DC, LOW);
  spiWrite(c);
  digitalWrite(TFT_DC, HIGH);
}

void initPanel() {
  digitalWrite(TFT_RST, LOW);
  delay(20);
  digitalWrite(TFT_RST, HIGH);
  delay(150);
  SPI.beginTransaction(SPI_FAST);
  digitalWrite(TFT_CS, LOW);
  for (uint16_t i = 0; i < sizeof INIT;) {
    command(pgm_read_byte(&INIT[i++]));
    uint8_t n = pgm_read_byte(&INIT[i++]);
    while (n--) spiWrite(pgm_read_byte(&INIT[i++]));
    uint8_t ms = pgm_read_byte(&INIT[i++]);
    if (ms) delay(ms == 255 ? 500 : ms);
  }
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

// Where emo::Face sends each frame: one window, then runs of color.
struct Panel {
  void window(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    SPI.beginTransaction(SPI_FAST);
    digitalWrite(TFT_CS, LOW);
    x0 += XOFF; x1 += XOFF; y0 += YOFF; y1 += YOFF;
    command(0x2A); spiWrite(x0 >> 8); spiWrite(x0); spiWrite(x1 >> 8); spiWrite(x1);
    command(0x2B); spiWrite(y0 >> 8); spiWrite(y0); spiWrite(y1 >> 8); spiWrite(y1);
    command(0x2C);
  }
  void run(uint16_t color, uint16_t n) {
    uint8_t hi = color >> 8, lo = color;
    while (n--) { spiWrite(hi); spiWrite(lo); }
  }
  void end() {
    digitalWrite(TFT_CS, HIGH);
    SPI.endTransaction();
  }
};

// Ask the panel for its power mode (RDDPM, 0Ah); it answers on its data
// line, so SPI is paused and the read is bit-banged on port B
// (D13=SCK, D11=SDA, D10=CS, D9=DC). 0x9C = awake with the display on.
uint8_t panelPowerMode() {
  const uint8_t sck = _BV(PB5), sda = _BV(PB3), cs = _BV(PB2), dc = _BV(PB1);
  uint8_t spcr = SPCR;
  SPCR = 0;
  PORTB &= ~(cs | dc | sck);
  uint8_t c = 0x0A;
  for (uint8_t i = 0; i < 8; i++) {
    if (c & 0x80) PORTB |= sda; else PORTB &= ~sda;
    delayMicroseconds(1);
    PORTB |= sck;
    delayMicroseconds(1);
    if (i == 7) { DDRB &= ~sda; PORTB |= sda; }  // release SDA for the answer
    PORTB &= ~sck;
    c <<= 1;
  }
  PORTB |= dc;
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; i++) {
    delayMicroseconds(1);
    PORTB |= sck;
    delayMicroseconds(1);
    v = (v << 1) | ((PINB & sda) ? 1 : 0);
    PORTB &= ~sck;
  }
  DDRB |= sda;
  PORTB |= cs;
  SPCR = spcr;
  return v;
}

// Mood names in Derja, in emo::Mood order.
const char M0[] PROGMEM = "Labes", M1[] PROGMEM = "Mrigel!", M2[] PROGMEM = "Mghboun",
           M3[] PROGMEM = "Harissa!", M4[] PROGMEM = "Ya mimti!", M5[] PROGMEM = "Na3sen",
           M6[] PROGMEM = "Mghrom", M7[] PROGMEM = "Chkoun?!", M8[] PROGMEM = "Dawkha! Bchwaya 3liya!";
const char *const MOOD_NAMES[] PROGMEM = {M0, M1, M2, M3, M4, M5, M6, M7, M8};

void printMood(emo::Mood m) {
  Serial.print('[');
  Serial.print((const __FlashStringHelper *)pgm_read_word(&MOOD_NAMES[m]));
  Serial.println(']');
}

void drawSplash() {
  Panel p;
  p.window(0, 0, emo::W - 1, emo::H - 1);
  for (int16_t y = 0; y < emo::H; y++)
    for (int16_t x = 0; x < emo::W; x++) p.run(splash::pixel(x, y), 1);
  p.end();
}

emo::Face face;
Panel panel;
emo::Mood shownMood = emo::NEUTRAL;
uint32_t lastMicros, lastCheck, fpsStart;
uint16_t frames;
uint32_t updateMicros, drawMicros;

void setup() {
  Serial.begin(115200);
  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  SPI.begin();
  initPanel();
  drawSplash();
  Serial.println(F("Aslema! Ena Masoud. Kol nhar, mas3oud."));
  Serial.println(F("Moods: n Labes, h Mrigel!, s Mghboun, a Harissa!, u Ya mimti!,"));
  Serial.println(F("       z Na3sen, l Mghrom, ? Chkoun?!, d Dawkha! (shake)"));
  Serial.println(F("b blink, w wink, 4 6 8 2 5 look, x autonomous on/off"));
  delay(2200);
  lastMicros = micros();
  lastCheck = fpsStart = millis();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (face.command(c)) {
      Serial.print(F("> ")); Serial.print(c);
      if (c == 'x') Serial.print(face.autoMode() ? F(" auto on") : F(" auto off"));
      Serial.println();
    }
  }

  uint32_t now = micros();
  float dt = (now - lastMicros) * 1e-6f;
  lastMicros = now;
  if (dt > 0.1f) dt = 0.1f;
  face.update(dt);
  if (face.mood() != shownMood) { shownMood = face.mood(); printMood(shownMood); }
  uint32_t t1 = micros();
  face.render(panel);
  drawMicros += micros() - t1;
  updateMicros += t1 - now;
  frames++;

  uint32_t ms = millis();
  if (ms - lastCheck > 2000) {
    lastCheck = ms;
    uint8_t pm = panelPowerMode();
    if (pm != 0x9C) {        // a loose RST/VCC wire reset the panel: set it up, repaint all
      Serial.print(F("panel was reset (power mode 0x")); Serial.print(pm, HEX); Serial.println(F(")"));
      initPanel();
      face.invalidate();
    }
  }
  if (ms - fpsStart >= 5000) {
    Serial.print(F("fps ")); Serial.print(frames * 1000UL / (ms - fpsStart));
    Serial.print(F("  (update ")); Serial.print(updateMicros / frames);
    Serial.print(F(" us, draw ")); Serial.print(drawMicros / frames); Serial.println(F(" us per frame)"));
    frames = 0;
    updateMicros = drawMicros = 0;
    fpsStart = ms;
  }
}
