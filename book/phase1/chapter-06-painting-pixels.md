# Chapter 6: Painting Pixels

## What is a Framebuffer?

A framebuffer is a region of memory where each location corresponds to a pixel on screen. Change a value in the framebuffer, and the corresponding pixel changes color on the monitor. It's the most direct possible connection between software and a visual display.

Imagine a grid of numbers, one per pixel:

```
Framebuffer memory:
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

If the screen is 1024 pixels wide and 768 pixels tall, the framebuffer contains 1024 x 768 = 786,432 pixels, each 4 bytes, totaling about 3 MB of memory.

The pixels are stored in row-major order: all pixels of row 0 first, then row 1, then row 2, and so on. To find the memory address of pixel (x, y):

```
address = framebuffer_base + (y * pitch + x) * 4
```

Or, since our framebuffer pointer is already a `UINT32 *` (each element is 4 bytes):

```c
framebuffer[y * pitch + x] = color;
```

### What's "pitch"?

The **pitch** (sometimes called **stride**) is the number of pixels per horizontal line of the framebuffer, including any padding. Why would there be padding?

Some display hardware requires that each row start at a memory address aligned to a certain boundary (like 64 bytes, or 256 bytes). If the screen is 1024 pixels wide and each pixel is 4 bytes, a row is 4096 bytes — which happens to be nicely aligned. But if the screen were 1023 pixels wide, a row would be 4092 bytes. The hardware might pad each row to 4096 bytes (1024 pixels), making the pitch 1024 even though the visible width is 1023.

Always use pitch, never width, for address calculations. If they happen to be equal (as they often are), no harm done. If they differ and you use width, you'll get a diagonally distorted image.

## Initializing the Graphics

Let's walk through `fb_init()` in `src/fb.c`:

```c
EFI_STATUS fb_init(void) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status;
```

Every UEFI protocol is identified by a GUID — a 128-bit globally unique identifier. `EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID` is defined in the gnu-efi headers as `{0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}`. We store it in a local variable because `LocateProtocol` needs its address.

```c
    status = g_boot.bs->LocateProtocol(&gop_guid, NULL, (void **)&g_boot.gop);
    if (EFI_ERROR(status))
        return status;
```

`LocateProtocol` searches the firmware for a driver that implements the requested protocol. It's like asking: "Is there anything in this system that knows how to do graphics?" If a graphics driver exists (which it will on any system with a display), the firmware returns a pointer to the protocol interface.

The second parameter (`NULL`) is a registration key for notifications — we don't use it.

The third parameter is a double pointer. `LocateProtocol` writes the protocol pointer through it. After this call, `g_boot.gop` points to a structure full of function pointers:

```c
typedef struct {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE  QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE    SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT         Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE        *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;
```

- `QueryMode` — Ask about a specific display mode (resolution, pixel format)
- `SetMode` — Switch to a different display mode
- `Blt` — Block transfer (hardware-accelerated copy/fill, but we won't use it)
- `Mode` — Pointer to current mode information

```c
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = g_boot.gop;

    g_boot.framebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    g_boot.fb_width = gop->Mode->Info->HorizontalResolution;
    g_boot.fb_height = gop->Mode->Info->VerticalResolution;
    g_boot.fb_pitch = gop->Mode->Info->PixelsPerScanLine;
    g_boot.fb_size = gop->Mode->FrameBufferSize;
```

We extract the framebuffer information from the current mode. Let's unpack the types:

**`FrameBufferBase`** is an `EFI_PHYSICAL_ADDRESS` (a 64-bit integer representing a physical memory address). We cast it to `UINTN` (native pointer size integer) and then to `UINT32 *` (a pointer to 32-bit values). After this, `g_boot.framebuffer[0]` is the top-left pixel.

**`HorizontalResolution`** and **`VerticalResolution`** are the screen dimensions in pixels.

**`PixelsPerScanLine`** is the pitch — pixels per row including padding.

**`FrameBufferSize`** is the total framebuffer size in bytes.

```c
    if (g_boot.framebuffer == NULL || g_boot.fb_size == 0)
        return EFI_UNSUPPORTED;
```

A critical check. Some UEFI implementations (notably QEMU's `virtio-gpu-pci`) provide the GOP protocol but don't have a linear framebuffer. They use GPU command queues instead. In that case, `FrameBufferBase` is 0 (NULL). We detect this and return an error, which triggers the console fallback in `main.c`.

On real hardware with HDMI output and on QEMU with the `ramfb` device, this check passes and we have a valid framebuffer.

```c
    g_boot.cols = g_boot.fb_width / FONT_WIDTH;
    g_boot.rows = g_boot.fb_height / FONT_HEIGHT;
    g_boot.cursor_x = 0;
    g_boot.cursor_y = 0;
```

We calculate how many characters fit on screen. With an 8x16 font on a 1024x768 display, that's 128 columns by 48 rows — plenty for a text terminal.

```c
    fb_clear(COLOR_BLACK);
    return EFI_SUCCESS;
}
```

Clear the screen to black and report success. On a real display, you'll see the screen go from the UEFI boot logo to solid black at this moment.

## Drawing Pixels

The most fundamental operation — set a single pixel:

```c
void fb_pixel(UINT32 x, UINT32 y, UINT32 color) {
    if (x < g_boot.fb_width && y < g_boot.fb_height)
        g_boot.framebuffer[y * g_boot.fb_pitch + x] = color;
}
```

That's it. One bounds check and one memory write. The bounds check prevents writing outside the framebuffer, which could corrupt memory or crash.

The address formula `y * pitch + x` is the core of all 2D framebuffer graphics:

```
Row 0:  [0]  [1]  [2]  ... [pitch-1]
Row 1:  [pitch]  [pitch+1]  [pitch+2]  ...
Row 2:  [2*pitch]  [2*pitch+1]  ...
...
Pixel (x,y) = [y * pitch + x]
```

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

We fill a rectangular region pixel by pixel. For each row, we calculate a pointer to the start of that row's section within the rectangle, then fill pixels left to right.

The bounds checks (`row < fb_height`, `x + col < fb_width`) ensure we don't draw outside the screen. This is called **clipping** — constraining drawing operations to the visible area.

**Optimization note:** We compute `line` (the starting address for each row) once, then index from it. This avoids recomputing `row * fb_pitch + x` for every pixel. The compiler would likely optimize this anyway, but it makes the intent clearer.

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

This is just `fb_rect(0, 0, fb_width, fb_height, color)` — but writing it as a separate function makes the intent obvious. On a 1024x768 display, this writes 786,432 pixels (about 3 MB). Even with our simple loop, it completes in milliseconds on the Cortex-A53.

## The Bitmap Font

Now comes the most interesting part: rendering text. We need a **font** — a mapping from characters to visual shapes.

We use a **bitmap font**, the simplest possible font format. Each character is represented as a grid of pixels: some "on" (foreground color) and some "off" (background color). Our font is 8 pixels wide by 16 pixels tall:

```
Character 'A' (8x16 bitmap):

Row  0: 00000000  ........
Row  1: 00000000  ........
Row  2: 00010000  ...*....
Row  3: 00111000  ..***...
Row  4: 01101100  .***.**.
Row  5: 11000110  **...**.
Row  6: 11000110  **..,**.
Row  7: 11111110  *******.
Row  8: 11000110  **..,**.
Row  9: 11000110  **..,**.
Row 10: 11000110  **..,**.
Row 11: 11000110  **..,**.
Row 12: 00000000  ........
Row 13: 00000000  ........
Row 14: 00000000  ........
Row 15: 00000000  ........
```

Each row is stored as a single byte. Bit 7 (the leftmost, most significant bit) is the leftmost pixel. A 1 bit means "draw foreground color" and a 0 bit means "draw background color."

So the entire 'A' character is 16 bytes:

```c
{0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE,
 0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00}
```

Let's verify row 3 (`0x38`):
```
0x38 = 0011 1000 in binary
        ..***...
```

That's the three horizontal pixels forming the top of the A's triangle. Checks out.

### Font Data Structure

The font definition in `src/font.h`:

```c
#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#define FONT_FIRST  32   // space
#define FONT_LAST   126  // tilde
#define FONT_CHARS  (FONT_LAST - FONT_FIRST + 1)  // = 95 characters

extern const UINT8 font_data[FONT_CHARS][FONT_HEIGHT];
```

We cover ASCII 32 (space) through 126 (tilde) — all the printable ASCII characters. That's 95 characters, each 16 bytes, totaling **1,520 bytes**. Not even 2 KB for a complete text font.

The font data in `src/font.c` is a 2D array. `font_data[0]` is space (character 32), `font_data[1]` is '!' (character 33), and so on. To get the glyph for character `c`, you access `font_data[c - FONT_FIRST]`.

The font is based on the classic IBM VGA 8x16 font, which is in the public domain. It's the same font you'd see on a 1990s DOS screen. Readable, compact, and distinctive.

## Rendering Characters

```c
void fb_char(UINT32 cx, UINT32 cy, char c, UINT32 fg, UINT32 bg) {
```

`cx` and `cy` are **character coordinates**, not pixel coordinates. Character (0,0) is the top-left of the screen. Character (1,0) is 8 pixels to the right. Character (0,1) is 16 pixels down.

```c
    if (c < FONT_FIRST || c > FONT_LAST)
        c = '?';
```

If the character is outside our font's range (control characters, or anything above tilde), we substitute a question mark. This prevents out-of-bounds array access.

```c
    UINT32 px = cx * FONT_WIDTH;
    UINT32 py = cy * FONT_HEIGHT;
```

Convert from character coordinates to pixel coordinates. Character column 5 starts at pixel x = 40 (5 * 8). Character row 3 starts at pixel y = 48 (3 * 16).

```c
    const UINT8 *glyph = font_data[c - FONT_FIRST];
```

Get the 16-byte bitmap for this character. For 'A' (ASCII 65), `c - FONT_FIRST = 65 - 32 = 33`, so we get `font_data[33]`.

```c
    for (UINT32 row = 0; row < FONT_HEIGHT; row++) {
        UINT8 bits = glyph[row];
        for (UINT32 col = 0; col < FONT_WIDTH; col++) {
            UINT32 color = (bits & (0x80 >> col)) ? fg : bg;
            fb_pixel(px + col, py + row, color);
        }
    }
}
```

The nested loop iterates over every pixel in the 8x16 glyph.

The bit extraction `bits & (0x80 >> col)` deserves explanation:

```
0x80 = 1000 0000    (bit 7 set — leftmost pixel)
0x80 >> 0 = 1000 0000  → tests bit 7 (column 0, leftmost)
0x80 >> 1 = 0100 0000  → tests bit 6 (column 1)
0x80 >> 2 = 0010 0000  → tests bit 5 (column 2)
...
0x80 >> 7 = 0000 0001  → tests bit 0 (column 7, rightmost)
```

For each column position, we shift a single-bit mask to the right and AND it with the glyph byte. If the result is nonzero, that pixel is "on" (foreground color). If zero, it's "off" (background color).

This is why bit 7 is the leftmost pixel — we test from the most significant bit to the least significant bit as we go left to right across the character.

## Drawing Strings

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

Draws a string starting at character position (cx, cy). We advance `cx` after each character. If we hit the right edge, we wrap to the next line. If we fall off the bottom, we stop (rather than writing to invalid memory).

## Scrolling

When the cursor reaches the bottom of the screen, we need to scroll — move everything up by one text line:

```c
void fb_scroll(void) {
    UINT32 scroll_rows = FONT_HEIGHT;  // 16 pixels
```

We scroll by exactly one font row (16 pixels).

```c
    for (UINT32 y = 0; y < g_boot.fb_height - scroll_rows; y++) {
        mem_copy(&g_boot.framebuffer[y * g_boot.fb_pitch],
                 &g_boot.framebuffer[(y + scroll_rows) * g_boot.fb_pitch],
                 g_boot.fb_width * sizeof(UINT32));
    }
```

This copies each pixel row upward by `scroll_rows` pixels. Row 16 becomes row 0. Row 17 becomes row 1. And so on.

Visually:

```
Before scroll:          After scroll:
┌──────────────┐       ┌──────────────┐
│ Line 1       │       │ Line 2       │  ← was line 2
│ Line 2       │  ───→ │ Line 3       │  ← was line 3
│ Line 3       │       │ ...          │
│ ...          │       │ Last line    │
│ Last line    │       │ [blank]      │  ← cleared
└──────────────┘       └──────────────┘
```

The copy goes forward (from low addresses to high), and since the destination (row y) is always before the source (row y + scroll_rows) in memory, our simple forward `mem_copy` works correctly — we never overwrite data we haven't yet read.

```c
    for (UINT32 y = g_boot.fb_height - scroll_rows; y < g_boot.fb_height; y++) {
        UINT32 *line = &g_boot.framebuffer[y * g_boot.fb_pitch];
        for (UINT32 x = 0; x < g_boot.fb_width; x++)
            line[x] = COLOR_BLACK;
    }
}
```

After copying, the bottom `scroll_rows` pixel rows contain stale data (a copy of the old last line). We clear them to black.

## The Print Function

`fb_print` is our workhorse — the framebuffer equivalent of `printf` (without format strings):

```c
void fb_print(const char *s, UINT32 fg) {
    while (*s) {
```

Iterate through each character of the null-terminated string.

```c
        if (*s == '\n') {
            g_boot.cursor_x = 0;
            g_boot.cursor_y++;
```

Newline: move to column 0 of the next row. Note we only handle `\n` (not `\r\n`). Our strings use Unix-style newlines. The UEFI console needed `\r\n`, but our framebuffer text handles them separately.

```c
        } else {
            if (g_boot.cursor_x >= g_boot.cols) {
                g_boot.cursor_x = 0;
                g_boot.cursor_y++;
            }
```

If we've reached the right edge, wrap to the next line. This happens automatically — the caller doesn't need to insert newlines for wrapping.

```c
            if (g_boot.cursor_y >= g_boot.rows) {
                fb_scroll();
                g_boot.cursor_y = g_boot.rows - 1;
            }
```

If we've reached the bottom, scroll the screen up and keep the cursor on the last line. Without this, we'd write characters below the visible area.

```c
            fb_char(g_boot.cursor_x, g_boot.cursor_y, *s, fg, COLOR_BLACK);
            g_boot.cursor_x++;
        }
```

Draw the character and advance the cursor. The background is always black. The foreground color is whatever the caller specified — green for prompts, white for user text, gray for system info.

```c
        if (g_boot.cursor_y >= g_boot.rows) {
            fb_scroll();
            g_boot.cursor_y = g_boot.rows - 1;
        }

        s++;
    }
}
```

We check for scroll again after advancing the cursor. This catches the case where a `\n` pushed us past the bottom.

## The Full Picture

Here's what happens when we print a character 'H' in green at position (5, 3):

```
1. fb_print("H", COLOR_GREEN) is called
2. cursor_x is 5, cursor_y is 3
3. fb_char(5, 3, 'H', 0x0000FF00, 0x00000000) is called
4. px = 5 * 8 = 40, py = 3 * 16 = 48
5. glyph = font_data['H' - 32] = font_data[40]
6. For each of 16 rows:
     Read one byte from the glyph
     For each of 8 bits:
       If bit is set: write green pixel at (40+col, 48+row)
       If bit is clear: write black pixel at (40+col, 48+row)
7. cursor_x becomes 6
```

128 pixels are written (8 wide x 16 tall). Each pixel is one memory write to the framebuffer. The display hardware continuously reads the framebuffer and sends the pixel data to the monitor over HDMI. The 'H' appears on screen.

## Key Takeaways

- A framebuffer is memory where each location is a pixel on screen
- Pixel address = `base + (y * pitch + x)`, always use pitch not width
- Our 8x16 bitmap font stores each character as 16 bytes, one per row
- Bits are tested from MSB (left) to LSB (right) using a shifting mask
- Scrolling copies pixel rows upward and clears the bottom
- `fb_print` manages cursor position, line wrapping, and auto-scrolling
- The entire font for 95 printable ASCII characters is just 1,520 bytes

Next: receiving input from the keyboard.
