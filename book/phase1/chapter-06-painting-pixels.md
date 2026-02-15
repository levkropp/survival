# Chapter 6: Painting Pixels

## Escaping the Console

At the end of Chapter 5, we have memory management but nothing to show for it. Our application still uses UEFI's built-in text console, which gives us white text on a black background — and that's about it. No colors, no custom fonts, no control over layout.

To build a real workstation UI, we need to draw our own pixels. That means we need a framebuffer.

## What Is a Framebuffer?

A framebuffer is a region of memory where each location corresponds to a pixel on screen. Write a value to a framebuffer address, and the display hardware turns that into a colored dot on your monitor. It's the most direct connection between software and a visual display.

```
Framebuffer memory (row-major order):
┌──────┬──────┬──────┬──────┬──────┬───
│ 0,0  │ 1,0  │ 2,0  │ 3,0  │ 4,0  │...
├──────┼──────┼──────┼──────┼──────┼───
│ 0,1  │ 1,1  │ 2,1  │ 3,1  │ 4,1  │...
├──────┼──────┼──────┼──────┼──────┼───
│ 0,2  │ 1,2  │ 2,2  │ 3,2  │ 4,2  │...
└──────┴──────┴──────┴──────┴──────┴───

Each cell is 4 bytes (32 bits):
  Byte 0: Blue  (0-255)
  Byte 1: Green (0-255)
  Byte 2: Red   (0-255)
  Byte 3: Reserved (usually 0)
```

The pixel format is BGRX — blue first, then green, then red, then a reserved byte. This is backwards from what you might expect (most color pickers show RGB), but it's what the hardware uses. We'll define color constants to hide this detail.

To find any pixel in memory:

```c
framebuffer[y * pitch + x] = color;
```

Where **pitch** is the number of pixels per row, including any padding. Some display hardware requires rows to be aligned to certain boundaries (64 bytes, 256 bytes), so the pitch might be slightly larger than the visible width. Always use pitch for address calculations, never width.

## Getting a Framebuffer From UEFI

UEFI provides graphics through the **Graphics Output Protocol (GOP)**. To use it, we need to:

1. Ask the firmware if a GOP driver exists
2. Get a pointer to the framebuffer from the GOP
3. Store the framebuffer address and dimensions

This requires new fields in our boot state. Let's expand `src/boot.h`:

```c
struct boot_state {
    EFI_HANDLE image_handle;
    EFI_SYSTEM_TABLE *st;
    EFI_BOOT_SERVICES *bs;
    EFI_RUNTIME_SERVICES *rs;

    /* Graphics */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    UINT32 *framebuffer;
    UINT32 fb_width;
    UINT32 fb_height;
    UINT32 fb_pitch; /* pixels per scan line */
    UINTN fb_size;

    /* Text cursor state */
    UINT32 cursor_x; /* column in characters */
    UINT32 cursor_y; /* row in characters */
    UINT32 cols;     /* max columns */
    UINT32 rows;     /* max rows */
};
```

Six new fields for the framebuffer itself, and four for text cursor tracking. Let's go through them:

- **`gop`** — Pointer to the GOP protocol interface. We'll use this to query the display mode.
- **`framebuffer`** — The framebuffer base address, cast to `UINT32 *` so each array element is one pixel.
- **`fb_width`, `fb_height`** — Screen dimensions in pixels.
- **`fb_pitch`** — Pixels per scan line (may differ from width due to padding).
- **`fb_size`** — Total framebuffer size in bytes.
- **`cursor_x`, `cursor_y`** — Current text cursor position in character coordinates (not pixels).
- **`cols`, `rows`** — How many characters fit on screen (computed from pixel dimensions and font size).

We also need color constants. Since the pixel format is BGRX, the byte order is blue-green-red-reserved. Add these to `boot.h`, after the struct:

```c
/* Color constants (BGRX 32-bit) */
#define COLOR_BLACK   0x00000000
#define COLOR_WHITE   0x00FFFFFF
#define COLOR_GREEN   0x0000FF00
#define COLOR_RED     0x00FF0000
#define COLOR_BLUE    0x000000FF
#define COLOR_YELLOW  0x0000FFFF
#define COLOR_CYAN    0x00FFFF00
#define COLOR_GRAY    0x00808080
#define COLOR_DGRAY   0x00404040
```

Yellow is green + red. Cyan is green + blue. These look odd if you're thinking in RGB, but in BGRX format, `0x0000FFFF` has full green (byte 1 = `0xFF`) and full red (byte 2 = `0xFF`), which makes yellow.

## The Font

Before we write any framebuffer code, we need a font. Without it, we can draw colored rectangles but not text — and a workstation without text is useless.

We use a **bitmap font** — the simplest possible font format. Each character is an 8-pixel-wide by 16-pixel-tall grid where each pixel is either "on" (foreground) or "off" (background). Each row is stored as one byte, with bit 7 being the leftmost pixel:

```
Character 'A' (8x16 bitmap):

Row  0: 00000000  ........
Row  1: 00000000  ........
Row  2: 00010000  ...*....
Row  3: 00111000  ..***...
Row  4: 01101100  .***.**.
Row  5: 11000110  **...**.
Row  6: 11000110  **...**.
Row  7: 11111110  *******.
Row  8: 11000110  **...**.
Row  9: 11000110  **...**.
Row 10: 11000110  **...**.
Row 11: 11000110  **...**.
Row 12: 00000000  ........
Row 13: 00000000  ........
Row 14: 00000000  ........
Row 15: 00000000  ........
```

The entire 'A' is 16 bytes: `{0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00}`.

Create `src/font.h`:

```c
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
```

We cover ASCII 32 (space) through 126 (tilde) — all 95 printable ASCII characters. At 16 bytes per character, the entire font is **1,520 bytes**. Not even 2 KB.

The actual font data lives in `src/font.c` — a large array of bitmap data based on the classic IBM VGA 8x16 font (public domain). `font_data[0]` is space, `font_data[1]` is '!', and so on. To get the glyph for any character `c`, you access `font_data[c - FONT_FIRST]`.

The font data file is about 280 lines of hex arrays. There's no logic in it — just data. We won't reproduce it all here, but it's in the repository.

## The First Pixel

Now let's build the framebuffer module. Create `src/fb.c`:

```c
#include "fb.h"
#include "font.h"
#include "mem.h"
```

And `src/fb.h`:

```c
#ifndef FB_H
#define FB_H

#include "boot.h"

EFI_STATUS fb_init(void);

#endif /* FB_H */
```

We start with just `fb_init`. Let's write it:

```c
EFI_STATUS fb_init(void) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status;

    status = g_boot.bs->LocateProtocol(&gop_guid, NULL, (void **)&g_boot.gop);
    if (EFI_ERROR(status))
        return status;
```

Every UEFI protocol is identified by a 128-bit GUID. `LocateProtocol` searches the firmware for a driver implementing the requested protocol — we're asking "does this system have a graphics driver?" If yes, `g_boot.gop` gets filled with a pointer to the protocol interface.

```c
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = g_boot.gop;

    g_boot.framebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    g_boot.fb_width = gop->Mode->Info->HorizontalResolution;
    g_boot.fb_height = gop->Mode->Info->VerticalResolution;
    g_boot.fb_pitch = gop->Mode->Info->PixelsPerScanLine;
    g_boot.fb_size = gop->Mode->FrameBufferSize;
```

We extract the framebuffer information from the current display mode. `FrameBufferBase` is a 64-bit physical address — we cast it through `UINTN` (pointer-sized integer) to `UINT32 *` so that `g_boot.framebuffer[0]` is the top-left pixel.

```c
    if (g_boot.framebuffer == NULL || g_boot.fb_size == 0)
        return EFI_UNSUPPORTED;
```

A critical check. Some UEFI implementations provide the GOP protocol but have no linear framebuffer — they use GPU command queues instead. In that case, `FrameBufferBase` is 0. We detect this and return an error, which will trigger a console-only fallback in `main.c`.

```c
    g_boot.cols = g_boot.fb_width / FONT_WIDTH;
    g_boot.rows = g_boot.fb_height / FONT_HEIGHT;
    g_boot.cursor_x = 0;
    g_boot.cursor_y = 0;

    fb_clear(COLOR_BLACK);
    return EFI_SUCCESS;
}
```

We compute how many text characters fit on screen (with an 800x600 display, that's 100 columns by 37 rows), initialize the cursor to the top-left, and clear the screen to black.

But wait — we called `fb_clear` before we've written it. Let's build up the drawing functions.

## Drawing a Single Pixel

The most fundamental operation:

```c
void fb_pixel(UINT32 x, UINT32 y, UINT32 color) {
    if (x < g_boot.fb_width && y < g_boot.fb_height)
        g_boot.framebuffer[y * g_boot.fb_pitch + x] = color;
}
```

One bounds check and one memory write. The bounds check prevents writing outside the framebuffer — which could corrupt memory or crash the system.

The formula `y * pitch + x` is the core of all 2D framebuffer graphics:

```
Row 0:  [0]  [1]  [2]  ... [pitch-1]
Row 1:  [pitch]  [pitch+1]  [pitch+2]  ...
Row 2:  [2*pitch]  [2*pitch+1]  ...
...
Pixel (x,y) = [y * pitch + x]
```

## Clearing the Screen

```c
void fb_clear(UINT32 color) {
    for (UINT32 y = 0; y < g_boot.fb_height; y++) {
        UINT32 *line = &g_boot.framebuffer[y * g_boot.fb_pitch];
        for (UINT32 x = 0; x < g_boot.fb_width; x++)
            line[x] = color;
    }
}
```

Fill every pixel with the given color. We compute the line pointer once per row to avoid recomputing `y * pitch` for every pixel. On an 800x600 display, this writes 480,000 pixels (about 1.8 MB). Even with our simple loop, it completes in milliseconds on the Cortex-A53.

## Drawing Rectangles

```c
void fb_rect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color) {
    for (UINT32 row = y; row < y + h && row < g_boot.fb_height; row++) {
        UINT32 *line = &g_boot.framebuffer[row * g_boot.fb_pitch + x];
        for (UINT32 col = 0; col < w && (x + col) < g_boot.fb_width; col++)
            line[col] = color;
    }
}
```

Same pattern as `fb_clear`, but restricted to a rectangular region. The bounds checks (`row < fb_height`, `x + col < fb_width`) ensure we never draw outside the screen. This is called **clipping**.

## Rendering a Character

Now the interesting part — turning font bitmaps into visible characters:

```c
void fb_char(UINT32 cx, UINT32 cy, char c, UINT32 fg, UINT32 bg) {
    if (c < FONT_FIRST || c > FONT_LAST)
        c = '?';

    UINT32 px = cx * FONT_WIDTH;
    UINT32 py = cy * FONT_HEIGHT;
    const UINT8 *glyph = font_data[c - FONT_FIRST];

    for (UINT32 row = 0; row < FONT_HEIGHT; row++) {
        UINT8 bits = glyph[row];
        for (UINT32 col = 0; col < FONT_WIDTH; col++) {
            UINT32 color = (bits & (0x80 >> col)) ? fg : bg;
            fb_pixel(px + col, py + row, color);
        }
    }
}
```

Let's trace through this.

`cx` and `cy` are **character coordinates**, not pixel coordinates. Character (0,0) is the top-left. Character (5,3) starts at pixel (40, 48) because each character is 8 pixels wide and 16 tall.

If the character is outside our font's range, we substitute `'?'` to prevent an out-of-bounds array access.

We look up the 16-byte glyph bitmap, then iterate over all 128 pixels (8 wide x 16 tall). For each pixel, we test whether the corresponding bit is set:

```
0x80 = 1000 0000    (bit 7 — leftmost pixel)
0x80 >> 0 = 1000 0000  → tests column 0 (leftmost)
0x80 >> 1 = 0100 0000  → tests column 1
0x80 >> 2 = 0010 0000  → tests column 2
...
0x80 >> 7 = 0000 0001  → tests column 7 (rightmost)
```

If the bit is set, we draw the foreground color. If clear, the background color. Drawing the background is important — it ensures each character's 8x16 rectangle is fully painted, covering anything that was there before.

## Drawing Strings

A single character is useful, but we need to render strings:

```c
void fb_string(UINT32 cx, UINT32 cy, const char *s, UINT32 fg, UINT32 bg) {
    while (*s) {
        if (cx >= g_boot.cols) {
            cx = 0;
            cy++;
        }
        if (cy >= g_boot.rows)
            return;
        fb_char(cx, cy, *s, fg, bg);
        cx++;
        s++;
    }
}
```

Advance `cx` after each character. If we hit the right edge, wrap to the next line. If we fall off the bottom, stop. This function is used by the file browser (Chapter 10) to draw individual lines at specific positions with specific colors.

## Scrolling

When text fills the screen, we need to move everything up by one line:

```c
void fb_scroll(void) {
    UINT32 scroll_rows = FONT_HEIGHT;

    for (UINT32 y = 0; y < g_boot.fb_height - scroll_rows; y++) {
        mem_copy(&g_boot.framebuffer[y * g_boot.fb_pitch],
                 &g_boot.framebuffer[(y + scroll_rows) * g_boot.fb_pitch],
                 g_boot.fb_width * sizeof(UINT32));
    }

    for (UINT32 y = g_boot.fb_height - scroll_rows; y < g_boot.fb_height; y++) {
        UINT32 *line = &g_boot.framebuffer[y * g_boot.fb_pitch];
        for (UINT32 x = 0; x < g_boot.fb_width; x++)
            line[x] = COLOR_BLACK;
    }
}
```

The first loop copies each pixel row upward by 16 pixels (one text line). Row 16 becomes row 0, row 17 becomes row 1, and so on. The second loop clears the bottom 16 rows to black.

This is where `mem_copy` from Chapter 5 earns its keep. And remember the overlap concern? The destination (row y) is always before the source (row y + 16) in memory, so our forward-copying `mem_copy` works correctly here.

## The Print Workhorse

`fb_print` is our main text output function — it manages cursor position, line wrapping, and auto-scrolling:

```c
void fb_print(const char *s, UINT32 fg) {
    while (*s) {
        if (*s == '\n') {
            g_boot.cursor_x = 0;
            g_boot.cursor_y++;
        } else {
            if (g_boot.cursor_x >= g_boot.cols) {
                g_boot.cursor_x = 0;
                g_boot.cursor_y++;
            }
            if (g_boot.cursor_y >= g_boot.rows) {
                fb_scroll();
                g_boot.cursor_y = g_boot.rows - 1;
            }
            fb_char(g_boot.cursor_x, g_boot.cursor_y, *s, fg, COLOR_BLACK);
            g_boot.cursor_x++;
        }

        if (g_boot.cursor_y >= g_boot.rows) {
            fb_scroll();
            g_boot.cursor_y = g_boot.rows - 1;
        }

        s++;
    }
}
```

For newlines (`\n`), we move to column 0 of the next row. For normal characters, we first check if we need to wrap or scroll, draw the character, and advance the cursor. The scroll check appears twice — once before drawing (in case we wrapped) and once after advancing (in case a newline pushed us past the bottom).

The background is always black. The foreground color is whatever the caller passes — green for headings, white for content, yellow for prompts, gray for system info.

## The Complete fb.h

Now that we've built all the functions, here's the complete header:

```c
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
```

Eight functions. Three layers of abstraction: pixels (`fb_pixel`, `fb_rect`, `fb_clear`), characters (`fb_char`, `fb_string`), and the cursor-aware print function (`fb_print`, `fb_scroll`).

## What Happens When We Print 'H' in Green

Let's trace the full path from a `fb_print` call to pixels on screen:

```
1. fb_print("H", COLOR_GREEN) is called
2. cursor_x is 5, cursor_y is 3
3. fb_char(5, 3, 'H', 0x0000FF00, 0x00000000) is called
4. px = 5 * 8 = 40, py = 3 * 16 = 48
5. glyph = font_data['H' - 32] = font_data[40]
6. For each of 16 rows:
     Read one byte from the glyph
     For each of 8 bits:
       If bit is set: write 0x0000FF00 (green) at (40+col, 48+row)
       If bit is clear: write 0x00000000 (black) at (40+col, 48+row)
7. cursor_x becomes 6
```

128 pixels are written. Each pixel is one memory write to the framebuffer. The display hardware continuously reads the framebuffer and sends pixel data to the monitor over HDMI. The 'H' appears on screen.

## What We Have

Five files now (plus font data):

```
src/boot.h   — Global state with framebuffer fields, color constants
src/mem.h/c  — Memory allocation and utilities
src/font.h/c — 8x16 bitmap font (95 characters, 1520 bytes)
src/fb.h/c   — Framebuffer driver: 8 functions, 127 lines
src/main.c   — Entry point (still using console loop)
```

We can draw pixels, rectangles, characters, and strings. We can scroll the screen and maintain a text cursor. The entire font for all printable ASCII is under 2 KB, and the framebuffer driver is 127 lines.

But we can't use any of this yet — `main.c` still runs the console loop from Chapter 4. We need keyboard input to make it interactive, and then we need to wire everything together in the main loop.

Next: hearing from the keyboard.
