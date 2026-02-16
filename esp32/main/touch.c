/*
 * touch.c — XPT2046 resistive touchscreen for ESP32 CYD
 *
 * Bit-banged SPI since both hardware SPI hosts are taken (display + SD).
 * The XPT2046 only needs ~50Hz polling, so bit-bang is fine.
 *
 * CYD pin assignments:
 *   MOSI=32, MISO=39, CLK=25, CS=33, IRQ=36
 *
 * Calibration: raw ADC range ~200..3900 maps to screen 0..319 / 0..239.
 * The CYD's touch panel is mounted with X/Y swapped relative to the
 * ILI9341 in landscape mode.
 */

#include "touch.h"
#include "display.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

static const char *TAG = "touch";

/* Pin assignments */
#define PIN_MOSI  32
#define PIN_MISO  39
#define PIN_CLK   25
#define PIN_CS    33
#define PIN_IRQ   36

/* XPT2046 command bytes */
#define CMD_READ_X  0xD0  /* differential, 12-bit, X position */
#define CMD_READ_Y  0x90  /* differential, 12-bit, Y position */

/* Calibration constants (raw ADC range → screen coords).
 * These are typical for the CYD 2.8" panel; adjust if needed. */
#define CAL_X_MIN  200
#define CAL_X_MAX  3900
#define CAL_Y_MIN  200
#define CAL_Y_MAX  3900

/* Pressure threshold — readings below this are noise */
#define TOUCH_THRESHOLD 100

void touch_init(void)
{
    ESP_LOGI(TAG, "Initializing XPT2046 touch (bit-bang SPI)");

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

    ESP_LOGI(TAG, "Touch ready");
}

/* Bit-bang one byte out, read one byte back (SPI mode 0: CPOL=0, CPHA=0) */
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

/* Send XPT2046 command, read 12-bit result */
static uint16_t xpt2046_read(uint8_t cmd)
{
    spi_transfer(cmd);
    uint8_t hi = spi_transfer(0x00);
    uint8_t lo = spi_transfer(0x00);
    return ((uint16_t)hi << 5) | (lo >> 3);  /* 12-bit, top-aligned in 16-bit response */
}

/* Read raw ADC values, averaging multiple samples for stability */
static bool read_raw(uint16_t *raw_x, uint16_t *raw_y)
{
    /* Check IRQ pin first — active low when touched */
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

/* Map raw ADC to screen coordinates with calibration + clamping */
static int map_coord(uint16_t raw, int raw_min, int raw_max, int screen_max)
{
    if (raw < raw_min) raw = raw_min;
    if (raw > raw_max) raw = raw_max;
    return (int)((uint32_t)(raw - raw_min) * screen_max / (raw_max - raw_min));
}

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
