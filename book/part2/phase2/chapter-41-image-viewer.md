---
layout: default
title: "Chapter 41: The Image Viewer"
parent: "Phase 2: The Survival Toolkit"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 7
---

# Chapter 41: The Image Viewer

## Beyond Tap-to-Dismiss

The image decoders from Chapter 40 can display any PNG or JPEG on screen. But the viewer is static: open a file, see it centered on the display, tap to go back. If the image is larger than 320×240, you see the center and nothing else. If it's smaller, you see it at 1:1 with no way to zoom in and examine detail.

A useful image viewer needs two things: **zoom** to change magnification, and **pan** to move around a zoomed image. On a phone, you'd pinch to zoom and swipe to pan. Our CYD has a resistive touchscreen — single-touch only, no multi-touch gestures. So we need a different approach: on-screen buttons for zoom, and drag-to-pan for navigation.

## The Architecture Shift

The old viewer was simple: decode the image row by row, blit each row directly to the display, done. This streaming approach uses minimal memory but produces a static image. You can't scroll or zoom because the pixel data is gone — it was consumed during the single decode pass.

The new viewer decodes to an off-screen pixel buffer:

```c
struct img_buf_ctx {
    uint16_t *pixels;
    int w, h;
};

static void img_buf_cb(int y, int w,
                        const uint16_t *rgb565, void *user)
{
    struct img_buf_ctx *ctx = user;
    if (y >= ctx->h) return;
    int copy_w = (w > ctx->w) ? ctx->w : w;
    memcpy(ctx->pixels + y * ctx->w, rgb565,
           copy_w * sizeof(uint16_t));
}
```

Same row callback interface, different destination. Instead of `display_draw_rgb565_line()`, we `memcpy()` each row into a heap-allocated buffer. After decoding, we free the source file data — it's no longer needed. The pixel buffer stays in memory for the lifetime of the viewing session.

The memory lifecycle:

```
1. malloc(source_file)         — read compressed file from SD
2. malloc(pixel_buffer)        — allocate decoded image
3. decode(source → pixels)     — fill pixel buffer via row callback
4. free(source_file)           — compressed data no longer needed
5. interactive viewing loop    — only pixel_buffer in memory
6. free(pixel_buffer)          — done
```

Steps 1–4 are the peak memory moment: both the source file and pixel buffer coexist. But the source file is compressed (a 320×240 JPEG photo might be 30–60 KB), while the pixel buffer is `width × height × 2` bytes (RGB565). For a 320×240 decoded image: 153,600 bytes. For a small 100×75 thumbnail: 15,000 bytes. The ESP32 has about 300 KB of usable heap, so this fits comfortably.

## The Viewport

The display shows a 320×216 pixel viewport (24 pixels reserved for the toolbar at the top). The pixel buffer holds the decoded image at whatever resolution the decoders produced — possibly smaller than the screen if the original image was tiny, possibly filling it if a large image was scaled down to fit.

The viewer's state is three numbers: `zoom`, `pan_x`, `pan_y`.

**Zoom** is an integer multiplier: 1, 2, or 4. At zoom=1, one buffer pixel maps to one screen pixel. At zoom=2, each buffer pixel covers a 2×2 block on screen, so you see half the image at double size. At zoom=4, each pixel covers 4×4, showing a quarter of the image at 4× magnification.

**Pan** is the offset in *zoomed* coordinate space — the top-left corner of the viewport within the virtual zoomed image. The zoomed image is `img_w × zoom` by `img_h × zoom` pixels. Pan values are clamped so the viewport never slides past the edges:

```c
static void clamp_pan(int *pan_x, int *pan_y,
                      int img_w, int img_h, int zoom)
{
    int zoomed_w = img_w * zoom;
    int zoomed_h = img_h * zoom;

    if (zoomed_w <= VIEW_W)
        *pan_x = -(VIEW_W - zoomed_w) / 2;  /* center */
    else {
        if (*pan_x < 0) *pan_x = 0;
        if (*pan_x > zoomed_w - VIEW_W)
            *pan_x = zoomed_w - VIEW_W;
    }
    /* Same for Y... */
}
```

When the zoomed image is smaller than the viewport, the pan value goes negative — this centers the image on screen. When it's larger, pan is clamped to `[0, zoomed_size - viewport_size]`.

## Drawing the Viewport

Each frame, we scan through every screen row in the viewport, map it back to the pixel buffer through the zoom and pan transform, and send it to the display:

```c
for (int sy = 0; sy < VIEW_H; sy++) {
    int zy = pan_y + sy;
    if (zy < 0 || zy >= zoomed_h) {
        memset(line, 0, sizeof(line));  /* black */
    } else {
        int iy = zy / zoom;
        for (int sx = 0; sx < VIEW_W; sx++) {
            int zx = pan_x + sx;
            if (zx < 0 || zx >= zoomed_w)
                line[sx] = COLOR_BLACK;
            else
                line[sx] = pixels[iy * img_w + zx / zoom];
        }
    }
    display_draw_rgb565_line(0, VIEW_Y + sy, VIEW_W, line);
}
```

The integer division `zx / zoom` is nearest-neighbor scaling — the simplest and cheapest approach. At zoom=2, adjacent screen pixels show the same buffer pixel. It looks blocky at high zoom, which is fine: the point is to see individual pixel-level detail in maps and diagrams, not to produce smooth magnification.

This redraws the full viewport every time. At 320×216 pixels, that's 69,120 pixels per frame — about 138 KB of SPI data. The ILI9341 can handle this at 40 MHz SPI, so a full redraw takes roughly 28 ms. That's fast enough for smooth drag-to-pan.

## The Toolbar

The top 24 pixels hold the controls: Back, [−], [+], and a label showing the current zoom level and filename.

```
[Back] [-] [+]  2x photo.jpg
```

The buttons are just filled rectangles with text drawn over them. Inactive buttons (e.g., [−] at zoom=1, [+] at zoom=4) are dimmed to black, providing visual feedback about what's available.

```c
display_fill_rect(BTN_MINUS_X, 0, BTN_MINUS_W, VIEW_HDR_H,
                  zoom > 1 ? COLOR_DGRAY : COLOR_BLACK);
display_char(BTN_MINUS_X + 10, 4, '-',
             zoom > 1 ? COLOR_WHITE : COLOR_DGRAY,
             zoom > 1 ? COLOR_DGRAY : COLOR_BLACK);
```

## Touch Handling: Taps vs. Drags

The tricky part: the same touchscreen must handle both taps (on buttons) and drags (to pan). The difference is movement distance.

We use a polling loop at ~50 Hz via `touch_read()` and `vTaskDelay(20)`. The state machine tracks three phases:

1. **Touch down**: Record the starting screen coordinates and current pan offset.
2. **Held/dragging**: If the finger moves more than 6 pixels from the start point, it's a drag. Update `pan_x`/`pan_y` relative to the starting position and redraw the viewport.
3. **Release**: If the total movement was under the threshold, it was a tap. Check which button the tap landed on.

```c
if (touching && !was_touching) {
    drag_sx = tx; drag_sy = ty;
    drag_pan_x = pan_x; drag_pan_y = pan_y;
    dragging = 0;
}
else if (touching && was_touching) {
    int dx = tx - drag_sx;
    int dy = ty - drag_sy;
    if (!dragging && (abs(dx) > DRAG_THRESH ||
                      abs(dy) > DRAG_THRESH))
        dragging = 1;
    if (dragging && drag_sy >= VIEW_Y) {
        pan_x = drag_pan_x - dx;
        pan_y = drag_pan_y - dy;
        clamp_pan(&pan_x, &pan_y, ...);
        viewer_draw_viewport(...);
    }
}
else if (!touching && was_touching && !dragging) {
    /* It was a tap — check button zones */
}
```

The 6-pixel threshold prevents accidental drags from slightly shaky taps — resistive touchscreens have lower resolution than capacitive ones, and small jitter is normal. The drag only activates for touches that start in the viewport area (`drag_sy >= VIEW_Y`), so dragging near the toolbar doesn't accidentally pan the image.

### Center-Preserving Zoom

When zooming in or out, we want the center of the viewport to stay fixed. If you're looking at a specific detail and tap [+], that detail should stay centered on screen, just bigger.

The math: compute the viewport's center in zoomed coordinates before the zoom change, adjust zoom, then set the new pan so the center is preserved:

```c
/* Zoom in */
int cx = pan_x + VIEW_W / 2;  /* center in old zoom space */
int cy = pan_y + VIEW_H / 2;
zoom *= 2;
pan_x = cx * 2 - VIEW_W / 2;  /* center in new zoom space */
pan_y = cy * 2 - VIEW_H / 2;
clamp_pan(&pan_x, &pan_y, img_w, img_h, zoom);
```

At zoom-out, the center coordinates halve. At zoom-in, they double. The `clamp_pan` afterward ensures we don't end up showing out-of-bounds space.

## Memory Budget

The pixel buffer dominates:

| Image | Decoded size | Buffer | + decode overhead | Peak |
|-------|-------------|--------|-------------------|------|
| 320×240 JPEG (1:1) | 320×240 | 150 KB | ~7 KB | ~157 KB |
| 1280×960 JPEG (1/4) | 320×240 | 150 KB | ~4 KB | ~154 KB |
| 640×480 PNG (1/2) | 320×240 | 150 KB | ~39 KB | ~189 KB |
| 100×75 JPEG (1:1) | 100×75 | 15 KB | ~7 KB | ~22 KB |

Plus the compressed source file in memory during decode. For a typical 50 KB JPEG: ~207 KB peak, well within the ESP32's ~300 KB usable heap. The source file is freed immediately after decode, dropping to ~150 KB for the viewing session.

The viewport drawing uses a single 320-pixel scanline buffer (640 bytes) on the stack — no additional heap allocation.

## What We Built

```
File                         Lines   Change
───────────────────────────  ─────   ──────────────────────────────
esp32/main/app_files.c        1106   Replaced simple tap-to-dismiss
                                     viewer with interactive zoom/pan
                                     viewer: decode-to-buffer, toolbar,
                                     drag-to-pan, center-preserving zoom
```

The image viewer went from 30 lines of display-and-wait to about 280 lines of interactive viewer. The decoders didn't change — the same row callback interface that streamed to the display now streams to a buffer. The complexity is all in the viewing loop: touch state machine, viewport math, and zoom/pan coordination.

The result: open any image on the SD card, see it on screen, tap [+] to zoom in, drag to explore, tap [−] to zoom out, tap Back to return to the file browser. A complete image viewing experience on a device with no GPU, no floating-point hardware, and a resistive touchscreen — just integer math, a pixel buffer, and careful state management.

---

**Next:** Chapter 42: The Notes App
