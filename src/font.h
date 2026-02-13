#ifndef FONT_H
#define FONT_H

#include <efi.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#define FONT_FIRST  32  /* space */
#define FONT_LAST   126 /* tilde */
#define FONT_CHARS  (FONT_LAST - FONT_FIRST + 1)

/* Each character is 16 bytes (one byte per row, 8 pixels wide).
   Bit 7 = leftmost pixel. */
extern const UINT8 font_data[FONT_CHARS][FONT_HEIGHT];

#endif /* FONT_H */
