// Masoud's voice: 8-bit audio on D3 into the PAM8403 amp and two speakers,
// the same amp and speakers as the arduino-sound-player project.
//
// Wiring:
//   D3  -> PAM8403 left AND right input (same mono signal)
//          optional filter: D3 --[1k]--+-- amp input, 10 nF from there to GND
//   5V  -> PAM8403 VCC,  GND -> PAM8403 GND,  PAM8403 outputs -> the speakers
//
// Why D3: the sound project used D9/D10 (Timer1), but on Masoud those pins
// drive the screen (DC and CS). D3 is OC2B, Timer2's free PWM output, so:
//   Timer2  fast PWM 8-bit on OC2B (D3), no prescaler -> 62.5 kHz carrier
//   Timer1  CTC at 8 kHz -> sample clock (its own pins stay with the screen)
//   Timer0  untouched, so millis() and delay() keep working
//
// Two sources, the stream wins:
//   * chirps synthesized right here, one per mood (no PC needed)
//   * speech streamed from the PC over USB serial (tools/masoud_say.py),
//     same credit flow control as the sound project, at 115200 baud:
//     the PC sends 0x01, the robot answers OK:START and one 0x06 credit per
//     free CHUNK; the PC sends exactly CHUNK bytes per credit. A 250 ms gap
//     ends the stream (OK:IDLE:<underruns>).
#pragma once
#include <Arduino.h>
#include <util/atomic.h>

namespace audio {

const uint16_t RATE = 8000;
const uint16_t RING = 256;              // 32 ms of speech; the face needs the rest of the Uno's 2 KB
const uint8_t CHUNK = 32;
const uint8_t CREDIT = 0x06;
const uint8_t START = 0x01;             // byte that switches serial into stream mode
const uint16_t IDLE_MS = 250, GRACE_MS = 4000, PREBUFFER = 160;

// ---- synthesized chirps -----------------------------------------------------

enum Wave : uint8_t { TRI, SQUARE, REST };
struct Note { uint16_t f0, f1, ms; uint8_t wave, vol; };   // sweep f0 -> f1 Hz

#define N(f0, f1, ms, w, v) {f0, f1, ms, w, v}
const Note S_HELLO[] PROGMEM = {N(520, 880, 90, TRI, 190), N(880, 1320, 120, TRI, 190), N(0, 0, 40, REST, 0),
                                N(1320, 1180, 90, TRI, 170), {0, 0, 0, 0, 0}};
const Note S_HAPPY[] PROGMEM = {N(700, 1200, 70, TRI, 190), N(0, 0, 30, REST, 0), N(900, 1500, 110, TRI, 190), {0, 0, 0, 0, 0}};
const Note S_SAD[] PROGMEM = {N(620, 380, 380, TRI, 150), N(380, 300, 260, TRI, 120), {0, 0, 0, 0, 0}};
const Note S_ANGRY[] PROGMEM = {N(140, 120, 160, SQUARE, 150), N(0, 0, 40, REST, 0), N(150, 110, 220, SQUARE, 150), {0, 0, 0, 0, 0}};
const Note S_SURPRISED[] PROGMEM = {N(300, 1400, 140, TRI, 200), N(1400, 1300, 80, TRI, 170), {0, 0, 0, 0, 0}};
const Note S_SLEEPY[] PROGMEM = {N(220, 160, 500, TRI, 110), N(0, 0, 120, REST, 0), N(200, 150, 600, TRI, 90), {0, 0, 0, 0, 0}};
const Note S_LOVE[] PROGMEM = {N(800, 1000, 140, TRI, 170), N(0, 0, 40, REST, 0), N(1000, 800, 200, TRI, 170), {0, 0, 0, 0, 0}};
const Note S_SUSPICIOUS[] PROGMEM = {N(500, 520, 120, TRI, 170), N(0, 0, 90, REST, 0), N(480, 700, 160, TRI, 180), {0, 0, 0, 0, 0}};
const Note S_DIZZY[] PROGMEM = {N(600, 1100, 120, TRI, 180), N(1100, 500, 120, TRI, 180), N(500, 1000, 120, TRI, 180),
                                N(1000, 450, 120, TRI, 170), N(450, 900, 140, TRI, 160), {0, 0, 0, 0, 0}};
#undef N
// In emo::Mood order; NEUTRAL is silent.
const Note *const MOOD_SOUND[] = {nullptr, S_HAPPY, S_SAD, S_ANGRY, S_SURPRISED, S_SLEEPY, S_LOVE, S_SUSPICIOUS, S_DIZZY};

// ---- state shared with the sample ISR ------------------------------------------

static uint8_t ring[RING];
static volatile uint16_t head = 0, tail = 0, underruns = 0;
static volatile bool playing = false, feeding = false;
static volatile uint8_t last = 128, peak = 0;

static const Note *volatile song = nullptr;   // current chirp, in PROGMEM
static volatile uint16_t left = 0, done = 0;  // samples left / done in this note
static volatile uint32_t acc = 0, inc = 0;    // phase accumulator, 16.16-ish
static volatile int32_t dinc = 0;
static volatile uint8_t wave = REST, vol = 0;

static bool streaming = false, gotData = false;
static uint8_t outstanding = 0;
static uint16_t rxSinceCredit = 0;
static uint32_t lastAudio = 0;

// Load the next note of the current chirp (ISR context or interrupts off).
static void nextNote() {
  Note n;
  memcpy_P(&n, song, sizeof n);
  if (n.ms == 0) { song = nullptr; return; }
  song = song + 1;
  left = (uint32_t)n.ms * RATE / 1000; done = 0;
  wave = n.wave; vol = n.vol;
  inc = (uint32_t)n.f0 * 2097UL;               // 2^24 / 8000 per Hz
  dinc = ((int32_t)n.f1 - (int32_t)n.f0) * 2097L / (int32_t)(left ? left : 1);
}

static inline uint8_t synth() {
  if (!left) { if (song) nextNote(); if (!left) return 128; }
  left--; done++;
  acc += inc; inc += dinc;
  if (wave == REST) return 128;
  uint8_t ph = acc >> 16;
  int16_t v = wave == SQUARE ? (ph < 128 ? 90 : -90) : (ph < 128 ? (int16_t)ph * 2 - 128 : 383 - (int16_t)ph * 2);
  uint8_t env = done < 64 ? done * 4 : left < 128 ? left * 2 : 255;   // soft attack and release, no clicks
  return 128 + (int8_t)(((v * env) >> 8) * vol >> 8);
}

}  // namespace audio

ISR(TIMER1_COMPA_vect) {
  using namespace audio;
  uint8_t s;
  if (playing) {
    uint16_t t = tail;
    if (head != t) { s = ring[t]; tail = t + 1 == RING ? 0 : t + 1; last = s; }
    else { s = last; if (feeding) underruns++; }   // hold the level: a jump would click
  } else {
    s = synth();
  }
  uint8_t d = s > 128 ? s - 128 : 128 - s;
  if (d > peak) peak = d;
  OCR2B = s;
}

namespace audio {

static void begin() {
  pinMode(3, OUTPUT);
  TCCR2A = _BV(COM2B1) | _BV(WGM21) | _BV(WGM20);   // fast PWM 8-bit on OC2B (D3)
  TCCR2B = _BV(CS20);                               // no prescaler: 62.5 kHz carrier
  OCR2B = 128;
  TCCR1A = 0;                                       // Timer1: CTC sample clock
  TCCR1B = _BV(WGM12) | _BV(CS10);
  OCR1A = F_CPU / RATE - 1;
  TIMSK1 = _BV(OCIE1A);
}

// Play a chirp (ignored while speech is streaming).
static void play(const Note *n) {
  if (streaming || !n) return;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { song = n; left = 0; }
}
static void playMood(uint8_t mood) { if (mood < sizeof MOOD_SOUND / sizeof MOOD_SOUND[0]) play(MOOD_SOUND[mood]); }
static void hello() { play(S_HELLO); }

// Loudest sample since the last call, 0..128 (drives the talking eyes).
static uint8_t takePeak() { uint8_t p; ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { p = peak; peak = 0; } return p; }
static bool talking() { return playing; }
static bool isStreaming() { return streaming; }

static void beginStream() {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { head = tail = 0; song = nullptr; left = 0; }
  outstanding = 0; rxSinceCredit = 0; underruns = 0;
  streaming = true; gotData = false; playing = false; feeding = false;
  lastAudio = millis();
  Serial.println(F("OK:START"));
}

static void endStream() {
  streaming = false; playing = false; feeding = false;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { head = tail = 0; }
  OCR2B = 128;
  Serial.print(F("OK:IDLE:"));
  Serial.println(underruns);
}

// Move received speech into the ring and hand out credits. Call it often:
// the face renderer calls it between rows, so the 64-byte serial buffer never
// overflows even mid-frame.
static void pump() {
  if (!streaming) return;
  uint16_t t, h = head;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { t = tail; }
  int n = Serial.available();
  if (n > 0) {
    while (n-- > 0) {
      uint16_t next = h + 1 == RING ? 0 : h + 1;
      if (next == t) { Serial.read(); continue; }     // full; credits make this impossible
      ring[h] = (uint8_t)Serial.read();
      h = next;
      rxSinceCredit++;
    }
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { head = h; }
    lastAudio = millis();
    gotData = true;
  }
  while (rxSinceCredit >= CHUNK && outstanding > 0) { rxSinceCredit -= CHUNK; outstanding--; }
  uint16_t used = h >= t ? h - t : RING - t + h;
  uint16_t room = RING - 1 - used;
  while (room >= (uint16_t)(outstanding + 1) * CHUNK) { Serial.write(CREDIT); outstanding++; }
  feeding = gotData && millis() - lastAudio < 50;
  if (!playing && (used >= PREBUFFER || (gotData && millis() - lastAudio > 100))) playing = true;
  if (used == 0 && millis() - lastAudio > (gotData ? IDLE_MS : GRACE_MS)) endStream();
}

}  // namespace audio
