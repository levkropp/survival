---
layout: default
title: "Chapter 35: The Home Screen"
parent: "Phase 2: The Survival Toolkit"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 1
---

# Chapter 35: The Home Screen

## Beyond a Single Purpose

The flasher works. Insert a blank SD card, tap the screen, wait a minute, and you have a bootable survival workstation. But the ESP32 CYD is now sitting idle 99% of the time — a $7 computer with a touchscreen and a display, doing nothing between flash jobs.

Look at what's already running. We have a display driver that draws text and rectangles at 40 MHz. We have a touchscreen driver that reads taps. We have FAT32 code that writes filesystems. We have a decompression engine. We have 1.1 MB of free flash. We have all the tools to build something more useful.

So we will. Over the next eight chapters, we turn the single-purpose flasher into a multi-app survival toolkit:

- **Chapter 35 (this one):** A home screen with six app icons and an app framework
- **Chapter 36:** FAT32 directory listing and file reading
- **Chapter 37:** exFAT support — porting the Part 1 driver to ESP32
- **Chapter 38:** A file browser with scrollable directory navigation
- **Chapter 39:** A text file viewer and image viewer (BMP + JPEG)
- **Chapter 40:** A notes app with an on-screen keyboard
- **Chapter 41:** A compressed survival reference guide stored in flash
- **Chapter 42:** NVS persistence, polish, and integration

The architecture stays simple. No widget framework. No event system. No heap allocation games. Each app is a function that takes over the screen and returns when the user taps "Back." The same raw `display_*` and `touch_*` API from Phase 1. The same 520 KB of RAM.

## The App Framework

An app needs four things: a name for its label, an icon for the grid, a color for visual distinction, and a function that runs it.

```c
typedef struct {
    const char     *name;
    const uint8_t  *icon;
    uint16_t        icon_color;
    void          (*run)(void);
} app_t;
```

That's the entire framework. No base classes, no virtual dispatch tables, no registration macros. A struct with a function pointer. The `run()` function takes over the display, handles its own touch input, and returns when the user wants to go home. The caller — `main.c` — just calls `g_apps[choice].run()` in a loop.

Six apps, defined in `apps.c`:

```c
app_t g_apps[APP_COUNT] = {
    { "Flash SD", icon_flash,   COLOR_GREEN,  app_flasher_run  },
    { "Files",    icon_folder,  COLOR_YELLOW, app_files_run    },
    { "Notes",    icon_notes,   COLOR_CYAN,   app_notes_run    },
    { "Guide",    icon_book,    COLOR_WHITE,  app_guide_run    },
    { "Tools",    icon_wrench,  COLOR_GRAY,   app_tools_run    },
    { "Settings", icon_gear,    COLOR_GRAY,   app_settings_run },
};
```

Only two apps have real implementations right now: Flash SD (wrapping the existing flasher) and Settings (device info and a configuration toggle). The other four show "Coming soon..." and wait for a back button tap. They'll be filled in over the next chapters.

## Drawing Icons

The home screen needs icons. On a system with a PNG decoder and a filesystem, you'd load image files. We have neither. The icons are monochrome bitmaps compiled directly into the firmware as `const` arrays — they live in flash, cost zero RAM, and draw instantly.

Each icon is 32×32 pixels, stored as 1 bit per pixel:

```c
#define ICON_W 32
#define ICON_H 32
```

At 32 pixels wide, each row is exactly 4 bytes. 32 rows × 4 bytes = 128 bytes per icon. Six icons = 768 bytes of flash. For comparison, a single 32×32 RGB565 image would be 2,048 bytes — nearly 3× larger and harder to colorize.

The monochrome format has a useful property: the color is applied at draw time, not baked into the data. The same lightning bolt bitmap renders green for "Flash SD" and could render red for an error state — just change the foreground color parameter. The bitmap is a shape; the color is a choice.

To draw these bitmaps, we need a new display function:

```c
void display_draw_bitmap1bpp(int x, int y, int w, int h,
                              const uint8_t *bitmap,
                              uint16_t fg, uint16_t bg);
```

The implementation follows the same pattern as `display_char`: walk each row, expand bits to RGB565 pixels, send a scanline to the display controller.

```c
void display_draw_bitmap1bpp(int x, int y, int w, int h,
                              const uint8_t *bitmap,
                              uint16_t fg, uint16_t bg)
{
    int row_bytes = (w + 7) / 8;
    uint16_t pixels[32];

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
        if (dx < 0) { pw += dx; dx = 0; }
        if (dx + pw > DISPLAY_WIDTH) pw = DISPLAY_WIDTH - dx;
        if (pw > 0)
            esp_lcd_panel_draw_bitmap(panel, dx, dy, dx + pw, dy + 1,
                                      pixels + (dx - x));
    }
}
```

`pixels[32]` is a stack buffer — 64 bytes, used and released each call. The bit extraction uses `0x80 >> (col & 7)` because the bitmap format is MSB-first: bit 7 of each byte is the leftmost pixel, matching the font data convention established in Chapter 29.

The emulator version writes directly to the framebuffer under mutex instead of calling the SPI panel driver. Same logic, different output target.

## The Home Screen Layout

320×240 pixels. Not a lot of space. The layout divides the screen into three zones:

```
y=0..23:    "SURVIVAL WORKSTATION" title (green, centered)
y=32..215:  2×3 icon grid
y=220..239: "v2.0" footer (dim gray)
```

The grid cells are 106×92 pixels each (320 ÷ 3 ≈ 106 wide, 184 ÷ 2 = 92 tall). Each cell contains:
- A 32×32 icon, centered horizontally, offset 12 pixels from the cell top
- A text label, centered below the icon

```
+----------+----------+----------+
| Flash SD | Files    | Notes    |
|  (green) | (yellow) | (cyan)   |
+----------+----------+----------+
| Guide    | Tools    | Settings |
| (white)  | (gray)   | (gray)   |
+----------+----------+----------+
```

Touch detection is simple: which cell did the tap land in?

```c
int home_screen_show(void)
{
    draw_home_screen();

    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);

        if (ty >= GRID_Y && ty < GRID_Y + GRID_ROWS * CELL_H) {
            int col = (tx - GRID_X) / CELL_W;
            int row = (ty - GRID_Y) / CELL_H;

            if (col >= 0 && col < GRID_COLS &&
                row >= 0 && row < GRID_ROWS) {
                int idx = row * GRID_COLS + col;
                if (idx < APP_COUNT)
                    return idx;
            }
        }
    }
}
```

Integer division does all the work. No hit-test arrays, no button objects. The grid is implicit in the arithmetic: `col = tx / CELL_W`, `row = (ty - GRID_Y) / CELL_H`, `index = row * 3 + col`. Taps outside the grid area (on the title or footer) are ignored — the loop just calls `touch_wait_tap` again.

## The Back Button

Every app needs a way to return to the home screen. A consistent "< Back" button in the top-left corner handles this:

```c
#define BACK_X     0
#define BACK_Y     0
#define BACK_W     56
#define BACK_H     24

void ui_draw_back_button(void)
{
    display_fill_rect(BACK_X, BACK_Y, BACK_W, BACK_H, COLOR_DGRAY);
    display_string(BACK_X + 4, BACK_Y + 4, "< Back",
                   COLOR_WHITE, COLOR_DGRAY);
}

int ui_check_back_button(int tx, int ty)
{
    return tx >= BACK_X && tx < BACK_X + BACK_W &&
           ty >= BACK_Y && ty < BACK_Y + BACK_H;
}
```

Apps call `ui_draw_back_button()` when drawing their screen, then check `ui_check_back_button(tx, ty)` after each tap. If it returns true, the app's `run()` function returns, and the main loop redraws the home screen. Simple convention, no framework overhead.

## Refactoring the Flasher

The old flasher owned the entire UI flow: `main.c` called `ui_show_menu()` which drew two big buttons ("Flash aarch64" / "Flash x86_64") and blocked until a tap. The new flasher is one app among six. It needs to fit the app framework pattern: take over the screen, do its work, return on back.

`app_flasher.c` wraps the existing `flasher_run()` without changing a single line of flasher, GPT, or FAT32 code. The core flashing logic is untouched — only the entry point changes.

The default behavior is streamlined. Most users want x86_64. Instead of forcing an architecture choice every time, the flasher now shows a single confirmation screen:

```c
static int show_confirm(void)
{
    display_clear(COLOR_BLACK);
    ui_draw_back_button();

    /* ... draw "Flash SD Card" title, "Architecture: x86_64" ... */

    draw_button(BTN_X, BTN_Y2, BTN_W, BTN_H,
                COLOR_BLUE, "Flash x86_64");

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
```

One tap to flash. Back button to cancel. No unnecessary choice.

For users who need aarch64 (ARM64 machines), there's a toggle buried in Settings. When enabled, the flasher shows the dual-architecture selection screen — the same two buttons as the old Phase 1 UI, plus a back button.

```c
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
```

The `while (1)` loop means a successful flash returns to the flasher screen (in case you want to flash another card), while the back button breaks out to the home screen.

## Settings

The settings app has two screens. The main screen shows device info — chip model, core count, board identifier, display specs:

```c
static void draw_settings_main(void)
{
    display_clear(COLOR_BLACK);
    ui_draw_back_button();

    display_string(100, 4, "Settings", COLOR_WHITE, COLOR_BLACK);

    /* Device info */
    display_string(20, 36, "Device Info", COLOR_GREEN, COLOR_BLACK);

    esp_chip_info_t info;
    esp_chip_info(&info);

    char line[48];
    snprintf(line, sizeof(line), "%s  %d cores  4MB flash",
             chip, info.cores);
    display_string(28, 60, line, COLOR_GRAY, COLOR_BLACK);

    display_string(28, 80, "Board: 2432S028R (CYD)",
                   COLOR_GRAY, COLOR_BLACK);

    /* ... "Advanced Options >" button ... */
}
```

Tapping "Advanced Options" navigates to a second screen with a checkbox toggle:

```c
static void draw_checkbox(int x, int y, int w, int h,
                           int checked, const char *label)
{
    display_fill_rect(x, y, w, h, COLOR_DGRAY);
    const char *box = checked ? "[x]" : "[ ]";
    display_string(x + 8, y + 8, box, COLOR_CYAN, COLOR_DGRAY);
    display_string(x + 40, y + 8, label, COLOR_WHITE, COLOR_DGRAY);
}
```

Tapping the checkbox toggles `g_settings.show_aarch64`, redraws the checkbox, and calls `settings_save()`. For now, `settings_save()` is a stub — NVS persistence comes in Chapter 42. The setting is in-memory only: it resets on reboot. That's fine for a toggle that rarely changes.

The `settings_t` struct is minimal:

```c
typedef struct {
    bool show_aarch64;
} settings_t;

settings_t g_settings;
```

One field. Room to grow. Each future chapter may add a field — text editor font size, file browser sort order, guide bookmarks — without changing the framework.

## The New Main Loop

The old `main.c` was a single-purpose flasher loop:

```c
/* Old: Phase 1 */
while (1) {
    int choice = ui_show_menu(has_usb_otg);
    /* ... sdcard_init, flasher_run, sdcard_deinit ... */
}
```

The new version is an app launcher:

```c
/* New: Phase 2 */
while (1) {
    int choice = home_screen_show();
    g_apps[choice].run();
}
```

Two lines. Show the grid, get a choice, run the app. When the app returns, show the grid again. No state machine, no event dispatch, no lifecycle callbacks.

One other change: `payload_init()` failure is no longer fatal. In Phase 1, missing payload data meant the flasher couldn't work, so we showed an error and halted. Now the device has other apps — the file browser, notes, and survival guide don't need the payload at all. A missing payload just means "Flash SD" will fail gracefully when you try to use it.

```c
int payload_ok = payload_init();
if (payload_ok != 0) {
    ESP_LOGW(TAG, "No payload found — Flash SD will not work");
}
```

A warning instead of an infinite loop. The device boots to the home screen regardless.

## The UI Changes

The old `ui.h` exported `ui_show_menu()` and two choice constants:

```c
/* Phase 1 — removed */
#define UI_CHOICE_FLASH_AARCH64  1
#define UI_CHOICE_FLASH_X86_64   2
int ui_show_menu(int has_usb);
```

These are gone. The home screen replaces the menu. The flasher handles its own architecture selection internally. What remains in `ui.h` is the shared utility functions — splash, progress, done, error, wait-for-tap — plus the new back button helpers. These are the building blocks that every app uses.

The splash screen subtitle changes from "SD Card Flasher" to "Survival Toolkit" and the version bumps to v2.0. Small details, but they signal to the user that the device does more than flash cards now.

## What We Built

New files:

```
File               Lines   Purpose
─────────────────  ─────   ──────────────────────────────
app.h                29    App struct, home screen declaration
app.c                94    Home screen: grid, icons, touch
apps.c               51    App registry (6 entries)
app_flasher.h        11    Flasher app declaration
app_flasher.c       138    Flasher wrapper with arch toggle
app_settings.h       10    Settings app declaration
app_settings.c      134    Device info, advanced options
settings.h           22    Settings struct
settings.c           17    Settings init/save
icons.h             231    Six 32×32 monochrome bitmaps
─────────────────  ─────
New code:           737 lines
```

Modified files:

```
File               Lines   Changes
─────────────────  ─────   ──────────────────────────────
display.h            49    +display_draw_bitmap1bpp declaration
display.c           182    +display_draw_bitmap1bpp implementation
ui.h                 28    +back button, -menu/choice constants
ui.c                120    +back button, -ui_show_menu, updated splash
main.c               72    App framework loop, non-fatal payload
```

The total codebase is now about 3,300 lines across 24 source files. The binary grew by less than 1 KB — icon data is `const` in flash, and the app framework is just function pointers and integer arithmetic. We're using 243 KB of our 1.4 MB application partition. Plenty of room for the chapters ahead.

The home screen is the foundation. Every feature from here forward — file browsing, note taking, image viewing, the survival guide — is another `run()` function in the registry. The pattern is set. Now we fill it in.

---

**Next:** [Chapter 36: Reading FAT32](chapter-36-reading-fat32)
