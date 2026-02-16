---
layout: default
title: "Chapter 6: Painting Pixels"
parent: "Phase 1: Boot & Input"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 6
---

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

This requires new fields in our boot state, plus we need to think about resolution. Modern laptops ship with screens that are 2560x1600 or higher. Drawing every pixel on a 2560x1600 framebuffer means pushing over 4 million pixels per screen redraw — with no GPU acceleration, just raw CPU writes to memory-mapped video RAM. Scrolling through a text file becomes painfully slow.

The fix is simple: we hard-limit resolution to 800x600. That's 480,000 pixels — about 8x fewer than a typical laptop display. At 800x600 with an 8x16 font, we get 100 columns by 37 rows of text. That's plenty for a survival workstation.

Let's expand `src/boot.h`:

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
    UINT32 scale;    /* font scale factor (1 or 2) */
};
```

Six new fields for the framebuffer itself, and five for text cursor tracking. Let's go through them:

- **`gop`** — Pointer to the GOP protocol interface. We'll use this to query the display mode.
- **`framebuffer`** — The framebuffer base address, cast to `UINT32 *` so each array element is one pixel.
- **`fb_width`, `fb_height`** — Screen dimensions in pixels.
- **`fb_pitch`** — Pixels per scan line (may differ from width due to padding).
- **`fb_size`** — Total framebuffer size in bytes.
- **`cursor_x`, `cursor_y`** — Current text cursor position in character coordinates (not pixels).
- **`cols`, `rows`** — How many characters fit on screen (computed from pixel dimensions and font size).
- **`scale`** — Font scale factor. Always 1 at 800x600. The field exists so the character renderer can support 2x scaling on higher resolutions if needed in the future.

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

    /* GOP method casts (protocol stores them as void *) */
    typedef EFI_STATUS (*GOP_QUERY)(EFI_GRAPHICS_OUTPUT_PROTOCOL *,
                                    UINT32, UINTN *,
                                    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    typedef EFI_STATUS (*GOP_SET)(EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32);
    GOP_QUERY query_mode = (GOP_QUERY)gop->QueryMode;
    GOP_SET   set_mode   = (GOP_SET)gop->SetMode;
```

The GOP protocol has `QueryMode` and `SetMode` methods, but our minimal EFI header declares them as `void *`. We cast them to proper function pointer types so we can call them. `QueryMode` takes a mode number and returns that mode's resolution info. `SetMode` switches the display to a given mode.

```c
    /* Hard-limit resolution to 800x600 to keep framebuffer draws fast */
    UINT32 max_w = 800;
    UINT32 max_h = 600;

    UINT32 best_mode = gop->Mode->Mode;
    UINT32 best_pixels = 0;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN info_size;
        if (EFI_ERROR(query_mode(gop, m, &info_size, &info)))
            continue;
        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;
        if (w <= max_w && h <= max_h && w * h > best_pixels) {
            best_pixels = w * h;
            best_mode = m;
        }
    }

    if (best_mode != gop->Mode->Mode)
        set_mode(gop, best_mode);
```

This is the mode selection loop. Every UEFI firmware advertises a list of supported display modes — `gop->Mode->MaxMode` tells us how many. We iterate through all of them, querying each one's resolution, and pick the largest mode that fits within 800x600. Most firmwares provide standard VGA-compatible modes (640x480, 800x600, 1024x768) alongside the panel's native resolution, so 800x600 is almost always available.

If the best mode we found is different from the current mode, we switch to it. After `SetMode`, the GOP protocol updates its `Mode` struct with the new framebuffer address, resolution, and pitch.

```c
    g_boot.framebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    g_boot.fb_width = gop->Mode->Info->HorizontalResolution;
    g_boot.fb_height = gop->Mode->Info->VerticalResolution;
    g_boot.fb_pitch = gop->Mode->Info->PixelsPerScanLine;
    g_boot.fb_size = gop->Mode->FrameBufferSize;
```

We extract the framebuffer information from the (now selected) display mode. `FrameBufferBase` is a 64-bit physical address — we cast it through `UINTN` (pointer-sized integer) to `UINT32 *` so that `g_boot.framebuffer[0]` is the top-left pixel.

```c
    if (g_boot.framebuffer == NULL || g_boot.fb_size == 0)
        return EFI_UNSUPPORTED;
```

A critical check. Some UEFI implementations provide the GOP protocol but have no linear framebuffer — they use GPU command queues instead. In that case, `FrameBufferBase` is 0. We detect this and return an error, which will trigger a console-only fallback in `main.c`.

```c
    g_boot.scale = 1;

    g_boot.cols = g_boot.fb_width / (FONT_WIDTH * g_boot.scale);
    g_boot.rows = g_boot.fb_height / (FONT_HEIGHT * g_boot.scale);
    g_boot.cursor_x = 0;
    g_boot.cursor_y = 0;

    fb_clear(COLOR_BLACK);
    return EFI_SUCCESS;
}
```

We set the font scale to 1 (no scaling at 800x600), compute how many text characters fit on screen (100 columns by 37 rows), initialize the cursor to the top-left, and clear the screen to black.

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

    UINT32 scale = g_boot.scale;
    UINT32 cw = FONT_WIDTH * scale;
    UINT32 ch = FONT_HEIGHT * scale;
    UINT32 px = cx * cw;
    UINT32 py = cy * ch;

    if (px + cw > g_boot.fb_width || py + ch > g_boot.fb_height)
        return;

    const UINT8 *glyph = font_data[c - FONT_FIRST];
    UINT32 pitch = g_boot.fb_pitch;
    UINT32 *base = g_boot.framebuffer + py * pitch + px;

    if (scale == 1) {
        for (UINT32 row = 0; row < FONT_HEIGHT; row++) {
            UINT8 bits = glyph[row];
            UINT32 *dst = base;
            for (UINT32 col = 0; col < FONT_WIDTH; col++)
                *dst++ = (bits & (0x80 >> col)) ? fg : bg;
            base += pitch;
        }
    } else {
        /* 2x: write 2 pixels wide, 2 rows at once per font row */
        for (UINT32 row = 0; row < FONT_HEIGHT; row++) {
            UINT8 bits = glyph[row];
            UINT32 *dst = base;
            for (UINT32 col = 0; col < FONT_WIDTH; col++) {
                UINT32 color = (bits & (0x80 >> col)) ? fg : bg;
                dst[0] = color;
                dst[1] = color;
                dst[pitch] = color;
                dst[pitch + 1] = color;
                dst += 2;
            }
            base += pitch * 2;
        }
    }
}
```

Let's trace through this.

`cx` and `cy` are **character coordinates**, not pixel coordinates. Character (0,0) is the top-left. At scale 1, character (5,3) starts at pixel (40, 48) because each character is 8 pixels wide and 16 tall.

If the character is outside our font's range, we substitute `'?'` to prevent an out-of-bounds array access. The bounds check on `px + cw` and `py + ch` prevents writing past the framebuffer edge.

We compute `base` — a direct pointer into the framebuffer at the character's top-left pixel. This avoids calling `fb_pixel` 128 times per character. Instead, we write directly to the framebuffer through pointer arithmetic, computing the row pointer once per font row.

The scale 1 path is the normal case at 800x600: for each of the 16 font rows, we read one byte from the glyph and write 8 pixels. The bit test works like this:

```
0x80 = 1000 0000    (bit 7 — leftmost pixel)
0x80 >> 0 = 1000 0000  → tests column 0 (leftmost)
0x80 >> 1 = 0100 0000  → tests column 1
0x80 >> 2 = 0010 0000  → tests column 2
...
0x80 >> 7 = 0000 0001  → tests column 7 (rightmost)
```

If the bit is set, we draw the foreground color. If clear, the background color. Drawing the background is important — it ensures each character's rectangle is fully painted, covering anything that was there before.

The scale 2 path doubles each pixel in both dimensions — each font pixel becomes a 2x2 block, and each font row spans two scanlines. This is useful if you ever run at a higher resolution where 8x16 characters would be too small to read. At 800x600, we always use scale 1.

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
    UINT32 scroll_rows = FONT_HEIGHT * g_boot.scale;

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

The scroll distance is `FONT_HEIGHT * scale` — one text row in pixels. The first loop copies each pixel row upward by that distance. The second loop clears the vacated rows at the bottom to black.

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
4. scale=1, cw=8, ch=16, px = 5*8 = 40, py = 3*16 = 48
5. glyph = font_data['H' - 32] = font_data[40]
6. base = framebuffer + 48*pitch + 40
7. For each of 16 rows:
     Read one byte from the glyph
     Write 8 pixels directly via pointer: fg or bg
     Advance base by pitch
8. cursor_x becomes 6
```

128 pixels are written via direct pointer writes to the framebuffer — no function call overhead per pixel. The display hardware continuously reads the framebuffer and sends pixel data to the monitor. The 'H' appears on screen.

## What We Have

Five files now (plus font data):

```
src/boot.h   — Global state with framebuffer fields, color constants
src/mem.h/c  — Memory allocation and utilities
src/font.h/c — 8x16 bitmap font (95 characters, 1520 bytes)
src/fb.h/c   — Framebuffer driver: 8 functions, 185 lines
src/main.c   — Entry point (still using console loop)
```

We can draw pixels, rectangles, characters, and strings. We can scroll the screen and maintain a text cursor. The entire font for all printable ASCII is under 2 KB, and the framebuffer driver is 185 lines.

But we can't use any of this yet — `main.c` still runs the console loop from Chapter 4. We need keyboard input to make it interactive, and then we need to wire everything together in the main loop.

Next: hearing from the keyboard.

---

**Next:** [Chapter 7: Hearing Keystrokes](chapter-07-hearing-keystrokes)
