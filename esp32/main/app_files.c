/*
 * app_files.c — File browser app
 *
 * Detects partitions (MBR/GPT/superfloppy), mounts FAT32 or exFAT,
 * and presents a touchscreen directory browser.
 */

#include "app_files.h"
#include "sdcard.h"
#include "fat32.h"
#include "exfat.h"
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "font.h"

#include <string.h>
#include <stdio.h>

/* --- Configuration --- */

#define FILES_MAX_ENTRIES 128
#define FILES_MAX_NAME    48
#define ROWS_PER_PAGE     12
#define ROW_HEIGHT        16

/* Screen layout */
#define HEADER_Y     0
#define HEADER_H    24
#define LIST_Y      (HEADER_Y + HEADER_H)
#define LIST_H      (ROWS_PER_PAGE * ROW_HEIGHT)  /* 192 */
#define FOOTER_Y    (LIST_Y + LIST_H)             /* 216 */
#define FOOTER_H    24

/* Touch zones */
#define BACK_W      56
#define PGUP_W     106
#define PGDN_X     214

/* --- Types --- */

enum fs_type { FS_NONE, FS_FAT32, FS_EXFAT };

struct file_entry {
    char     name[FILES_MAX_NAME];
    uint32_t size;
    uint8_t  is_dir;
};

/* --- State --- */

static struct file_entry s_entries[FILES_MAX_ENTRIES];
static char              s_path[256];
static int               s_entry_count;
static int               s_scroll;
static enum fs_type      s_fs_type;
static struct exfat_vol *s_exvol;

/* --- Helpers --- */

static int to_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int strcasecmp_simple(const char *a, const char *b)
{
    while (*a && *b) {
        int d = to_lower((unsigned char)*a) - to_lower((unsigned char)*b);
        if (d != 0) return d;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Format a file size into a human-readable string (right-aligned, 4 chars). */
static void format_size(uint32_t size, char *buf, int buflen)
{
    if (size < 1024)
        snprintf(buf, buflen, "%4u", (unsigned)size);
    else if (size < 1024 * 1024)
        snprintf(buf, buflen, "%3uK", (unsigned)(size / 1024));
    else if (size < 1024u * 1024u * 1024u)
        snprintf(buf, buflen, "%3uM", (unsigned)(size / (1024 * 1024)));
    else
        snprintf(buf, buflen, "%3uG", (unsigned)(size / (1024u * 1024u * 1024u)));
}

/* --- Partition Detection --- */

static uint32_t find_partition(void)
{
    uint8_t buf[512];

    /* Read MBR (sector 0) */
    if (sdcard_read(0, 1, buf) != 0)
        return 0; /* superfloppy fallback */

    /* Check MBR signature */
    if (buf[510] != 0x55 || buf[511] != 0xAA)
        return 0; /* no valid MBR — try superfloppy */

    /* First partition entry at offset 446 */
    uint8_t *pe = buf + 446;
    uint8_t ptype = pe[4];

    uint32_t start_lba = (uint32_t)pe[8]
                       | ((uint32_t)pe[9]  << 8)
                       | ((uint32_t)pe[10] << 16)
                       | ((uint32_t)pe[11] << 24);

    if (ptype == 0xEE) {
        /* GPT protective MBR — read GPT header at LBA 1 */
        uint8_t gpt[512];
        if (sdcard_read(1, 1, gpt) != 0)
            return 0;

        /* Verify "EFI PART" signature */
        if (memcmp(gpt, "EFI PART", 8) != 0)
            return 0;

        /* Partition entry LBA is at offset 72 (8 bytes, little-endian) */
        uint32_t entry_lba = (uint32_t)gpt[72]
                            | ((uint32_t)gpt[73] << 8)
                            | ((uint32_t)gpt[74] << 16)
                            | ((uint32_t)gpt[75] << 24);

        /* Read first partition entry */
        uint8_t entry[512];
        if (sdcard_read(entry_lba, 1, entry) != 0)
            return 0;

        /* Starting LBA is at offset 32 in the entry (8 bytes LE) */
        uint32_t part_lba = (uint32_t)entry[32]
                          | ((uint32_t)entry[33] << 8)
                          | ((uint32_t)entry[34] << 16)
                          | ((uint32_t)entry[35] << 24);
        return part_lba;
    }

    if (ptype == 0x0B || ptype == 0x0C || ptype == 0x07) {
        /* FAT32 (0x0B/0x0C) or exFAT/NTFS (0x07) */
        return start_lba;
    }

    /* Unknown partition type — try superfloppy */
    return 0;
}

/* --- Directory Loading --- */

static void load_directory(void)
{
    s_entry_count = 0;

    if (s_fs_type == FS_FAT32) {
        int dh = fat32_open_dir(s_path);
        if (dh < 0)
            return;
        struct fat32_dir_info info;
        while (s_entry_count < FILES_MAX_ENTRIES &&
               fat32_read_dir(dh, &info) == 1) {
            struct file_entry *e = &s_entries[s_entry_count];
            strncpy(e->name, info.name, FILES_MAX_NAME - 1);
            e->name[FILES_MAX_NAME - 1] = '\0';
            e->size = info.size;
            e->is_dir = info.is_dir;
            s_entry_count++;
        }
        fat32_close_dir(dh);
    } else {
        struct exfat_dir_info exents[FILES_MAX_ENTRIES];
        int n = exfat_readdir(s_exvol, s_path, exents, FILES_MAX_ENTRIES);
        if (n < 0)
            return;
        for (int i = 0; i < n && s_entry_count < FILES_MAX_ENTRIES; i++) {
            struct file_entry *e = &s_entries[s_entry_count];
            strncpy(e->name, exents[i].name, FILES_MAX_NAME - 1);
            e->name[FILES_MAX_NAME - 1] = '\0';
            e->size = (uint32_t)exents[i].size; /* truncate to 4 GB */
            e->is_dir = exents[i].is_dir;
            s_entry_count++;
        }
    }

    /* Sort: directories first, then case-insensitive alphabetical */
    for (int i = 1; i < s_entry_count; i++) {
        struct file_entry tmp = s_entries[i];
        int j = i - 1;
        while (j >= 0) {
            int swap = 0;
            if (tmp.is_dir && !s_entries[j].is_dir)
                swap = 1;
            else if (tmp.is_dir == s_entries[j].is_dir &&
                     strcasecmp_simple(tmp.name, s_entries[j].name) < 0)
                swap = 1;
            if (!swap)
                break;
            s_entries[j + 1] = s_entries[j];
            j--;
        }
        s_entries[j + 1] = tmp;
    }
}

/* --- Drawing --- */

static void draw_header(void)
{
    /* Back button */
    display_fill_rect(0, HEADER_Y, BACK_W, HEADER_H, COLOR_DGRAY);
    display_string(4, HEADER_Y + 4, "< Back", COLOR_WHITE, COLOR_DGRAY);

    /* Path */
    display_fill_rect(BACK_W, HEADER_Y,
                      DISPLAY_WIDTH - BACK_W, HEADER_H, COLOR_BLACK);

    /* Truncate path to fit */
    int max_chars = (DISPLAY_WIDTH - BACK_W - 8) / FONT_WIDTH;
    char pathbuf[48];
    int plen = (int)strlen(s_path);
    if (plen <= max_chars) {
        strncpy(pathbuf, s_path, sizeof(pathbuf));
    } else {
        pathbuf[0] = '.';
        pathbuf[1] = '.';
        strncpy(pathbuf + 2, s_path + plen - (max_chars - 2),
                sizeof(pathbuf) - 2);
    }
    pathbuf[sizeof(pathbuf) - 1] = '\0';
    display_string(BACK_W + 4, HEADER_Y + 4, pathbuf, COLOR_CYAN, COLOR_BLACK);
}

static void draw_file_list(void)
{
    /* Clear list area */
    display_fill_rect(0, LIST_Y, DISPLAY_WIDTH, LIST_H, COLOR_BLACK);

    int visible = s_entry_count - s_scroll;
    if (visible > ROWS_PER_PAGE)
        visible = ROWS_PER_PAGE;

    for (int i = 0; i < visible; i++) {
        struct file_entry *e = &s_entries[s_scroll + i];
        int y = LIST_Y + i * ROW_HEIGHT;

        /* Alternating subtle background */
        uint16_t bg = (i % 2) ? COLOR_BLACK : display_rgb(16, 16, 24);

        display_fill_rect(0, y, DISPLAY_WIDTH, ROW_HEIGHT, bg);

        /* Directory marker */
        display_char(4, y, e->is_dir ? '>' : ' ', COLOR_YELLOW, bg);

        /* Filename (up to 29 chars) */
        char namebuf[30];
        strncpy(namebuf, e->name, 29);
        namebuf[29] = '\0';
        display_string(12, y, namebuf,
                       e->is_dir ? COLOR_WHITE : COLOR_GRAY, bg);

        /* Size / DIR label */
        char sizebuf[8];
        if (e->is_dir) {
            strncpy(sizebuf, " DIR", sizeof(sizebuf));
        } else {
            format_size(e->size, sizebuf, sizeof(sizebuf));
        }
        int sw = (int)strlen(sizebuf) * FONT_WIDTH;
        display_string(DISPLAY_WIDTH - sw - 4, y, sizebuf,
                       COLOR_DGRAY, bg);
    }
}

static void draw_footer(void)
{
    display_fill_rect(0, FOOTER_Y, DISPLAY_WIDTH, FOOTER_H, COLOR_BLACK);

    int dirs = 0, files = 0;
    for (int i = 0; i < s_entry_count; i++) {
        if (s_entries[i].is_dir) dirs++;
        else files++;
    }

    /* Page Up button */
    if (s_scroll > 0) {
        display_fill_rect(0, FOOTER_Y, PGUP_W, FOOTER_H, COLOR_DGRAY);
        display_string(8, FOOTER_Y + 4, "< Pg Up", COLOR_WHITE, COLOR_DGRAY);
    }

    /* Summary */
    char summary[48];
    snprintf(summary, sizeof(summary), "%d file%s, %d dir%s",
             files, files == 1 ? "" : "s",
             dirs, dirs == 1 ? "" : "s");
    int sumw = (int)strlen(summary) * FONT_WIDTH;
    int sumx = (DISPLAY_WIDTH - sumw) / 2;
    display_string(sumx, FOOTER_Y + 4, summary, COLOR_GRAY, COLOR_BLACK);

    /* Page Down button */
    if (s_scroll + ROWS_PER_PAGE < s_entry_count) {
        display_fill_rect(PGDN_X, FOOTER_Y,
                          DISPLAY_WIDTH - PGDN_X, FOOTER_H, COLOR_DGRAY);
        display_string(PGDN_X + 8, FOOTER_Y + 4, "Pg Dn >",
                       COLOR_WHITE, COLOR_DGRAY);
    }
}

static void draw_screen(void)
{
    draw_header();
    draw_file_list();
    draw_footer();
}

/* --- File Info Popup --- */

static void show_file_info(struct file_entry *e)
{
    /* Draw overlay box */
    int bx = 20, by = 60, bw = 280, bh = 120;
    display_fill_rect(bx, by, bw, bh, COLOR_DGRAY);

    display_string(bx + 8, by + 8, "File Info", COLOR_CYAN, COLOR_DGRAY);

    /* Name (may truncate) */
    char namebuf[34];
    strncpy(namebuf, e->name, 33);
    namebuf[33] = '\0';
    display_string(bx + 8, by + 32, namebuf, COLOR_WHITE, COLOR_DGRAY);

    /* Size */
    char sizeline[32];
    if (e->size < 1024)
        snprintf(sizeline, sizeof(sizeline), "Size: %u bytes", (unsigned)e->size);
    else if (e->size < 1024 * 1024)
        snprintf(sizeline, sizeof(sizeline), "Size: %u KB",
                 (unsigned)(e->size / 1024));
    else
        snprintf(sizeline, sizeof(sizeline), "Size: %u.%u MB",
                 (unsigned)(e->size / (1024 * 1024)),
                 (unsigned)((e->size % (1024 * 1024)) * 10 / (1024 * 1024)));
    display_string(bx + 8, by + 56, sizeline, COLOR_GRAY, COLOR_DGRAY);

    display_string(bx + 8, by + 88, "Tap to dismiss",
                   COLOR_GRAY, COLOR_DGRAY);

    /* Wait for tap to dismiss */
    int tx, ty;
    touch_wait_tap(&tx, &ty);
}

/* --- Navigation --- */

static void enter_directory(const char *name)
{
    int plen = (int)strlen(s_path);

    /* Append /name to s_path */
    if (plen == 1 && s_path[0] == '/') {
        /* Root — just append name */
        snprintf(s_path + 1, sizeof(s_path) - 1, "%s", name);
    } else {
        snprintf(s_path + plen, sizeof(s_path) - plen, "/%s", name);
    }

    s_scroll = 0;
    load_directory();
    draw_screen();
}

/* Returns 1 if we went up, 0 if already at root (should exit). */
static int go_back(void)
{
    if (s_path[0] == '/' && s_path[1] == '\0')
        return 0; /* at root — exit app */

    /* Strip last path component */
    char *last = strrchr(s_path, '/');
    if (last == s_path) {
        /* Going back to root */
        s_path[1] = '\0';
    } else if (last) {
        *last = '\0';
    }

    s_scroll = 0;
    load_directory();
    draw_screen();
    return 1;
}

/* --- Main Entry Point --- */

void app_files_run(void)
{
    /* Init SD card */
    if (sdcard_init() != 0) {
        ui_show_error("SD card not found.");
        ui_wait_for_tap();
        return;
    }

    /* Find partition */
    uint32_t start_lba = find_partition();

    /* Detect filesystem */
    s_fs_type = FS_NONE;
    s_exvol = NULL;

    if (fat32_read_init(start_lba) == 0) {
        s_fs_type = FS_FAT32;
    } else {
        s_exvol = exfat_mount_sdcard(start_lba);
        if (s_exvol)
            s_fs_type = FS_EXFAT;
    }

    if (s_fs_type == FS_NONE) {
        ui_show_error("No FAT32 or exFAT found.");
        ui_wait_for_tap();
        sdcard_deinit();
        return;
    }

    /* Start at root */
    strcpy(s_path, "/");
    s_scroll = 0;
    load_directory();

    display_clear(COLOR_BLACK);
    draw_screen();

    /* Main loop */
    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);

        /* Back button */
        if (tx < BACK_W && ty < HEADER_H) {
            if (!go_back())
                break; /* exit app */
            continue;
        }

        /* Page Up */
        if (ty >= FOOTER_Y && tx < PGUP_W && s_scroll > 0) {
            s_scroll -= ROWS_PER_PAGE;
            if (s_scroll < 0) s_scroll = 0;
            draw_file_list();
            draw_footer();
            continue;
        }

        /* Page Down */
        if (ty >= FOOTER_Y && tx >= PGDN_X &&
            s_scroll + ROWS_PER_PAGE < s_entry_count) {
            s_scroll += ROWS_PER_PAGE;
            draw_file_list();
            draw_footer();
            continue;
        }

        /* File/directory entry tap */
        if (ty >= LIST_Y && ty < LIST_Y + LIST_H) {
            int row = (ty - LIST_Y) / ROW_HEIGHT;
            int idx = s_scroll + row;
            if (idx < s_entry_count) {
                struct file_entry *e = &s_entries[idx];
                if (e->is_dir) {
                    enter_directory(e->name);
                } else {
                    show_file_info(e);
                    draw_screen();
                }
            }
        }
    }

    /* Cleanup */
    if (s_fs_type == FS_EXFAT && s_exvol)
        exfat_unmount(s_exvol);
    s_exvol = NULL;
    s_fs_type = FS_NONE;
    sdcard_deinit();
}
