/*
 * ui.c — Touch UI for the SD card flasher
 *
 * Simple rectangle + text UI with touch hit-testing.
 * Consistent with Part 1's fb_pixel/fb_rect approach — no LVGL.
 */

#include "ui.h"
#include "display.h"
#include "touch.h"
#include "font.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Button geometry */
#define BTN_W      200
#define BTN_H      50
#define BTN_X      ((DISPLAY_WIDTH - BTN_W) / 2)
#define BTN_Y1     90
#define BTN_Y2     160

/* Progress bar geometry */
#define BAR_X      20
#define BAR_Y      150
#define BAR_W      (DISPLAY_WIDTH - 40)
#define BAR_H      24

static void draw_button(int x, int y, int w, int h, uint16_t bg, const char *label)
{
    display_fill_rect(x, y, w, h, bg);
    /* Center text in button */
    int text_len = 0;
    const char *p = label;
    while (*p++) text_len++;
    int tx = x + (w - text_len * FONT_WIDTH) / 2;
    int ty = y + (h - FONT_HEIGHT) / 2;
    display_string(tx, ty, label, COLOR_WHITE, bg);
}

static int hit_test(int tx, int ty, int bx, int by, int bw, int bh)
{
    return tx >= bx && tx < bx + bw && ty >= by && ty < by + bh;
}

void ui_show_splash(void)
{
    display_clear(COLOR_BLACK);

    /* Title */
    const char *title = "SURVIVAL WORKSTATION";
    int tx = (DISPLAY_WIDTH - 20 * FONT_WIDTH) / 2;
    display_string(tx, 40, title, COLOR_GREEN, COLOR_BLACK);

    /* Subtitle */
    const char *sub = "SD Card Flasher";
    int sx = (DISPLAY_WIDTH - 15 * FONT_WIDTH) / 2;
    display_string(sx, 70, sub, COLOR_CYAN, COLOR_BLACK);

    /* Version */
    display_string(sx, 100, "v1.0", COLOR_GRAY, COLOR_BLACK);

    /* Hardware */
    display_string(20, 200, "ESP32-2432S028R (CYD)", COLOR_DGRAY, COLOR_BLACK);
}

int ui_show_menu(int has_usb)
{
    display_clear(COLOR_BLACK);

    /* Header */
    const char *hdr = "Select architecture:";
    int hx = (DISPLAY_WIDTH - 20 * FONT_WIDTH) / 2;
    display_string(hx, 30, hdr, COLOR_WHITE, COLOR_BLACK);

    /* Buttons */
    draw_button(BTN_X, BTN_Y1, BTN_W, BTN_H, COLOR_BLUE, "Flash aarch64");
    draw_button(BTN_X, BTN_Y2, BTN_W, BTN_H, COLOR_BLUE, "Flash x86_64");

    if (has_usb) {
        display_string(20, 220, "USB Boot: ESP32-S3 only", COLOR_DGRAY, COLOR_BLACK);
    }

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

void ui_update_progress(const char *status, int current, int total)
{
    /* First call: clear and draw frame */
    if (current <= 0) {
        display_clear(COLOR_BLACK);
        display_string(20, 20, "Flashing SD card...", COLOR_WHITE, COLOR_BLACK);
    }

    /* Status text (clear previous line first) */
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
    display_string(px, BAR_Y + 4, pct, COLOR_WHITE, (p > 50) ? COLOR_GREEN : COLOR_DGRAY);

    /* Counter */
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d / %d", current, total);
    int cx = (DISPLAY_WIDTH - (int)strlen(cnt) * FONT_WIDTH) / 2;
    display_string(cx, BAR_Y + BAR_H + 8, cnt, COLOR_GRAY, COLOR_BLACK);
}

void ui_show_done(const char *arch)
{
    display_clear(COLOR_BLACK);

    display_string(60, 40, "Flash Complete!", COLOR_GREEN, COLOR_BLACK);

    char msg[64];
    snprintf(msg, sizeof(msg), "Arch: %s", arch);
    display_string(20, 80, msg, COLOR_WHITE, COLOR_BLACK);

    display_string(20, 120, "Remove SD card and boot", COLOR_CYAN, COLOR_BLACK);
    display_string(20, 140, "from any UEFI machine.", COLOR_CYAN, COLOR_BLACK);

    display_string(20, 200, "Tap to return to menu.", COLOR_GRAY, COLOR_BLACK);
}

void ui_show_error(const char *message)
{
    display_clear(COLOR_BLACK);

    display_string(100, 30, "ERROR", COLOR_RED, COLOR_BLACK);

    /* Draw message (handles newlines) */
    display_string(20, 80, message, COLOR_WHITE, COLOR_BLACK);

    display_string(20, 200, "Tap to return to menu.", COLOR_GRAY, COLOR_BLACK);
}

void ui_wait_for_tap(void)
{
    int x, y;
    touch_wait_tap(&x, &y);
}
