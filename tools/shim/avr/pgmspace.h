// PC stand-in for the AVR flash-memory macros, so the preview can include
// the same headers as the Uno.
#pragma once
#define PROGMEM
#define pgm_read_byte(p) (*(const unsigned char *)(p))
#define pgm_read_word(p) (*(const unsigned short *)(p))
