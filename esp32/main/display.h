/*
 * display.h — ILI9341 320x240 display driver for ESP32 CYD
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240

/* Initialize the ILI9341 on SPI2 and turn on the backlight. */
void display_init(void);

/* Fill the entire screen with a color (RGB565). */
void display_clear(uint16_t color);

/* Fill a rectangle. Coordinates are clipped to screen bounds. */
void display_fill_rect(int x, int y, int w, int h, uint16_t color);

/* Draw a single character at (x, y) using the 8x16 VGA font. */
void display_char(int x, int y, char c, uint16_t fg, uint16_t bg);

/* Draw a null-terminated string. Wraps at screen edge. */
void display_string(int x, int y, const char *s, uint16_t fg, uint16_t bg);

/* Draw a 1-bit-per-pixel bitmap. Bit 7 of each byte = leftmost pixel.
 * Each row is (w+7)/8 bytes. Set bits draw fg, clear bits draw bg. */
void display_draw_bitmap1bpp(int x, int y, int w, int h,
                              const uint8_t *bitmap, uint16_t fg, uint16_t bg);

/* RGB888 to RGB565 conversion. */
static inline uint16_t display_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/* Common colors */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_GRAY    0x7BEF
#define COLOR_DGRAY   0x3186

#endif /* DISPLAY_H */
