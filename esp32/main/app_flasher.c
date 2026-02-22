/*
 * app_flasher.c — Flash SD card app wrapper
 *
 * Default: flash x86_64 directly (confirmation screen only).
 * If show_aarch64 is enabled in settings, shows arch selection first.
 */

#include "app_flasher.h"
#include "flasher.h"
#include "sdcard.h"
#include "settings.h"
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "font.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Button geometry */
#define BTN_W      200
#define BTN_H      50
#define BTN_X      ((DISPLAY_WIDTH - BTN_W) / 2)
#define BTN_Y1     70
#define BTN_Y2     140

static void draw_button(int x, int y, int w, int h, uint16_t bg, const char *label)
{
    display_fill_rect(x, y, w, h, bg);
    int text_len = (int)strlen(label);
    int tx = x + (w - text_len * FONT_WIDTH) / 2;
    int ty = y + (h - FONT_HEIGHT) / 2;
    display_string(tx, ty, label, COLOR_WHITE, bg);
}

static int hit_test(int tx, int ty, int bx, int by, int bw, int bh)
{
    return tx >= bx && tx < bx + bw && ty >= by && ty < by + bh;
}

static void do_flash(const char *arch)
{
    /* Initialize SD card */
    if (sdcard_init() != 0) {
        ui_show_error("No SD card detected.\n"
                      "Insert a card and try again.");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    int result = flasher_run(arch);
    sdcard_deinit();

    if (result == 0) {
        ui_show_done(arch);
    } else {
        ui_show_error("Flash failed.\n"
                      "Check serial log for details.");
    }
    ui_wait_for_tap();
}

/* Show arch selection (aarch64 + x86_64 + back) */
static int show_arch_select(void)
{
    display_clear(COLOR_BLACK);

    ui_draw_back_button();

    const char *hdr = "Select architecture:";
    int hx = (DISPLAY_WIDTH - (int)strlen(hdr) * FONT_WIDTH) / 2;
    display_string(hx, 30, hdr, COLOR_WHITE, COLOR_BLACK);

    draw_button(BTN_X, BTN_Y1, BTN_W, BTN_H, COLOR_BLUE, "Flash aarch64");
    draw_button(BTN_X, BTN_Y2, BTN_W, BTN_H, COLOR_BLUE, "Flash x86_64");

    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);

        if (ui_check_back_button(tx, ty))
            return -1;
        if (hit_test(tx, ty, BTN_X, BTN_Y1, BTN_W, BTN_H)) {
            do_flash("aarch64");
            return 0;
        }
        if (hit_test(tx, ty, BTN_X, BTN_Y2, BTN_W, BTN_H)) {
            do_flash("x86_64");
            return 0;
        }
    }
}

/* Show x86_64 confirmation (single button + back) */
static int show_confirm(void)
{
    display_clear(COLOR_BLACK);

    ui_draw_back_button();

    const char *hdr = "Flash SD Card";
    int hx = (DISPLAY_WIDTH - (int)strlen(hdr) * FONT_WIDTH) / 2;
    display_string(hx, 30, hdr, COLOR_WHITE, COLOR_BLACK);

    display_string(60, 70, "Architecture: x86_64", COLOR_CYAN, COLOR_BLACK);
    display_string(40, 100, "Insert SD card and tap", COLOR_GRAY, COLOR_BLACK);
    display_string(40, 116, "the button below.", COLOR_GRAY, COLOR_BLACK);

    draw_button(BTN_X, BTN_Y2, BTN_W, BTN_H, COLOR_BLUE, "Flash x86_64");

    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);

        if (ui_check_back_button(tx, ty))
            return -1;
        if (hit_test(tx, ty, BTN_X, BTN_Y2, BTN_W, BTN_H)) {
            do_flash("x86_64");
            return 0;
        }
    }
}

void app_flasher_run(void)
{
    while (1) {
        int result;
        if (g_settings.show_aarch64) {
            result = show_arch_select();
        } else {
            result = show_confirm();
        }
        if (result < 0)
            return;  /* back button — return to home */
    }
}
