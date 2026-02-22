---
layout: default
title: "Chapter 39: File Viewers"
parent: "Phase 2: The Survival Toolkit"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 5
---

# Chapter 39: File Viewers

## Beyond the File List

The file browser can navigate directories. It shows names, sizes, and icons. But tapping a file shows a popup: name, size, "Tap to dismiss." That's not viewing — it's reading metadata. The user brought files on their SD card for a reason. A survival guide in `README.txt`. A map saved as a BMP. Notes they typed on a laptop before the grid went down. The file browser can find these files, but it can't show what's inside them.

This chapter fixes that. Tapping a text file opens a scrollable text viewer. Tapping a BMP image renders it to the display. Unknown file types keep the original info popup. No new source files — everything fits inside `app_files.c`, with one small addition to the display driver.

## Reading Files Into Memory

Both viewers need the same first step: get the file contents into RAM. But FAT32 and exFAT have completely different read APIs. FAT32 uses a streaming model — open, read chunks, close:

```c
int fh = fat32_file_open(path);
fat32_file_read(fh, buf, 4096);  /* repeat until EOF */
fat32_file_close(fh);
```

exFAT uses a bulk model — one call, get the whole file:

```c
void *data = exfat_readfile(vol, path, &size);
/* use it, then free() */
```

A small abstraction hides this difference:

```c
static void *read_file(const char *path, uint32_t max_size,
                       uint32_t *out_size)
{
    if (s_fs_type == FS_FAT32) {
        int fh = fat32_file_open(path);
        if (fh < 0) return NULL;

        uint8_t *buf = malloc(max_size);
        if (!buf) { fat32_file_close(fh); return NULL; }

        uint32_t total = 0;
        while (total < max_size) {
            int n = fat32_file_read(fh, buf + total, 4096);
            if (n <= 0) break;
            total += (uint32_t)n;
        }
        fat32_file_close(fh);

        *out_size = total;
        return buf;
    } else {
        size_t fsz;
        void *data = exfat_readfile(s_exvol, path, &fsz);
        if (!data) return NULL;
        if (fsz > max_size) { free(data); return NULL; }
        *out_size = (uint32_t)fsz;
        return data;
    }
}
```

Caller gets a `malloc`'d buffer, frees it when done. The `max_size` parameter protects against accidentally loading a 2 GB file into 520 KB of RAM. For FAT32, we read in 4 KB chunks — small enough to keep stack usage low, large enough to amortize the SD card overhead. For exFAT, the driver already allocates and reads the whole file, so we just check the size and reject anything too large.

Both viewers call `read_file()` with their respective limits: 32 KB for text, ~307 KB for BMP images.

## Detecting File Types

Which viewer to launch depends on the filename extension. A helper function checks whether a name ends with a given extension, using the same case-insensitive comparison the file browser already uses for sorting:

```c
static int has_ext(const char *name, const char *ext)
{
    int nlen = (int)strlen(name);
    int elen = (int)strlen(ext);
    if (nlen < elen) return 0;
    return strcasecmp_simple(name + nlen - elen, ext) == 0;
}
```

Text files match a list of common extensions:

```c
static int is_text_file(const char *name)
{
    static const char *exts[] = {
        ".txt", ".log", ".md", ".csv", ".c", ".h",
        ".py", ".sh", ".cfg", ".ini", ".json", NULL
    };
    for (int i = 0; exts[i]; i++)
        if (has_ext(name, exts[i])) return 1;
    return 0;
}
```

This list covers what you'd actually find on a survival SD card: notes (`.txt`, `.md`), configuration files (`.cfg`, `.ini`, `.json`), data logs (`.csv`, `.log`), and source code (`.c`, `.h`, `.py`, `.sh`). It's deliberately inclusive — better to show a file as text and let the user judge than to refuse to open it. BMP detection is simpler: just check for `.bmp`.

The dispatcher replaces the old direct call to `show_file_info()`:

```c
static void open_file(struct file_entry *e)
{
    char full_path[256];
    build_full_path(full_path, sizeof(full_path), e->name);

    if (is_text_file(e->name))
        view_text_file(full_path, e->name, e->size);
    else if (is_bmp_file(e->name))
        view_bmp_file(full_path, e->name, e->size);
    else
        show_file_info(e);
}
```

Text first, BMP second, fallback third. When the viewer returns — back button for text, tap to dismiss for BMP — the file browser redraws itself and waits for the next tap.

## The Text Viewer

Loading a file into memory is just the start. To display text page by page, we need to know where each line begins.

The strategy: scan the buffer once, replace every `\n` with `\0`, and record the offset of each line's first character in an array. Now each line is a standalone C string — we can pass any `buf + line_off[i]` directly to `display_string()` without copying or counting.

```c
uint16_t *line_off = malloc(TEXT_MAX_LINES * sizeof(uint16_t));

int nlines = 0;
line_off[0] = 0;
nlines = 1;
for (uint32_t i = 0; i < actual && nlines < TEXT_MAX_LINES; i++) {
    if (buf[i] == '\n') {
        buf[i] = '\0';
        if (i + 1 < actual)
            line_off[nlines++] = (uint16_t)(i + 1);
    } else if (buf[i] == '\r') {
        buf[i] = '\0';
    }
}
```

`\r` characters get nulled too — Windows-style `\r\n` line endings become `\0\0`, and the second byte is harmless because the line offset points past it. The offsets are `uint16_t` because 32 KB of text fits in 16 bits, saving half the memory compared to `uint32_t`.

Memory budget: 32 KB file buffer plus 2 KB line table (1000 lines × 2 bytes) = 34 KB total. That's about 6.5% of the ESP32's RAM. Freed completely when the viewer closes.

### Screen Layout

The text viewer reuses the file browser's three-zone layout:

```
y=0..23:    [< Back]  "filename.txt"
y=24..215:  12 lines of text content (gray on black)
y=216..239: [< Pg Up]  "1-12 / 47"  [Pg Dn >]
```

Twelve lines per page, 40 characters per line. The 8-pixel-wide VGA font gives us 40 characters across the 320-pixel screen. Lines longer than 40 characters are truncated — no word wrap. For a survival device showing notes and configs, truncation is the right tradeoff: wrapping would halve the visible content and make code or data files unreadable.

The footer shows a line range indicator: "1-12 / 47" means lines 1 through 12 of 47 total. Page Up and Page Down buttons appear only when there's content in that direction — the same conditional rendering pattern the file browser uses.

### Pagination

A single integer, `top_line`, tracks the scroll position. Page Down adds 12, Page Up subtracts 12 (clamped to 0). Each navigation redraws the content area and footer. The header doesn't change — the filename stays put.

The touch zones mirror the file browser exactly: "< Back" in the top-left corner, "< Pg Up" in the bottom-left, "Pg Dn >" in the bottom-right. A user who learned to navigate directories already knows how to navigate text pages. Same muscle memory, different content.

## The BMP Viewer

Text is straightforward — bytes become characters. Images are harder. The display speaks RGB565 (16 bits per pixel), but BMP files store pixels as 24-bit BGR. Every pixel needs conversion, every row needs reordering, and the whole thing has to fit in memory.

### Why BMP

We could support PNG or JPEG, but both need decoders. PNG needs zlib (we have miniz, but the PNG container format adds complexity). JPEG needs a DCT decoder — not trivial on an ESP32. BMP is uncompressed, so the "decoder" is just byte shuffling. It's enough to prove the display pipeline works, and BMP is what you'd create from a screenshot or a simple image tool.

### The BMP Format

A BMP file starts with a 54-byte header (14-byte file header + 40-byte BITMAPINFOHEADER). The fields we care about:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 2 | Signature: `'B'` `'M'` |
| 10 | 4 | Pixel data offset |
| 18 | 4 | Width (pixels) |
| 22 | 4 | Height (signed — positive = bottom-up) |
| 28 | 2 | Bits per pixel |
| 30 | 4 | Compression (0 = none) |

We validate the signature, reject anything that isn't 24-bit uncompressed, and extract the dimensions. The height field has a quirk: positive means the rows are stored bottom-to-top. The first row in the file is the bottom row of the image. Negative height means top-to-bottom (rare). We handle both.

```c
int bottom_up = (bmp_h > 0);
int img_h = bottom_up ? bmp_h : -bmp_h;
```

### Row Padding

BMP rows are padded to a 4-byte boundary. A 100-pixel-wide image has 300 bytes of pixel data per row (100 × 3), but each row in the file is 300 bytes — already a multiple of 4. A 101-pixel-wide image has 303 bytes of pixel data, padded to 304. The formula:

```c
uint32_t row_bytes = ((uint32_t)(img_w * 3) + 3) & ~3u;
```

Get this wrong and every row shifts by a few pixels. The image renders as a diagonal smear. It's one of those details that's easy to forget and painful to debug.

### A New Display Primitive

Drawing an image pixel by pixel with `display_char` or `display_fill_rect` would be catastrophically slow — 320×240 = 76,800 individual SPI transactions. The existing `display_fill_rect` sends one color repeated across a line. What we need is a way to send a pre-built array of different colors for a single scanline.

```c
void display_draw_rgb565_line(int x, int y, int w,
                               const uint16_t *pixels);
```

On the ESP32, this wraps `esp_lcd_panel_draw_bitmap()` with clipping:

```c
void display_draw_rgb565_line(int x, int y, int w,
                               const uint16_t *pixels)
{
    if (y < 0 || y >= DISPLAY_HEIGHT || w <= 0) return;
    if (x < 0) { pixels -= x; w += x; x = 0; }
    if (x + w > DISPLAY_WIDTH) w = DISPLAY_WIDTH - x;
    if (w <= 0) return;

    esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + 1, pixels);
}
```

One SPI transaction per scanline. A 320-pixel-wide image takes 240 transactions to draw — three orders of magnitude fewer than pixel-by-pixel. The emulator version copies directly into the framebuffer under mutex.

This function is general-purpose. Any future feature that needs to blit pixel data — sprites, charts, camera input — can use it.

### Rendering

The render loop is simple. For each screen row, calculate which BMP row it corresponds to (reversing the order for bottom-up images), convert BGR bytes to RGB565, and blit:

```c
uint16_t line[320];
for (int y = 0; y < draw_h; y++) {
    int src_row;
    if (bottom_up)
        src_row = (img_h - 1 - y);
    else
        src_row = y;

    uint32_t row_offset = data_offset + (uint32_t)src_row * row_bytes;
    const uint8_t *row = buf + row_offset;

    for (int x = 0; x < draw_w; x++) {
        uint8_t b = row[x * 3 + 0];
        uint8_t g = row[x * 3 + 1];
        uint8_t r = row[x * 3 + 2];
        line[x] = display_rgb(r, g, b);
    }
    display_draw_rgb565_line(off_x, off_y + y, draw_w, line);
}
```

Note the byte order: BMP stores Blue, Green, Red. Our `display_rgb()` macro expects Red, Green, Blue. Swap two arguments and the image renders with red and blue channels inverted — a subtle bug that makes skin tones look like a bad Instagram filter.

Images smaller than 320×240 are centered. Images larger are clipped to the top-left corner. Black borders fill any unused screen area. After rendering, the filename is overlaid in the top-left corner on a black rectangle, just enough to identify what you're looking at.

### Memory

A full-screen 320×240×3 BMP is about 230 KB of pixel data plus the 54-byte header. The `read_file()` call caps at ~307 KB (`320 * 240 * 4` — generous padding for headers and row alignment). This is the largest single allocation in the toolkit, consuming about 44% of available heap. It works because nothing else is allocated at the time — the file browser uses only static buffers.

The 640-byte scanline conversion buffer (`uint16_t line[320]`) lives on the stack. We convert one row at a time, so we never need two rows in memory simultaneously.

## Wiring It Together

The main loop's file tap handler changes from one line to four:

```c
/* Before */
show_file_info(e);
draw_screen();

/* After */
open_file(e);
display_clear(COLOR_BLACK);
draw_screen();
```

The `display_clear()` before `draw_screen()` is necessary because viewers take over the full screen. Text viewer draws its own header, content, and footer. BMP viewer draws a full-screen image. When they return, the file browser can't assume anything about the display state — it has to start from a clean slate.

## What We Built

```
File                         Lines   Change
───────────────────────────  ─────   ──────────────────────────────
esp32/main/display.h            53   +display_draw_rgb565_line()
esp32/main/display.c           192   +display_draw_rgb565_line()
cyd-emulator/src/emu_display.c 122   +display_draw_rgb565_line()
esp32/main/app_files.c         804   +317 lines: file type detection,
                                     read_file(), text viewer,
                                     BMP viewer, open_file() dispatch
```

The file browser grew from 487 lines to 804. Most of the new code is the two viewers. The text viewer is about 120 lines — file loading, line parsing, page rendering, touch loop. The BMP viewer is about 90 lines — header parsing, row conversion, scanline blitting. The shared infrastructure — `read_file()`, `has_ext()`, `build_full_path()` — adds another 70 lines that both viewers use.

Total heap usage during viewing: ~34 KB for text files, ~230 KB for full-screen BMPs. Both allocations are transient — allocated on entry, freed on exit. The file browser's static buffers (6.8 KB for the entry list) stay allocated throughout the app's lifetime.

The file browser is now genuinely useful. You can navigate to a text file and read it. You can view a BMP image. You can check the contents of a config file, read a note, or look at a map — all on a $7 device with no operating system.

---

**Next:** Chapter 40: The Notes App
