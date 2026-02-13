#ifndef FB_H
#define FB_H

#include "boot.h"

/* Initialize framebuffer via UEFI GOP */
EFI_STATUS fb_init(void);

/* Pixel-level operations */
void fb_pixel(UINT32 x, UINT32 y, UINT32 color);
void fb_rect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color);
void fb_clear(UINT32 color);

/* Text rendering (uses bitmap font) */
void fb_char(UINT32 cx, UINT32 cy, char c, UINT32 fg, UINT32 bg);
void fb_string(UINT32 cx, UINT32 cy, const char *s, UINT32 fg, UINT32 bg);

/* Scroll the screen up by one text row */
void fb_scroll(void);

/* Print a string at the current cursor position, advancing cursor.
   Handles \n for newline. Scrolls when reaching bottom. */
void fb_print(const char *s, UINT32 fg);

#endif /* FB_H */
