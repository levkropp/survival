---
layout: default
title: "Chapter 29: Driving the Display"
parent: "Phase 1: The Pocket Flasher"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 3
---

# Chapter 29: Driving the Display

## Two Buses, Three Devices

The CYD board has three SPI peripherals: the ILI9341 display, the XPT2046 touchscreen, and the SD card slot. The ESP32 has three SPI controllers: SPI1 (reserved for the internal flash chip), SPI2 (called HSPI), and SPI3 (called VSPI). Two available controllers, three devices.

The display gets SPI2 — it needs high bandwidth (40 MHz) for fast screen updates. The SD card gets SPI3 — it needs reliable transfers for writing sectors. The touchscreen gets neither. Instead, we bit-bang SPI in software by toggling GPIO pins manually. At the ~50 Hz polling rate a touch controller needs, bit-banging is perfectly adequate.

```
ESP32 SPI Controllers:
┌─────────┬──────────────────────────┐
│  SPI1   │ Internal flash (reserved)│
├─────────┼──────────────────────────┤
│  SPI2   │ ILI9341 display          │
│ (HSPI)  │ MOSI=13, CLK=14, CS=15  │
├─────────┼──────────────────────────┤
│  SPI3   │ SD card                  │
│ (VSPI)  │ MOSI=23, CLK=18, CS=5   │
├─────────┼──────────────────────────┤
│ Bit-bang│ XPT2046 touch            │
│ (GPIO)  │ MOSI=32, CLK=25, CS=33  │
└─────────┴──────────────────────────┘
```

## Initializing the Display

The display driver lives in `display.c`. Let's walk through initialization:

```c
#include "display.h"
#include "font.h"

#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "display";

/* CYD display pin assignments */
#define PIN_MOSI  13
#define PIN_MISO  12
#define PIN_CLK   14
#define PIN_CS    15
#define PIN_DC    2
#define PIN_BL    21

#define SPI_CLOCK_HZ  (40 * 1000 * 1000)  /* 40MHz — ILI9341 max */

static esp_lcd_panel_handle_t panel = NULL;
```

The pin numbers are fixed by the CYD board's PCB layout — they cannot be changed. `PIN_DC` (data/command) is the signal that tells the ILI9341 whether an incoming byte is a command (like "set column address") or pixel data. `PIN_BL` controls the backlight LED.

40 MHz is the ILI9341's maximum SPI clock speed. At this rate, pushing a full 320x240 frame of 16-bit pixels takes about 30 ms — fast enough for smooth UI updates.

```c
/* Send a filled rectangle of one color to the display */
static uint16_t line_buf[DISPLAY_WIDTH];  /* one scanline buffer */

void display_init(void)
{
    ESP_LOGI(TAG, "Initializing ILI9341 on SPI2");

    /* Backlight on */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_BL, 1);
```

Backlight first, so the screen lights up as soon as we draw something. `1ULL << PIN_BL` creates a 64-bit bitmask with bit 21 set — ESP-IDF's GPIO configuration uses bitmasks so you can configure multiple pins in one call. We only need one.

```c
    /* SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
```

`spi_bus_initialize` configures the physical SPI2 hardware: which GPIO pins carry MOSI (master out, slave in), MISO (master in, slave out), and SCLK (clock). The `-1` values for `quadwp` and `quadhd` mean we're not using quad-SPI mode — standard single-bit SPI is enough for the ILI9341.

`max_transfer_sz` sets the largest single DMA transfer. One scanline of 320 pixels at 16 bits each is 640 bytes. We never transfer more than one line at a time, so this is our maximum.

`SPI_DMA_CH_AUTO` lets ESP-IDF pick a DMA channel automatically. DMA (Direct Memory Access) allows the SPI peripheral to send data without CPU involvement — the CPU sets up the transfer and the DMA engine handles the actual byte-by-byte clocking. Without DMA, the CPU would be stuck in a loop pushing bytes, unable to do anything else.

`ESP_ERROR_CHECK` is ESP-IDF's assert-on-failure macro. If `spi_bus_initialize` returns an error, the firmware halts and prints a diagnostic. This is appropriate during initialization — if the SPI bus can't be set up, nothing works.

```c
    /* LCD panel IO (SPI) */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = SPI_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));
```

The "panel IO" layer is ESP-IDF's abstraction over the SPI transport for LCD panels. It handles the DC pin toggling (pulling DC low for commands, high for data) and manages a queue of SPI transactions. The `trans_queue_depth` of 10 means up to 10 SPI transfers can be queued before blocking — this allows the CPU to prepare the next line of pixels while the DMA engine sends the current one.

`spi_mode = 0` means CPOL=0, CPHA=0: clock idles low, data sampled on the rising edge. This is the ILI9341's native SPI mode.

```c
    /* LCD panel driver */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,  /* CYD has no reset pin exposed */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
```

Here's a subtlety that might puzzle you: we're using `esp_lcd_new_panel_st7789` for an ILI9341 display. The ST7789 and ILI9341 are register-compatible for basic operations — initialization sequences, pixel writes, rotation commands all use the same register addresses. ESP-IDF provides a built-in ST7789 driver but not an ILI9341 driver, and the ST7789 driver works perfectly for our needs.

`reset_gpio_num = -1` means no hardware reset pin. The CYD board doesn't route the ILI9341's reset pin to a GPIO — a software reset command is sent instead during `esp_lcd_panel_reset()`.

`LCD_RGB_ELEMENT_ORDER_BGR` tells the driver that the display expects blue-green-red byte order within each 16-bit pixel. This is a hardware characteristic of the ILI9341 panel on the CYD board.

```c
    /* Init sequence */
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);

    /* Landscape: swap X/Y, no mirror */
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, false);

    /* Display on */
    esp_lcd_panel_disp_on_off(panel, true);

    display_clear(COLOR_BLACK);
    ESP_LOGI(TAG, "Display ready: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}
```

## The Landscape Problem

The ILI9341's native orientation is portrait: 240 pixels wide, 320 pixels tall. That's a phone layout. We want landscape: 320 wide, 240 tall. This gives us 40 columns of 8-pixel-wide characters instead of 30 — much better for showing file paths and status messages.

LCD controllers handle rotation through a register called MADCTL (Memory Access Data Control). It controls three transformations: swap X/Y axes, mirror X, and mirror Y. The correct combination for landscape on the CYD is:

```c
esp_lcd_panel_swap_xy(panel, true);
esp_lcd_panel_mirror(panel, false, false);
```

Swap X/Y rotates the coordinate system 90 degrees. No mirroring is needed because the CYD's panel is oriented so that the swap alone produces the correct left-to-right, top-to-bottom layout.

Getting this wrong produces confusing results: the image might appear rotated 90 degrees, mirrored, or upside-down. There are eight possible combinations of swap + mirror X + mirror Y, and only one is correct for a given board. Trial and error is the standard debugging approach — LCD datasheets rarely tell you which combination to use because it depends on how the panel is physically mounted on the PCB.

## RGB565 Color

In Part 1, the framebuffer used 32-bit BGRX pixels — one byte each for blue, green, red, and a reserved byte. The ILI9341 uses **RGB565** — a 16-bit format that packs three color channels into two bytes:

```
Bit layout of one RGB565 pixel:
15  14  13  12  11  10  9   8   7   6   5   4   3   2   1   0
R4  R3  R2  R1  R0  G5  G4  G3  G2  G1  G0  B4  B3  B2  B1  B0
└──── Red (5 bits) ────┘└──── Green (6 bits) ────┘└── Blue (5 bits) ─┘
```

Red gets 5 bits (32 levels), green gets 6 bits (64 levels), and blue gets 5 bits (32 levels). Green gets extra precision because human eyes are most sensitive to green. Total: 65,536 possible colors.

The conversion from 8-bit-per-channel RGB to RGB565:

```c
static inline uint16_t display_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
```

`r & 0xF8` keeps the top 5 bits of red. `<< 8` shifts them to bit positions 15-11. `g & 0xFC` keeps the top 6 bits of green. `<< 3` shifts them to bit positions 10-5. `b >> 3` keeps the top 5 bits of blue at positions 4-0.

Predefined color constants save us from computing this every time:

```c
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_GRAY    0x7BEF
#define COLOR_DGRAY   0x3186
```

These are the same logical colors as Part 1's BGRX constants, just encoded differently.

## Drawing Primitives

### Clearing the Screen

```c
void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}
```

Clear is just a full-screen rectangle fill.

### Filling Rectangles

```c
void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH)  w = DISPLAY_WIDTH - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    /* Fill line buffer with color */
    for (int i = 0; i < w; i++)
        line_buf[i] = color;

    /* Draw line by line */
    for (int row = y; row < y + h; row++)
        esp_lcd_panel_draw_bitmap(panel, x, row, x + w, row + 1, line_buf);
}
```

The clipping logic at the top handles rectangles that extend past the screen edges — the same defensive approach as Part 1's `fb_rect`. Negative coordinates, oversized widths, zero-area rectangles are all caught before any pixels are sent to the display.

The drawing strategy is different from Part 1. We don't have a framebuffer we can write to directly. Instead, we use `esp_lcd_panel_draw_bitmap`, which sends a rectangular block of pixels to the display over SPI. We fill a one-scanline buffer (`line_buf`) with the desired color, then send it once per row. The SPI + DMA hardware handles the actual transfer.

Why not send the entire rectangle at once? Memory. A full-screen bitmap at 320x240x2 bytes would be 150 KB — almost all our available RAM. A single-line buffer is 640 bytes. We trade speed for memory: line-by-line drawing is slightly slower than bulk transfer, but uses a negligible amount of RAM.

### Rendering Characters

```c
void display_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = ' ';
    const uint8_t *glyph = font_data[c - FONT_FIRST];

    uint16_t pixels[FONT_WIDTH];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++)
            pixels[col] = (bits & (0x80 >> col)) ? fg : bg;

        int dy = y + row;
        if (dy >= 0 && dy < DISPLAY_HEIGHT && x >= 0 && x + FONT_WIDTH <= DISPLAY_WIDTH)
            esp_lcd_panel_draw_bitmap(panel, x, dy, x + FONT_WIDTH, dy + 1, pixels);
    }
}
```

Same font, same rendering logic as Part 1's `fb_char` (Chapter 6). The glyph lookup, the bit test with `0x80 >> col`, the row-by-row rendering — all identical. The only difference is the output: instead of writing to a framebuffer pointer, we send 8 pixels per row to the display via `esp_lcd_panel_draw_bitmap`.

An 8-pixel buffer (`pixels[FONT_WIDTH]`) holds one row of the character. That's 16 bytes. We send 16 SPI transactions per character (one per font row). At 40 MHz SPI, each transaction takes microseconds. Drawing a full screen of 40×15 = 600 characters takes about 100 ms — not instant, but responsive enough for a UI that redraws on state changes, not every frame.

### Drawing Strings

```c
void display_string(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    int cx = x, cy = y;
    while (*s) {
        if (*s == '\n') {
            cx = x;
            cy += FONT_HEIGHT;
            s++;
            continue;
        }
        if (cx + FONT_WIDTH > DISPLAY_WIDTH) {
            cx = 0;
            cy += FONT_HEIGHT;
        }
        if (cy + FONT_HEIGHT > DISPLAY_HEIGHT) break;

        display_char(cx, cy, *s, fg, bg);
        cx += FONT_WIDTH;
        s++;
    }
}
```

Handles newlines and wrapping, just like Part 1's `fb_string`. The starting x position resets to the left margin on `\n` — note it resets to `x` (the original starting x), not to 0. This lets us draw indented multi-line strings that maintain their indentation.

## The Font

The font is the same 8x16 VGA bitmap font from Part 1 (Chapter 6), stored identically in `font.c` and declared in `font.h`:

```c
#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#define FONT_FIRST  32  /* space */
#define FONT_LAST   126 /* tilde */
#define FONT_CHARS  (FONT_LAST - FONT_FIRST + 1)

extern const uint8_t font_data[FONT_CHARS][FONT_HEIGHT];
```

95 printable ASCII characters, 16 bytes each, 1,520 bytes total. The data is a straight copy from Part 1 — the same glyphs render the same way on the ILI9341 as they do on a UEFI framebuffer. The only difference is the color encoding: RGB565 instead of BGRX.

## The Complete Display Header

```c
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240

void display_init(void);
void display_clear(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_char(int x, int y, char c, uint16_t fg, uint16_t bg);
void display_string(int x, int y, const char *s, uint16_t fg, uint16_t bg);

static inline uint16_t display_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

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
```

Five functions and ten constants. The interface is narrower than Part 1's framebuffer module because we don't need scrolling, cursor tracking, or the `fb_print` workhorse — the flasher UI is static screens, not a scrolling terminal.

## Touch Input

The XPT2046 touchscreen controller sits on a separate SPI connection that we bit-bang in software. The pin assignments are:

```c
/* Pin assignments */
#define PIN_MOSI  32
#define PIN_MISO  39
#define PIN_CLK   25
#define PIN_CS    33
#define PIN_IRQ   36
```

Why bit-bang instead of hardware SPI? The ESP32 has only two available SPI controllers, and both are taken (display on SPI2, SD card on SPI3). The touchscreen's bandwidth requirements are minimal — we read coordinates maybe 50 times per second, and each read is a few bytes. Bit-banging at GPIO speeds is fast enough.

### GPIO Setup

```c
void touch_init(void)
{
    /* Output pins: MOSI, CLK, CS */
    uint64_t out_mask = (1ULL << PIN_MOSI) | (1ULL << PIN_CLK) | (1ULL << PIN_CS);
    gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&out_cfg);

    /* Input pins: MISO, IRQ */
    uint64_t in_mask = (1ULL << PIN_MISO) | (1ULL << PIN_IRQ);
    gpio_config_t in_cfg = {
        .pin_bit_mask = in_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&in_cfg);

    /* CS high (deselect) */
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_CLK, 0);
}
```

Three output pins (MOSI, CLK, CS) and two input pins (MISO, IRQ). The IRQ pin goes low when the screen is touched — we check it before starting a read to avoid wasting time polling an untouched screen.

### Bit-Banged SPI Transfer

```c
static uint8_t spi_transfer(uint8_t data)
{
    uint8_t result = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_MOSI, (data >> i) & 1);
        ets_delay_us(1);
        gpio_set_level(PIN_CLK, 1);
        ets_delay_us(1);
        result = (result << 1) | gpio_get_level(PIN_MISO);
        gpio_set_level(PIN_CLK, 0);
        ets_delay_us(1);
    }
    return result;
}
```

SPI mode 0: data is set on MOSI while clock is low, then clock goes high and the slave latches the data. Simultaneously, we read MISO on the rising edge. The `ets_delay_us(1)` calls ensure timing compliance — the XPT2046 needs at least 200 ns between transitions.

This is the same SPI protocol that the hardware controllers implement, just done manually in software. Eight iterations of the loop, one per bit, MSB first. Each call transfers one byte in each direction.

### Reading Coordinates

```c
static uint16_t xpt2046_read(uint8_t cmd)
{
    spi_transfer(cmd);
    uint8_t hi = spi_transfer(0x00);
    uint8_t lo = spi_transfer(0x00);
    return ((uint16_t)hi << 5) | (lo >> 3);  /* 12-bit, top-aligned */
}
```

The XPT2046 protocol: send a command byte, then read two bytes of response. The 12-bit ADC result is packed across both bytes — the top 7 bits in the first byte and the bottom 5 bits in the second. The shift-and-combine produces a value from 0 to 4095.

Two commands matter: `0xD0` reads the X position, `0x90` reads the Y position. Both request differential measurement mode with 12-bit resolution.

```c
static bool read_raw(uint16_t *raw_x, uint16_t *raw_y)
{
    if (gpio_get_level(PIN_IRQ) == 1)
        return false;

    gpio_set_level(PIN_CS, 0);
    ets_delay_us(5);

    /* Take 4 samples and average */
    uint32_t sum_x = 0, sum_y = 0;
    int valid = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t x = xpt2046_read(CMD_READ_X);
        uint16_t y = xpt2046_read(CMD_READ_Y);
        if (x > TOUCH_THRESHOLD && y > TOUCH_THRESHOLD) {
            sum_x += x;
            sum_y += y;
            valid++;
        }
    }

    gpio_set_level(PIN_CS, 1);

    if (valid == 0) return false;

    *raw_x = (uint16_t)(sum_x / valid);
    *raw_y = (uint16_t)(sum_y / valid);
    return true;
}
```

Four samples averaged for stability. Resistive touchscreens are noisy — a single reading might jitter by 50+ ADC counts. Averaging four samples smooths this to a consistent position. The threshold check (`TOUCH_THRESHOLD = 100`) rejects readings near zero, which indicate no real touch contact.

### Calibration and Coordinate Mapping

```c
bool touch_read(int *x, int *y)
{
    uint16_t raw_x, raw_y;
    if (!read_raw(&raw_x, &raw_y))
        return false;

    /* CYD landscape: touch X maps to screen Y, touch Y maps to screen X (inverted) */
    *x = map_coord(raw_y, CAL_Y_MIN, CAL_Y_MAX, DISPLAY_WIDTH - 1);
    *x = (DISPLAY_WIDTH - 1) - *x;  /* invert */
    *y = map_coord(raw_x, CAL_X_MIN, CAL_X_MAX, DISPLAY_HEIGHT - 1);

    return true;
}
```

This is where the CYD's physical layout creates confusion. The touch panel's X axis runs along the display's Y axis, and vice versa — because the touch controller is oriented for portrait mode while we're using landscape. Raw touch X maps to screen Y, and raw touch Y maps to screen X (inverted).

The calibration constants (`CAL_X_MIN=200`, `CAL_X_MAX=3900`) define the usable range of ADC values. The edges of the touch panel produce values near 0 and 4095, but the first and last 200 counts are unreliable. `map_coord` linearly interpolates within the calibrated range:

```c
static int map_coord(uint16_t raw, int raw_min, int raw_max, int screen_max)
{
    if (raw < raw_min) raw = raw_min;
    if (raw > raw_max) raw = raw_max;
    return (int)((uint32_t)(raw - raw_min) * screen_max / (raw_max - raw_min));
}
```

Clamping before the multiplication prevents underflow (if `raw < raw_min`) and overflow (if `raw > raw_max`). The cast to `uint32_t` prevents overflow in the multiplication — `3700 * 319` fits in 32 bits but not 16.

### Waiting for a Tap

```c
void touch_wait_tap(int *x, int *y)
{
    /* Wait for finger down */
    while (!touch_read(x, y))
        vTaskDelay(pdMS_TO_TICKS(20));

    /* Wait for finger up (debounce) */
    int dummy_x, dummy_y;
    while (touch_read(&dummy_x, &dummy_y))
        vTaskDelay(pdMS_TO_TICKS(20));
}
```

A tap is a finger-down followed by a finger-up. We block until both events occur. The 20 ms delay between polls gives FreeRTOS time to run its idle task (which feeds the watchdog timer) and keeps CPU usage low. At 50 Hz polling, we detect taps within 40 ms — imperceptible to a human.

The complete touch header:

```c
#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>
#include <stdint.h>

void touch_init(void);
bool touch_read(int *x, int *y);
void touch_wait_tap(int *x, int *y);

#endif /* TOUCH_H */
```

Three functions. Initialize, read current state, wait for a tap. That's the entire touch interface the flasher needs.

## The UI Layer

With display and touch working, we can build the user interface. The UI module (`ui.c`) handles four screens: splash, menu, progress, and done/error.

### Splash Screen

```c
void ui_show_splash(void)
{
    display_clear(COLOR_BLACK);

    const char *title = "SURVIVAL WORKSTATION";
    int tx = (DISPLAY_WIDTH - 20 * FONT_WIDTH) / 2;
    display_string(tx, 40, title, COLOR_GREEN, COLOR_BLACK);

    const char *sub = "SD Card Flasher";
    int sx = (DISPLAY_WIDTH - 15 * FONT_WIDTH) / 2;
    display_string(sx, 70, sub, COLOR_CYAN, COLOR_BLACK);

    display_string(sx, 100, "v1.0", COLOR_GRAY, COLOR_BLACK);
    display_string(20, 200, "ESP32-2432S028R (CYD)", COLOR_DGRAY, COLOR_BLACK);
}
```

Centered text, color-coded: green title, cyan subtitle, gray version, dark gray hardware ID. The centering math is simple: `(screen_width - string_length * char_width) / 2`. No layout engine, no fonts, no CSS — just arithmetic.

### Menu with Buttons

```c
#define BTN_W      200
#define BTN_H      50
#define BTN_X      ((DISPLAY_WIDTH - BTN_W) / 2)
#define BTN_Y1     90
#define BTN_Y2     160

static void draw_button(int x, int y, int w, int h, uint16_t bg, const char *label)
{
    display_fill_rect(x, y, w, h, bg);
    int text_len = 0;
    const char *p = label;
    while (*p++) text_len++;
    int tx = x + (w - text_len * FONT_WIDTH) / 2;
    int ty = y + (h - FONT_HEIGHT) / 2;
    display_string(tx, ty, label, COLOR_WHITE, bg);
}
```

A button is a colored rectangle with centered text. Two buttons: "Flash aarch64" and "Flash x86_64", stacked vertically.

```c
int ui_show_menu(int has_usb)
{
    display_clear(COLOR_BLACK);

    const char *hdr = "Select architecture:";
    int hx = (DISPLAY_WIDTH - 20 * FONT_WIDTH) / 2;
    display_string(hx, 30, hdr, COLOR_WHITE, COLOR_BLACK);

    draw_button(BTN_X, BTN_Y1, BTN_W, BTN_H, COLOR_BLUE, "Flash aarch64");
    draw_button(BTN_X, BTN_Y2, BTN_W, BTN_H, COLOR_BLUE, "Flash x86_64");

    /* Wait for touch on a button */
    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);

        if (hit_test(tx, ty, BTN_X, BTN_Y1, BTN_W, BTN_H))
            return UI_CHOICE_FLASH_AARCH64;
        if (hit_test(tx, ty, BTN_X, BTN_Y2, BTN_W, BTN_H))
            return UI_CHOICE_FLASH_X86_64;
    }
}
```

The menu blocks until a valid button tap. Taps outside both buttons are ignored — the loop just waits for the next tap. `hit_test` is a simple bounds check:

```c
static int hit_test(int tx, int ty, int bx, int by, int bw, int bh)
{
    return tx >= bx && tx < bx + bw && ty >= by && ty < by + bh;
}
```

### Progress Display

```c
void ui_update_progress(const char *status, int current, int total)
{
    if (current <= 0) {
        display_clear(COLOR_BLACK);
        display_string(20, 20, "Flashing SD card...", COLOR_WHITE, COLOR_BLACK);
    }

    /* Status text */
    display_fill_rect(20, 100, DISPLAY_WIDTH - 40, FONT_HEIGHT, COLOR_BLACK);
    display_string(20, 100, status, COLOR_CYAN, COLOR_BLACK);

    /* Progress bar */
    display_fill_rect(BAR_X, BAR_Y, BAR_W, BAR_H, COLOR_DGRAY);

    if (total > 0 && current > 0) {
        int fill = (BAR_W * current) / total;
        if (fill > BAR_W) fill = BAR_W;
        display_fill_rect(BAR_X, BAR_Y, fill, BAR_H, COLOR_GREEN);
    }

    /* Percentage */
    char pct[16];
    int p = (total > 0) ? (current * 100 / total) : 0;
    snprintf(pct, sizeof(pct), "%d%%", p);
    int px = (DISPLAY_WIDTH - (int)strlen(pct) * FONT_WIDTH) / 2;
    display_string(px, BAR_Y + 4, pct, COLOR_WHITE,
                   (p > 50) ? COLOR_GREEN : COLOR_DGRAY);
}
```

The progress bar is a dark gray rectangle with a green fill that grows from left to right. The percentage text is drawn on top of the bar — its background color switches from dark gray to green at 50% so it stays readable against both halves. The status text above the bar shows what's currently happening: "Creating partition table...", "Formatting FAT32...", "Writing: EFI/BOOT/BOOTAA64.EFI".

The first call (`current <= 0`) clears the screen and draws the header. Subsequent calls only update the status text and bar, avoiding a full screen redraw per step. This keeps the UI responsive — updating just the bar and text takes about 5 ms, versus 30 ms for a full clear.

## What We Have

Two modules, 325 lines of C plus headers:

```
display.c   157 lines   ILI9341 driver: init, fill, char, string
display.h    45 lines   Interface + color constants
touch.c     168 lines   XPT2046 driver: init, read, wait_tap
touch.h      21 lines   Interface
font.c      202 lines   8x16 VGA font data (shared with Part 1)
font.h       20 lines   Font constants
ui.c        166 lines   Splash, menu, progress, done, error screens
ui.h         32 lines   UI interface
```

The display draws text and rectangles. The touchscreen reads tap coordinates. The UI module combines them into a usable interface with buttons, a progress bar, and status messages. No graphics library, no widget toolkit — just rectangles and bitmap characters, same as Part 1.

---

**Next:** [Chapter 30: Talking to the SD Card](chapter-30-talking-to-the-sd-card)
