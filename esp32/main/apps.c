/*
 * apps.c — App registry for the home screen
 *
 * Wires up all apps into the g_apps[] table with icons and colors.
 * Placeholder apps show "Coming soon..." and wait for a tap.
 */

#include "app.h"
#include "icons.h"
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "font.h"
#include "app_flasher.h"
#include "app_settings.h"

#include <string.h>

static void app_placeholder(const char *name)
{
    display_clear(COLOR_BLACK);
    ui_draw_back_button();

    int nx = (DISPLAY_WIDTH - (int)strlen(name) * FONT_WIDTH) / 2;
    display_string(nx, 4, name, COLOR_WHITE, COLOR_BLACK);

    const char *msg = "Coming soon...";
    int mx = (DISPLAY_WIDTH - (int)strlen(msg) * FONT_WIDTH) / 2;
    display_string(mx, 110, msg, COLOR_GRAY, COLOR_BLACK);

    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);
        if (ui_check_back_button(tx, ty))
            return;
    }
}

static void app_files_run(void)   { app_placeholder("Files"); }
static void app_notes_run(void)   { app_placeholder("Notes"); }
static void app_guide_run(void)   { app_placeholder("Guide"); }
static void app_tools_run(void)   { app_placeholder("Tools"); }

app_t g_apps[APP_COUNT] = {
    { "Flash SD", icon_flash,   COLOR_GREEN,  app_flasher_run  },
    { "Files",    icon_folder,  COLOR_YELLOW, app_files_run    },
    { "Notes",    icon_notes,   COLOR_CYAN,   app_notes_run    },
    { "Guide",    icon_book,    COLOR_WHITE,  app_guide_run    },
    { "Tools",    icon_wrench,  COLOR_GRAY,   app_tools_run    },
    { "Settings", icon_gear,    COLOR_GRAY,   app_settings_run },
};
