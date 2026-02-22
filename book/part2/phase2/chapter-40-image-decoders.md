---
layout: default
title: "Chapter 40: Image Decoders"
parent: "Phase 2: The Survival Toolkit"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 6
---

# Chapter 40: Image Decoders

## The Gap in Our Viewer

The BMP viewer works. It reads uncompressed pixels, flips rows, converts BGR to RGB565, and draws scanlines. But BMP is a format you settle for, not one you choose. The photos on your SD card are JPEGs. The maps you downloaded are PNGs. The survival guide's diagrams are PNGs. A viewer that only handles BMP is a viewer that can't open the files that matter.

PNG and JPEG decoders exist as libraries — libpng, libjpeg, stb_image. But they're built for desktops. libpng alone is 80,000+ lines of code. stb_image is 7,500. On an ESP32 with 520 KB of RAM and 4 MB of flash, we need something smaller. Much smaller.

This chapter builds two decoders from scratch: **sped** (Simplest PNG ESP32 Decoder) for PNG files, and **femtojpeg** for baseline JPEG. Together they're about 1,100 lines of C. They decode directly to RGB565, the display's native pixel format, via row-by-row callbacks. Both support downscaling — sped can halve or quarter a PNG, and femtojpeg can decode at 1/4 or 1/8 resolution — so even images far larger than our 320×240 screen can be displayed without exhausting memory.

## The Row Callback Pattern

Both decoders share the same output interface: a function pointer called once per decoded image row.

```c
typedef void (*sped_row_cb)(int y, int w,
                            const uint16_t *rgb565, void *user);
```

The decoder calls this with the row index (0 = top), width, a pointer to a temporary buffer of RGB565 pixels, and a user-provided context. The caller blits that row to the display and moves on. The decoder reuses the buffer for the next row, so the caller must consume it immediately.

This pattern is perfect for embedded: the decoder only needs one or two rows of memory at a time, and the display gets updated as decoding progresses — no waiting for the full image to finish.

## PNG: How It Works

A PNG file is a series of *chunks*. Each chunk has a 4-byte length, a 4-byte type code, the data, and a 4-byte CRC (which we skip — checking CRCs is a luxury on a survival device).

The chunks we care about:

| Chunk | Purpose |
|-------|---------|
| **IHDR** | Width, height, bit depth, color type |
| **PLTE** | Palette (for indexed-color images) |
| **tRNS** | Transparency for palette entries |
| **IDAT** | Compressed pixel data (one or more) |
| **IEND** | End of file |

The pixel data across all IDAT chunks is a single zlib-compressed stream. Decompressed, it yields scanlines: one filter byte followed by the pixel bytes for that row. The filter byte tells you how to reconstruct the original pixel values from the delta-encoded differences.

### The Five Scanline Filters

PNG doesn't store raw pixels. Each scanline is filtered to improve compression. The filter byte (0–4) tells us how to reverse the encoding:

| Filter | Name | Formula |
|--------|------|---------|
| 0 | None | `x` |
| 1 | Sub | `x + a` |
| 2 | Up | `x + b` |
| 3 | Average | `x + (a + b) / 2` |
| 4 | Paeth | `x + paeth(a, b, c)` |

Here `a` is the pixel to the left (or zero at the start of a row), `b` is the pixel above (or zero on the first row), and `c` is the pixel diagonally above-left. The Paeth predictor picks whichever of `a`, `b`, or `c` is closest to the prediction `a + b - c`:

```c
static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = (int)a + (int)b - (int)c;
    int pa = p - (int)a; if (pa < 0) pa = -pa;
    int pb = p - (int)b; if (pb < 0) pb = -pb;
    int pc = p - (int)c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}
```

This is a direct port of the PNG specification. The cleverness is in the encoder, which picks the filter that produces the most compressible output per row. The decoder just reverses it.

### The DEFLATE Problem

PNG's pixel data is compressed with DEFLATE (zlib). Writing a DEFLATE decompressor from scratch would be a chapter on its own — Huffman trees, length/distance codes, sliding windows. Fortunately, ESP-IDF includes `tinfl` (from the miniz library) in its ROM. One function, `tinfl_decompress()`, handles everything. We include it with `#include "miniz.h"`.

The trick is that IDAT data may be split across multiple chunks, but it's a single continuous zlib stream. So we collect pointers to all IDAT chunks during the chunk scan, then feed them to `tinfl_decompress()` sequentially.

### Streaming Decompression

We can't decompress the entire image at once — that could be hundreds of KB. Instead, we decompress into a 32 KB dictionary buffer (the minimum for DEFLATE), process whatever output we get, and repeat.

The decompressed bytes are a mix of filter bytes and pixel data. We assemble them into complete scanlines using a state machine:

```c
int sl_pos = 0;    /* 0 = expecting filter byte */
uint8_t filter = 0;

while (avail > 0 && row < h) {
    if (sl_pos == 0) {
        filter = *dp++;
        avail--;
        sl_pos = 1;
    } else {
        size_t need = stride - (sl_pos - 1);
        size_t take = (avail < need) ? avail : need;
        memcpy(cur + (sl_pos - 1), dp, take);
        dp += take;
        avail -= take;
        sl_pos += take;

        if (sl_pos > stride) {
            /* Complete scanline — apply filter, convert, emit */
            ...
        }
    }
}
```

Position 0 means we're reading the filter byte. Positions 1 through `stride` collect the filtered pixel data. When we have a full scanline, we apply the inverse filter, convert to RGB565, emit via the callback, swap the current/previous row buffers, and reset.

### Color Conversion

PNG supports five color types. We handle them all with a switch during the RGB565 conversion:

```c
for (uint32_t x = 0; x < w; x++) {
    uint8_t r, g, bl;
    switch (ctype) {
        case 0: r = g = bl = cur[x]; break;           /* grayscale */
        case 2: r=cur[x*3]; g=cur[x*3+1]; bl=cur[x*3+2]; break;  /* RGB */
        case 3: { uint8_t idx = cur[x];
                  r=pal[idx][0]; g=pal[idx][1]; bl=pal[idx][2]; break; }
        case 4: r = g = bl = cur[x*2]; break;         /* gray+alpha */
        case 6: r=cur[x*4]; g=cur[x*4+1]; bl=cur[x*4+2]; break;  /* RGBA */
    }
    out[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3);
}
```

Alpha channels (types 4 and 6) are ignored — our display has no concept of transparency. We just take the color components and pack them into RGB565. For indexed images (type 3), we look up the palette we parsed from the PLTE chunk.

### PNG Downscaling

A 640×480 PNG would need 640 × 3 × 2 (cur + prev) + 640 (out) + 32,768 (dict) = ~37 KB just for working memory, plus the output row is wider than our 320-pixel display. We need to downscale during decode.

PNG filter reconstruction requires processing every scanline — you can't skip rows, because each row depends on the previous one via the Up, Average, and Paeth filters. So we decode at full resolution and average the output. For scale=2, we accumulate 2×2 blocks of pixels into a sum buffer, then emit one averaged row per two input rows. For scale=4, we accumulate 4×4 blocks.

The accumulator stores per-output-pixel R, G, B sums as `uint16_t` values (max sum for scale=4: 255 × 16 = 4,080, fits in 16 bits). Each decoded scanline's pixels are extracted to R/G/B, accumulated into the appropriate output column, and every `scale` rows the sums are divided and packed to RGB565:

```c
for (uint32_t x = 0; x < limit; x++) {
    uint8_t r, g, bl;
    get_pixel(cur, x, ctype, pal, &r, &g, &bl);
    uint32_t ox = x / scale;
    acc[ox * 3 + 0] += r;
    acc[ox * 3 + 1] += g;
    acc[ox * 3 + 2] += bl;
}
if ((row % scale) == scale - 1) {
    int div = scale * scale;
    for (uint32_t ox = 0; ox < out_w; ox++) {
        uint8_t r  = acc[ox * 3 + 0] / div;
        uint8_t g  = acc[ox * 3 + 1] / div;
        uint8_t bl = acc[ox * 3 + 2] / div;
        out[ox] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3);
    }
    cb(out_row++, out_w, out, user);
    memset(acc, 0, out_w * 3 * sizeof(uint16_t));
}
```

The extra cost is the accumulator: `out_w × 3 × 2` bytes. For a 640×480 image scaled to 320×240: 320 × 6 = 1,920 bytes. A small price for handling images twice the screen size.

### PNG Memory Budget

| Buffer | Size | Purpose |
|--------|------|---------|
| `cur` | width × bpp | Current scanline (raw pixels) |
| `prev` | width × bpp | Previous scanline (for filters) |
| `out` | out_w × 2 | RGB565 output row |
| `dict` | 32,768 | DEFLATE dictionary |
| `acc` | out_w × 6 | Downscale accumulator (scale > 1 only) |

For a 320-pixel-wide RGB image at 1:1: 960 + 960 + 640 + 32,768 = ~35 KB. For a 640-wide image at 1/2: 1,920 + 1,920 + 640 + 32,768 + 1,920 = ~39 KB. The 32 KB DEFLATE dictionary dominates in every case.

Total: 296 lines of C. No external dependencies beyond miniz (already in ESP-IDF).

## JPEG: How It Works

JPEG is a different beast. Where PNG losslessly compresses exact pixel values, JPEG throws away information the human eye won't miss — high-frequency detail, subtle color differences. The decoder must reverse this transformation: parse Huffman codes, dequantize frequency coefficients, apply an inverse DCT, convert from YCbCr color space to RGB, and potentially upsample chroma components. It's intricate machinery, but each piece is small.

### The Marker Stream

A JPEG file is a sequence of *markers* — two-byte codes starting with `0xFF`. The decoder scans for markers it understands and skips the rest:

| Marker | Code | Purpose |
|--------|------|---------|
| SOI | `0xFFD8` | Start of image |
| SOF0 | `0xFFC0` | Baseline DCT frame — dimensions, components, sampling |
| DQT | `0xFFDB` | Quantization table(s) |
| DHT | `0xFFC4` | Huffman table(s) |
| DRI | `0xFFDD` | Restart interval |
| SOS | `0xFFDA` | Start of scan — entropy-coded data follows |
| EOI | `0xFFD9` | End of image |

Progressive JPEG (`0xFFC2`) is rejected outright — decoding it requires multiple passes over the data, which is impractical on an ESP32.

### Huffman Decoding

JPEG's entropy-coded data is Huffman-compressed. The Huffman tables (defined in DHT markers) map variable-length bit sequences to symbols. Our decoder builds a table-based lookup: for each code length (1–16 bits), store the minimum code, maximum code, and an index into the value array.

```c
static void huff_build(const uint8_t *counts, huff_table_t *ht)
{
    uint16_t code = 0;
    uint8_t j = 0;
    for (int i = 0; i < 16; i++) {
        if (counts[i] == 0) {
            ht->min_code[i] = 0;
            ht->max_code[i] = 0xFFFF;
        } else {
            ht->min_code[i] = code;
            ht->max_code[i] = code + counts[i] - 1;
            ht->val_ptr[i] = j;
            j += counts[i];
            code += counts[i];
        }
        code <<= 1;
    }
}
```

Decoding reads bits one at a time, growing the code until it falls within a valid range for that length. It's not the fastest approach — a lookup table would be faster — but it uses almost no memory: 48 bytes per table plus the value array.

A JPEG has four Huffman tables: two DC (one per luminance and chrominance) and two AC. DC tables are small — at most 16 symbols. AC tables can have up to 256. The naive approach is a flat `huff_val[4][256]` array: 1,024 bytes, three-quarters wasted on DC tables that never use more than 16 entries.

We split them: `dc_vals[2][16]` (32 bytes) and `ac_vals[2][256]` (512 bytes). The `huff_decode()` function indexes the right array based on table ID. This saves 480 bytes in the decoder context — meaningful when your total working memory target is under 7 KB.

### Quantization and the DCT

JPEG divides the image into 8×8 blocks. Each block is transformed via the Discrete Cosine Transform (DCT), which converts spatial pixel values into frequency coefficients. Low-frequency coefficients represent smooth gradients; high-frequency ones represent edges and fine detail.

The encoder *quantizes* these coefficients — divides each one by a value from a quantization table and rounds to the nearest integer. High-frequency coefficients get divided by large numbers, often rounding to zero. That's where JPEG's lossy compression comes from. The decoder multiplies them back (dequantization), but the lost precision is gone forever.

The quantization tables are stored in DQT markers. We parse them and pre-multiply each entry by a Winograd scale factor — this folds the IDCT's constant multiplications into the dequantization step, so the IDCT itself needs fewer multiplies.

### The Winograd IDCT

The Inverse DCT transforms 64 frequency coefficients back into 64 pixel values. The naive approach requires 1024 multiplications per block. The Winograd algorithm reduces this to 5 multiplications per row or column — a total of 80 per block. On an ESP32 without hardware floating-point, this matters.

The four constants (encoded as fixed-point integers) come from the DCT's trigonometric basis:

```c
static int16_t imul_362(int16_t w) {
    return (int16_t)(((long)w * 362 + 128) >> 8);
}  /* 1/cos(4π/16) ≈ 1.4142 */
static int16_t imul_669(int16_t w) {
    return (int16_t)(((long)w * 669 + 128) >> 8);
}  /* 1/cos(6π/16) ≈ 2.6131 */
static int16_t imul_277(int16_t w) {
    return (int16_t)(((long)w * 277 + 128) >> 8);
}  /* 1/cos(2π/16) ≈ 1.0824 */
static int16_t imul_196(int16_t w) {
    return (int16_t)(((long)w * 196 + 128) >> 8);
}  /* 1/(cos(2π/16)+cos(6π/16)) */
```

The IDCT runs in two passes: rows first, then columns. A common shortcut: if all AC coefficients in a row (or column) are zero — which happens frequently due to quantization — the output is just the DC value repeated. This "DC-only" fast path skips the entire butterfly computation.

### Chroma Subsampling

Human eyes are more sensitive to brightness (luminance) than color (chrominance). JPEG exploits this by storing color channels at lower resolution. The sampling factors in the SOF0 marker define the ratio:

| Mode | Y blocks per MCU | Chroma blocks | Resolution ratio |
|------|-----------------|---------------|-----------------|
| 4:4:4 (H1V1) | 1 | 1 Cb + 1 Cr | Full resolution |
| 4:2:2 (H2V1) | 2 | 1 Cb + 1 Cr | Half horizontal |
| 4:2:0 (H2V2) | 4 | 1 Cb + 1 Cr | Half both axes |

Most photos use 4:2:0 — four luma blocks share one Cb and one Cr block per MCU (Minimum Coded Unit). The decoder must upsample: spread each chroma sample across multiple pixels.

We handle this with bit shifts derived from the sampling factors:

```c
int h_shift = (hsamp[0] > 1) ? 1 : 0;
int v_shift = (vsamp[0] > 1) ? 1 : 0;
...
int cx = px >> h_shift;
int cy = py >> v_shift;
Cb_v = cb_block[cy * 8 + cx];
Cr_v = cr_block[cy * 8 + cx];
```

When `h_shift` is 1 (H2), pixel columns 0–1 map to chroma column 0, columns 2–3 to chroma column 1, and so on. This is nearest-neighbor upsampling — not as smooth as bilinear, but it costs zero extra computation and the quality loss is invisible at 320×240.

### YCbCr to RGB

JPEG stores pixels in YCbCr color space, not RGB. The conversion uses fixed-point integer math:

```c
int cr = (int)Cr_v - 128;
int cb_val = (int)Cb_v - 128;
int r = (int)Y + ((cr * 359) >> 8);
int g = (int)Y - ((cb_val * 88 + cr * 183) >> 8);
int b = (int)Y + ((cb_val * 454) >> 8);
```

The magic numbers (359, 88, 183, 454) are the ITU-R BT.601 conversion coefficients scaled by 256 for fixed-point arithmetic. The results are clamped to 0–255 and packed directly into RGB565 — no intermediate 24-bit buffer.

### Restart Markers

Long JPEG files may contain restart markers (`0xFFD0` through `0xFFD7`) that reset the DC prediction state. This allows partial decoding — if a few MCUs are corrupted, the damage doesn't propagate beyond the next restart point. The decoder counts MCUs and processes restart markers when the counter reaches zero:

```c
if (ctx.restart_interval) {
    if (ctx.restarts_left == 0)
        process_restart(&ctx);
    ctx.restarts_left--;
}
```

### JPEG Downscaling

A 2560×1920 photo from a digital camera would need a 2560-pixel-wide row buffer — over 80 KB just for the output. We need to decode it smaller. JPEG's block-based structure gives us two natural scaling strategies.

**1/8 scale — DC only.** Each 8×8 block in a JPEG starts with a DC coefficient that represents the block's average value. At 1/8 scale, we extract just the DC coefficient, skip all 63 AC coefficients (we still consume their Huffman codes to advance the bitstream), and emit a single pixel per block. No IDCT needed:

```c
*pixel_out = clamp8(DESCALE(dc * q[0]) + 128);
/* Then skip AC codes to advance the bitstream */
```

A 2560×1920 H2V2 image becomes 320×240 — exactly our screen size — with minimal computation.

**1/4 scale — IDCT then average.** We run the full IDCT (we already have it), then average each 8×8 output block into 2×2 pixels by summing four 4×4 quadrants:

```c
static void block_to_2x2(const uint8_t *blk, uint8_t out[4])
{
    for (int qy = 0; qy < 2; qy++)
        for (int qx = 0; qx < 2; qx++) {
            int sum = 0;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    sum += blk[(qy * 4 + y) * 8 + qx * 4 + x];
            out[qy * 2 + qx] = (uint8_t)(sum >> 4);
        }
}
```

This reuses the existing IDCT code. A 1280×960 image becomes 320×240.

### Two-Pass Decode for H2V2

H2V2 (4:2:0) MCUs are 16 pixels tall. At 1:1 scale, buffering all 16 rows at 320 pixels wide costs 320 × 16 × 2 = 10,240 bytes. We can halve this by decoding each MCU row twice.

The decoder saves its state (bitstream position, DC predictions, restart counter) at the start of each MCU row. The first pass decodes and emits the top 8 pixel rows. Then it restores the saved state and decodes the same MCU row again, emitting the bottom 8 rows. The row buffer drops from `width × 16 × 2` to `width × 8 × 2` = 5,120 bytes.

The cost: 2× the Huffman and IDCT work for H2V2 images at full scale. H1V1 and H2V1 images (MCU height = 8) don't need the second pass. At 1/4 and 1/8 scale, the MCU output height fits in a single pass regardless.

### JPEG Memory Budget

| Mode | Context (stack) | Blocks (stack) | Row buffer (heap) | **Total** |
|------|----------------|----------------|-------------------|-----------|
| 1:1 H2V2 320px | 1,320 | 384 | 5,120 | **~7 KB** |
| 1:1 H1V1 320px | 1,320 | 128 | 5,120 | **~7 KB** |
| 1/4 H2V2 1280px | 1,320 | 384 | 2,560 | **~4 KB** |
| 1/8 H2V2 2560px | 1,320 | 384 | 1,280 | **~3 KB** |

Under 7 KB in every case. That's competitive with TJpgDec (~3.5 KB) while supporting row-by-row streaming output.

Total: 819 lines of C. No external dependencies whatsoever.

## Integration

Both decoders follow the same pattern: read the file into memory, get dimensions, compute a scale factor, decode with a row callback. The scale factor is chosen to bring the output dimensions down to something reasonable for our 320×240 display:

```c
/* PNG: sped supports 1/2/4 */
if (img_w > DISPLAY_WIDTH * 2 || img_h > DISPLAY_HEIGHT * 2)
    scale = 4;
else if (img_w > DISPLAY_WIDTH || img_h > DISPLAY_HEIGHT)
    scale = 2;

/* JPEG: femtojpeg supports 1/4/8 */
if (img_w > DISPLAY_WIDTH * 4 || img_h > DISPLAY_HEIGHT * 4)
    scale = 8;
else if (img_w > DISPLAY_WIDTH || img_h > DISPLAY_HEIGHT)
    scale = 4;
```

Both decoders take the scale as a parameter:

```c
sped_decode(buf, actual, scale, row_cb, &ctx);
fjpeg_decode(buf, actual, scale, row_cb, &ctx);
```

The `open_file()` dispatcher in `app_files.c` routes by extension:

```c
if (is_text_file(e->name))
    view_text_file(full_path, e->name, e->size);
else if (is_bmp_file(e->name))
    view_bmp_file(full_path, e->name, e->size);
else if (is_png_file(e->name))
    view_decoded_image(full_path, e->name, e->size, 1);
else if (is_jpg_file(e->name))
    view_decoded_image(full_path, e->name, e->size, 0);
else
    show_file_info(e);
```

From the user's perspective, tapping a `.png` or `.jpg` file does the same thing as tapping a `.bmp` — the image appears on screen. A 2560×1920 JPEG photo from a digital camera is decoded at 1/8 scale to 320×240 and displayed directly. A 640×480 PNG map is decoded at 1/2 scale to 320×240. Images that fit the screen are decoded at full resolution. The scaling is invisible to the user — images just work.

## What We Built

```
File                         Lines   Change
───────────────────────────  ─────   ──────────────────────────────
sped/sped.h                     36   New: PNG decoder header
sped/sped.c                    296   New: Streaming PNG decoder with
                                     1/2 and 1/4 downscaling
femtojpeg/femtojpeg.h           36   New: JPEG decoder header
femtojpeg/femtojpeg.c          819   New: Baseline JPEG decoder with
                                     1/4 and 1/8 downscaling, two-pass
                                     H2V2, split Huffman tables
esp32/main/app_files.c        1106   +302 lines: Image viewer with
                                     scale computation, file type routing
```

Two decoders, 1,187 lines total:

- **sped**: 296 lines. Parses PNG chunks, inflates via tinfl, reconstructs all five scanline filters, handles grayscale/RGB/RGBA/indexed. Supports 1/2 and 1/4 downscaling via pixel averaging. Needs ~35 KB of working memory (dominated by the 32 KB DEFLATE dictionary).

- **femtojpeg**: 819 lines. Parses JPEG markers, decodes Huffman codes, dequantizes with pre-scaled Winograd factors, applies 80-multiply IDCT, upsamples chroma, converts YCbCr to RGB565. Supports 1/4 scale (IDCT + 4×4 averaging) and 1/8 scale (DC-only, no IDCT). Two-pass decode halves the row buffer for H2V2 images. Split Huffman value tables save 480 bytes. Total working memory: under 7 KB at any scale. Zero external dependencies.

Both decoders are standalone libraries — they know nothing about the display, the filesystem, or the ESP32. They take a buffer of bytes and call a function with rows of pixels. This means they can be reused in any embedded project that needs to decode images to RGB565: e-ink displays, LED matrices, frame buffers, or any other target that accepts pixel data row by row.

The file browser can now open the files that actually matter. Text files, BMP images, PNG diagrams, JPEG photos — all on a $7 microcontroller with no operating system, no GPU, and 520 KB of RAM.

---

**Next:** Chapter 41: The Image Viewer
