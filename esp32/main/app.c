/*
 * app.c — Home screen rendering for the Survival Workstation
 *
 * Draws a 2×3 icon grid with labels and waits for touch input.
 * Layout: 320×240 screen
 *   y=0..23:    title bar ("SURVIVAL WORKSTATION")
 *   y=32..215:  2 rows × 3 columns of icon cells (106×92px each)
 *   y=220..239: version footer
 */

#include "app.h"
#include "display.h"
#include "icons.h"
#include "touch.h"
#include "font.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Grid layout constants */
#define GRID_COLS   3
#define GRID_ROWS   2
#define GRID_X      1           /* left margin */
#define GRID_Y      32          /* top of grid area */
#define CELL_W      106         /* 320 / 3 = 106.6, use 106 */
#define CELL_H      92          /* (216 - 32) / 2 = 92 */

/* Title bar */
#define TITLE_Y     4

/* Footer */
#define FOOTER_Y    222

static void draw_home_screen(void)
{
    display_clear(COLOR_BLACK);

    /* Title: "SURVIVAL WORKSTATION" */
    const char *title = "SURVIVAL WORKSTATION";
    int title_len = (int)strlen(title);
    int tx = (DISPLAY_WIDTH - title_len * FONT_WIDTH) / 2;
    display_string(tx, TITLE_Y, title, COLOR_GREEN, COLOR_BLACK);

    /* Draw each app cell */
    for (int i = 0; i < APP_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        int cx = GRID_X + col * CELL_W;
        int cy = GRID_Y + row * CELL_H;

        /* Center 32×32 icon in cell */
        int icon_x = cx + (CELL_W - ICON_W) / 2;
        int icon_y = cy + 12;

        display_draw_bitmap1bpp(icon_x, icon_y, ICON_W, ICON_H,
                                 g_apps[i].icon,
                                 g_apps[i].icon_color, COLOR_BLACK);

        /* Label below icon */
        int label_len = (int)strlen(g_apps[i].name);
        int lx = cx + (CELL_W - label_len * FONT_WIDTH) / 2;
        int ly = icon_y + ICON_H + 8;
        display_string(lx, ly, g_apps[i].name, COLOR_WHITE, COLOR_BLACK);
    }

    /* Footer */
    const char *ver = "v2.0";
    int vx = (DISPLAY_WIDTH - (int)strlen(ver) * FONT_WIDTH) / 2;
    display_string(vx, FOOTER_Y, ver, COLOR_DGRAY, COLOR_BLACK);
}

int home_screen_show(void)
{
    draw_home_screen();

    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);

        /* Hit test against grid cells */
        if (ty >= GRID_Y && ty < GRID_Y + GRID_ROWS * CELL_H) {
            int col = (tx - GRID_X) / CELL_W;
            int row = (ty - GRID_Y) / CELL_H;

            if (col >= 0 && col < GRID_COLS && row >= 0 && row < GRID_ROWS) {
                int idx = row * GRID_COLS + col;
                if (idx < APP_COUNT)
                    return idx;
            }
        }
    }
}
