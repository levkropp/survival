---
layout: default
title: "Chapter 38: The File Browser"
parent: "Phase 2: The Survival Toolkit"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 4
---

# Chapter 38: The File Browser

## Connecting the Pieces

Chapters 36 and 37 gave us the ability to read. FAT32 can list directories and read files. exFAT can do the same. But those capabilities are just library functions — nobody calls them yet. The home screen's "Files" icon still shows "Coming soon..." and waits for a tap.

This chapter wires everything together. The file browser initializes the SD card, figures out what partition scheme and filesystem it's looking at, lists directory contents, and lets the user navigate with touch. It's the first app in the toolkit that does something useful with an SD card the user brings from their own world — not one we formatted, but one they prepared on their laptop with their own files.

## Finding the Partition

The flasher created GPT partition tables because UEFI machines need them. But most SD cards in the wild use MBR partitioning. Cheap cards from the factory are formatted as a single FAT32 or exFAT partition with an MBR. Some cards have no partition table at all — they're "superfloppy" format, where the filesystem starts at sector 0.

The file browser needs to handle all three: GPT, MBR, and superfloppy. A `find_partition()` function tries them in order:

```c
static uint32_t find_partition(void)
{
    uint8_t buf[512];

    if (sdcard_read(0, 1, buf) != 0)
        return 0;

    /* Check MBR signature */
    if (buf[510] != 0x55 || buf[511] != 0xAA)
        return 0; /* no valid MBR — try superfloppy */

    uint8_t *pe = buf + 446;  /* first partition entry */
    uint8_t ptype = pe[4];

    uint32_t start_lba = (uint32_t)pe[8]
                       | ((uint32_t)pe[9]  << 8)
                       | ((uint32_t)pe[10] << 16)
                       | ((uint32_t)pe[11] << 24);
```

The MBR's partition table starts at byte 446. Each entry is 16 bytes. Byte 4 of the entry is the partition type. Bytes 8–11 are the starting LBA, little-endian.

If the partition type is `0xEE`, it's a GPT protective MBR. We read the GPT header at LBA 1, verify the `"EFI PART"` signature, then read the partition entry array to find the actual starting LBA:

```c
    if (ptype == 0xEE) {
        uint8_t gpt[512];
        if (sdcard_read(1, 1, gpt) != 0)
            return 0;
        if (memcmp(gpt, "EFI PART", 8) != 0)
            return 0;

        uint32_t entry_lba = (uint32_t)gpt[72]
                            | ((uint32_t)gpt[73] << 8)
                            | ((uint32_t)gpt[74] << 16)
                            | ((uint32_t)gpt[75] << 24);

        uint8_t entry[512];
        if (sdcard_read(entry_lba, 1, entry) != 0)
            return 0;

        uint32_t part_lba = (uint32_t)entry[32]
                          | ((uint32_t)entry[33] << 8)
                          | ((uint32_t)entry[34] << 16)
                          | ((uint32_t)entry[35] << 24);
        return part_lba;
    }
```

For MBR partitions with type `0x0B` or `0x0C` (FAT32) or `0x07` (exFAT/NTFS), we use the start LBA directly from the MBR entry. If nothing matches, we return 0 and let the filesystem detection try sector 0 as superfloppy.

This covers every SD card layout we're likely to encounter. Cards we formatted ourselves have GPT. Cards from cameras and phones have MBR. Tiny cards from the early 2000s might be superfloppy. The function always returns an answer — it never fails, it just falls back to sector 0.

## Detecting the Filesystem

Once we have the partition offset, we try both filesystems:

```c
s_fs_type = FS_NONE;
s_exvol = NULL;

if (fat32_read_init(start_lba) == 0) {
    s_fs_type = FS_FAT32;
} else {
    s_exvol = exfat_mount_sdcard(start_lba);
    if (s_exvol)
        s_fs_type = FS_EXFAT;
}
```

FAT32 first, because it's more common on cards up to 32 GB and doesn't require heap allocation. If FAT32 fails (the BPB doesn't parse), we try exFAT. If both fail, we show an error and bail out.

An enum tracks which filesystem is active:

```c
enum fs_type { FS_NONE, FS_FAT32, FS_EXFAT };
```

This two-variable state — `s_fs_type` plus `s_exvol` — carries through every directory operation. Every call to load a directory checks which filesystem is active and calls the right API.

## A Unified Entry Buffer

FAT32 and exFAT return directory entries in different structs (`fat32_dir_info` and `exfat_dir_info`). The file browser needs a single representation:

```c
#define FILES_MAX_ENTRIES 128
#define FILES_MAX_NAME     48

struct file_entry {
    char     name[FILES_MAX_NAME];
    uint32_t size;
    uint8_t  is_dir;
};

static struct file_entry s_entries[FILES_MAX_ENTRIES];
```

53 bytes per entry, 128 entries, 6.8 KB total. This is a static buffer — no heap allocation, no fragmentation, always available. Names are truncated to 47 characters. The screen can display about 29 characters per row anyway, so the extra space handles the case where we need to compare or display a longer name in a popup.

The `load_directory()` function populates this buffer from whichever filesystem is active:

```c
if (s_fs_type == FS_FAT32) {
    int dh = fat32_open_dir(s_path);
    struct fat32_dir_info info;
    while (s_entry_count < FILES_MAX_ENTRIES &&
           fat32_read_dir(dh, &info) == 1) {
        /* copy into s_entries[s_entry_count++] */
    }
    fat32_close_dir(dh);
} else {
    struct exfat_dir_info exents[FILES_MAX_ENTRIES];
    int n = exfat_readdir(s_exvol, s_path, exents, FILES_MAX_ENTRIES);
    /* copy into s_entries */
}
```

After loading, we sort: directories first, then case-insensitive alphabetical within each group. An insertion sort — O(n²), but n is at most 128, and the comparison is a simple `strcasecmp`. On the ESP32 this finishes in under a millisecond.

## The Screen Layout

320 pixels wide, 240 pixels tall. Every pixel is accounted for:

```
y=0-23:    [< Back]  "/current/path"
y=24-215:  12 file entry rows (16px each)
y=216-239: [< Pg Up]  "5 files, 2 dirs"  [Pg Dn >]
```

Each entry row is 16 pixels tall — one line of our 8×16 VGA font with no padding. The left 8 pixels show `>` for directories or a space for files. The middle shows the filename. The right edge shows the size (`1.2M`, `456K`) or `DIR` for directories, right-aligned.

Alternating rows get a very subtle background tint — `rgb(16, 16, 24)` versus black. It's barely visible but makes it easier to track which row you're reading across a wide column gap.

The header shows a "< Back" button and the current path. If the path is longer than the available space, it's truncated with `..` at the beginning — we show the end of the path because that's the part that changed.

The footer shows pagination buttons when there are more than 12 entries. Between them, a summary: "5 files, 2 dirs". The page-up button only appears when scrolled down. The page-down button only appears when there are more entries below.

## Touch Handling

Four touch zones, checked in order:

| Zone | Region | Action |
|------|--------|--------|
| Back button | top-left 56×24px | Go to parent directory; exit app if at root |
| Page Up | bottom-left 106×24px | Scroll up 12 entries |
| Page Down | bottom-right 106×24px | Scroll down 12 entries |
| Entry row | middle 320×16px per row | Enter directory or show file info |

The entry tap is the interesting one. We calculate which row was tapped:

```c
int row = (ty - LIST_Y) / ROW_HEIGHT;
int idx = s_scroll + row;
if (idx < s_entry_count) {
    struct file_entry *e = &s_entries[idx];
    if (e->is_dir)
        enter_directory(e->name);
    else
        show_file_info(e);
}
```

For directories, we append the name to the current path and reload. For files, we show an info popup — name and size, dismissed with any tap. Chapter 39 will replace this popup with actual file viewers (text and images).

## Navigation State

Five static variables track the browser's state:

```c
static char s_path[256];           /* current directory path */
static int  s_entry_count;         /* entries in s_entries[] */
static int  s_scroll;              /* first visible entry index */
static enum fs_type s_fs_type;     /* FS_FAT32 or FS_EXFAT */
static struct exfat_vol *s_exvol;  /* non-NULL if exFAT */
```

Entering a directory appends `/name` to the path, resets scroll to 0, and reloads. Going back strips the last path component with `strrchr(s_path, '/')`. If we're already at root (`"/"`), going back exits the app entirely — returning control to the home screen, the same pattern every other app uses.

## The App Lifecycle

The file browser follows the same lifecycle as every app in the toolkit:

```
app_files_run():
    sdcard_init() — fail → show error, wait tap, return
    find_partition() → start_lba
    detect filesystem — fail → show error, deinit, return
    strcpy(s_path, "/")
    load_directory()

    loop:
        draw_screen()
        touch_wait_tap()
        handle: back / entry / page up / page down

    cleanup:
        if exFAT: exfat_unmount()
        sdcard_deinit()
```

Init, loop, cleanup. No threads. No callbacks. No state machines. The app owns the screen and the SD card for its entire lifetime. When it returns, both are released.

## Wiring Into the Home Screen

The `apps.c` file had a one-line placeholder:

```c
static void app_files_run(void) { app_placeholder("Files"); }
```

We replace it with the real function from `app_files.h`:

```c
#include "app_files.h"
```

The function signature is the same — `void app_files_run(void)` — so the `g_apps[]` table doesn't change. The home screen doesn't know or care that the Files app now has 487 lines of real code behind it instead of a placeholder call.

## What We Built

```
File                        Lines   Purpose
──────────────────────────  ─────   ──────────────────────────────────
esp32/main/app_files.h         11   Header: app_files_run() declaration
esp32/main/app_files.c        487   File browser: partition detection,
                                    filesystem mount, directory listing,
                                    touch navigation, file info popup
esp32/main/apps.c              ~3   Replace placeholder with real import
esp32/main/CMakeLists.txt      +1   Added app_files.c to build
cyd-emulator/CMakeLists.txt    +1   Added app_files.c to emulator build
```

The file browser is the first app that exercises both filesystem drivers. It reads real SD cards — ones formatted on laptops, pulled from cameras, carried in pockets. It's 487 lines of C that turn five chapters of infrastructure (SD card driver, GPT, FAT32 read, exFAT port) into something a user can touch and navigate.

But navigating to a file and seeing its name and size is only half the story. In Chapter 39, we replace the info popup with actual file viewers — text files displayed on screen, BMP and JPEG images rendered to the display. The file browser becomes the entry point; the viewers make it useful.

---

**Next:** [Chapter 39: File Viewers](chapter-39-file-viewers)
