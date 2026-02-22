/*
 * ui.c — Shared UI helpers for the Survival Workstation
 *
 * Simple rectangle + text UI with touch hit-testing.
 * The home screen and per-app UIs are in app.c and app_*.c.
 */

#include "ui.h"
#include "display.h"
#include "touch.h"
#include "font.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Progress bar geometry */
#define BAR_X      20
#define BAR_Y      150
#define BAR_W      (DISPLAY_WIDTH - 40)
#define BAR_H      24

/* Back button geometry */
#define BACK_X     0
#define BACK_Y     0
#define BACK_W     56
#define BACK_H     24

void ui_show_splash(void)
{
    display_clear(COLOR_BLACK);

    const char *title = "SURVIVAL WORKSTATION";
    int tx = (DISPLAY_WIDTH - 20 * FONT_WIDTH) / 2;
    display_string(tx, 40, title, COLOR_GREEN, COLOR_BLACK);

    const char *sub = "Survival Toolkit";
    int sx = (DISPLAY_WIDTH - (int)strlen(sub) * FONT_WIDTH) / 2;
    display_string(sx, 70, sub, COLOR_CYAN, COLOR_BLACK);

    display_string(sx, 100, "v2.0", COLOR_GRAY, COLOR_BLACK);

    display_string(20, 200, "ESP32-2432S028R (CYD)", COLOR_DGRAY, COLOR_BLACK);
}

void ui_update_progress(const char *status, int current, int total)
{
    if (current <= 0) {
        display_clear(COLOR_BLACK);
        display_string(20, 20, "Flashing SD card...", COLOR_WHITE, COLOR_BLACK);
    }

    display_fill_rect(20, 100, DISPLAY_WIDTH - 40, FONT_HEIGHT, COLOR_BLACK);
    display_string(20, 100, status, COLOR_CYAN, COLOR_BLACK);

    display_fill_rect(BAR_X, BAR_Y, BAR_W, BAR_H, COLOR_DGRAY);

    if (total > 0 && current > 0) {
        int fill = (BAR_W * current) / total;
        if (fill > BAR_W) fill = BAR_W;
        display_fill_rect(BAR_X, BAR_Y, fill, BAR_H, COLOR_GREEN);
    }

    char pct[16];
    int p = (total > 0) ? (current * 100 / total) : 0;
    snprintf(pct, sizeof(pct), "%d%%", p);
    int px = (DISPLAY_WIDTH - (int)strlen(pct) * FONT_WIDTH) / 2;
    display_string(px, BAR_Y + 4, pct, COLOR_WHITE, (p > 50) ? COLOR_GREEN : COLOR_DGRAY);

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

    display_string(20, 200, "Tap to continue.", COLOR_GRAY, COLOR_BLACK);
}

void ui_show_error(const char *message)
{
    display_clear(COLOR_BLACK);

    display_string(100, 30, "ERROR", COLOR_RED, COLOR_BLACK);

    display_string(20, 80, message, COLOR_WHITE, COLOR_BLACK);

    display_string(20, 200, "Tap to continue.", COLOR_GRAY, COLOR_BLACK);
}

void ui_wait_for_tap(void)
{
    int x, y;
    touch_wait_tap(&x, &y);
}

void ui_draw_back_button(void)
{
    display_fill_rect(BACK_X, BACK_Y, BACK_W, BACK_H, COLOR_DGRAY);
    display_string(BACK_X + 4, BACK_Y + 4, "< Back", COLOR_WHITE, COLOR_DGRAY);
}

int ui_check_back_button(int tx, int ty)
{
    return tx >= BACK_X && tx < BACK_X + BACK_W &&
           ty >= BACK_Y && ty < BACK_Y + BACK_H;
}
