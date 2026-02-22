/*
 * app_settings.c — Settings app
 *
 * Shows device info and advanced options with a toggle for
 * showing aarch64 in the Flash app.
 */

#include "app_settings.h"
#include "settings.h"
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "font.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"

/* Toggle button geometry */
#define TOGGLE_X    20
#define TOGGLE_Y    168
#define TOGGLE_W    280
#define TOGGLE_H    32

/* Advanced Options button */
#define ADV_X       20
#define ADV_Y       140
#define ADV_W       280
#define ADV_H       28

static void draw_settings_main(void)
{
    display_clear(COLOR_BLACK);
    ui_draw_back_button();

    display_string(100, 4, "Settings", COLOR_WHITE, COLOR_BLACK);

    /* Device info section */
    display_string(20, 36, "Device Info", COLOR_GREEN, COLOR_BLACK);

    esp_chip_info_t info;
    esp_chip_info(&info);

    const char *chip = "ESP32";
    if (info.model == CHIP_ESP32S3) chip = "ESP32-S3";
    else if (info.model == CHIP_ESP32S2) chip = "ESP32-S2";
    else if (info.model == CHIP_ESP32C3) chip = "ESP32-C3";

    char line[48];
    snprintf(line, sizeof(line), "%s  %d cores  4MB flash", chip, info.cores);
    display_string(28, 60, line, COLOR_GRAY, COLOR_BLACK);

    display_string(28, 80, "Board: 2432S028R (CYD)", COLOR_GRAY, COLOR_BLACK);
    display_string(28, 100, "Display: 320x240 ILI9341", COLOR_GRAY, COLOR_BLACK);

    /* Advanced Options button */
    display_fill_rect(ADV_X, ADV_Y, ADV_W, ADV_H, COLOR_DGRAY);
    display_string(ADV_X + 8, ADV_Y + 6, "Advanced Options", COLOR_WHITE, COLOR_DGRAY);
    display_string(ADV_X + ADV_W - 24, ADV_Y + 6, ">", COLOR_WHITE, COLOR_DGRAY);
}

static void draw_checkbox(int x, int y, int w, int h, int checked, const char *label)
{
    display_fill_rect(x, y, w, h, COLOR_DGRAY);

    /* Checkbox box */
    const char *box = checked ? "[x]" : "[ ]";
    display_string(x + 8, y + 8, box, COLOR_CYAN, COLOR_DGRAY);

    /* Label */
    display_string(x + 40, y + 8, label, COLOR_WHITE, COLOR_DGRAY);
}

static void draw_advanced(void)
{
    display_clear(COLOR_BLACK);
    ui_draw_back_button();

    display_string(60, 4, "Advanced Options", COLOR_WHITE, COLOR_BLACK);

    display_string(20, 40, "Flash App", COLOR_GREEN, COLOR_BLACK);
    display_string(28, 64, "By default, Flash SD", COLOR_GRAY, COLOR_BLACK);
    display_string(28, 80, "writes x86_64 images.", COLOR_GRAY, COLOR_BLACK);
    display_string(28, 100, "Enable this to also", COLOR_GRAY, COLOR_BLACK);
    display_string(28, 116, "show the aarch64 option.", COLOR_GRAY, COLOR_BLACK);

    draw_checkbox(TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H,
                  g_settings.show_aarch64, "Show aarch64 in Flash");
}

static int hit_test(int tx, int ty, int bx, int by, int bw, int bh)
{
    return tx >= bx && tx < bx + bw && ty >= by && ty < by + bh;
}

static void show_advanced(void)
{
    draw_advanced();

    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);

        if (ui_check_back_button(tx, ty))
            return;

        if (hit_test(tx, ty, TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H)) {
            g_settings.show_aarch64 = !g_settings.show_aarch64;
            settings_save();
            draw_checkbox(TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H,
                          g_settings.show_aarch64, "Show aarch64 in Flash");
        }
    }
}

void app_settings_run(void)
{
    draw_settings_main();

    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);

        if (ui_check_back_button(tx, ty))
            return;

        if (hit_test(tx, ty, ADV_X, ADV_Y, ADV_W, ADV_H)) {
            show_advanced();
            draw_settings_main();
        }
    }
}
