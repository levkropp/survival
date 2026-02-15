/*
 * survival.h — Main API header for Survival Workstation programs
 *
 * Include this in your .c files to access framebuffer, keyboard,
 * memory, and filesystem functions.
 */
#ifndef SURVIVAL_H
#define SURVIVAL_H

/* ---- Types ---- */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
typedef unsigned long      size_t;
typedef long               ssize_t;

#define NULL ((void *)0)

/* ---- Colors (ARGB32) ---- */
#define COLOR_BLACK   0xFF000000
#define COLOR_WHITE   0xFFFFFFFF
#define COLOR_RED     0xFFFF0000
#define COLOR_GREEN   0xFF00FF00
#define COLOR_BLUE    0xFF0000FF
#define COLOR_YELLOW  0xFFFFFF00
#define COLOR_CYAN    0xFF00FFFF
#define COLOR_MAGENTA 0xFFFF00FF
#define COLOR_GRAY    0xFFAAAAAA
#define COLOR_DGRAY   0xFF555555
#define COLOR_ORANGE  0xFFFF8800

/* ---- Framebuffer ---- */
void fb_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);
void fb_char(uint32_t col, uint32_t row, char c, uint32_t fg, uint32_t bg);
void fb_string(uint32_t col, uint32_t row, const char *s, uint32_t fg, uint32_t bg);
void fb_scroll(int lines);
void fb_print(const char *s, uint32_t color);

/* ---- Keyboard ---- */
struct key_event {
    uint16_t code;
    uint16_t scancode;
};

int  kbd_poll(struct key_event *ev);
void kbd_wait(struct key_event *ev);

/* Key codes */
#define KEY_ESC   0x1B
#define KEY_ENTER 0x0D
#define KEY_BS    0x08
#define KEY_TAB   0x09
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_HOME  0x84
#define KEY_END   0x85
#define KEY_PGUP  0x86
#define KEY_PGDN  0x87
#define KEY_DEL   0x88

/* ---- Memory ---- */
void *mem_alloc(size_t size);
void  mem_free(void *ptr);
void  mem_set(void *dst, uint8_t val, size_t size);
void  mem_copy(void *dst, const void *src, size_t size);

/* ---- Libc-like ---- */
void *malloc(size_t size);
void  free(void *ptr);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
char *strcpy(char *dst, const char *src);
int   printf(const char *fmt, ...);
int   snprintf(char *buf, size_t size, const char *fmt, ...);
int   puts(const char *s);

#endif /* SURVIVAL_H */
