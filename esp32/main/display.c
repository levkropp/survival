/*
 * display.c — ILI9341 driver for the ESP32-2432S028R CYD board
 *
 * Uses esp_lcd panel API over SPI2_HOST. The CYD board wires:
 *   MOSI=13, MISO=12, CLK=14, CS=15, DC=2, Backlight=21
 *
 * Display is 320x240 in landscape mode (MADCTL rotation).
 */

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

    /* LCD panel driver */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,  /* CYD has no reset pin exposed */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));

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

void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

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

void display_draw_bitmap1bpp(int x, int y, int w, int h,
                              const uint8_t *bitmap, uint16_t fg, uint16_t bg)
{
    int row_bytes = (w + 7) / 8;
    uint16_t pixels[32];  /* max 32px wide icons */

    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= DISPLAY_HEIGHT) continue;

        const uint8_t *src = bitmap + row * row_bytes;
        for (int col = 0; col < w; col++) {
            int bit = src[col / 8] & (0x80 >> (col & 7));
            pixels[col] = bit ? fg : bg;
        }

        int dx = x;
        int pw = w;
        if (dx < 0) { /* clip left */ pw += dx; dx = 0; }
        if (dx + pw > DISPLAY_WIDTH) pw = DISPLAY_WIDTH - dx;
        if (pw > 0)
            esp_lcd_panel_draw_bitmap(panel, dx, dy, dx + pw, dy + 1,
                                      pixels + (dx - x));
    }
}

void display_draw_rgb565_line(int x, int y, int w, const uint16_t *pixels)
{
    if (y < 0 || y >= DISPLAY_HEIGHT || w <= 0) return;
    if (x < 0) { pixels -= x; w += x; x = 0; }
    if (x + w > DISPLAY_WIDTH) w = DISPLAY_WIDTH - x;
    if (w <= 0) return;

    esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + 1, pixels);
}

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
