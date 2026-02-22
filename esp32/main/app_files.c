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
#include <stdlib.h>

#include "sped.h"
#include "femtojpeg.h"

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

/* Check if filename ends with the given extension (case-insensitive). */
static int has_ext(const char *name, const char *ext)
{
    int nlen = (int)strlen(name);
    int elen = (int)strlen(ext);
    if (nlen < elen) return 0;
    return strcasecmp_simple(name + nlen - elen, ext) == 0;
}

static int is_text_file(const char *name)
{
    static const char *exts[] = {
        ".txt", ".log", ".md", ".csv", ".c", ".h",
        ".py", ".sh", ".cfg", ".ini", ".json", NULL
    };
    for (int i = 0; exts[i]; i++)
        if (has_ext(name, exts[i])) return 1;
    return 0;
}

static int is_bmp_file(const char *name)
{
    return has_ext(name, ".bmp");
}

static int is_png_file(const char *name)
{
    return has_ext(name, ".png");
}

static int is_jpg_file(const char *name)
{
    return has_ext(name, ".jpg") || has_ext(name, ".jpeg");
}

/* Build full path from s_path + filename. */
static void build_full_path(char *out, size_t outsz, const char *name)
{
    int plen = (int)strlen(s_path);
    if (plen == 1 && s_path[0] == '/') {
        out[0] = '/';
        strncpy(out + 1, name, outsz - 2);
        out[outsz - 1] = '\0';
    } else {
        strncpy(out, s_path, outsz - 1);
        out[outsz - 1] = '\0';
        int cur = (int)strlen(out);
        if (cur + 1 < (int)outsz) {
            out[cur] = '/';
            strncpy(out + cur + 1, name, outsz - cur - 2);
            out[outsz - 1] = '\0';
        }
    }
}

/* Read an entire file into a malloc'd buffer. Returns NULL on error.
 * Caller must free(). Caps at max_size bytes. */
static void *read_file(const char *path, uint32_t max_size, uint32_t *out_size)
{
    if (s_fs_type == FS_FAT32) {
        int fh = fat32_file_open(path);
        if (fh < 0) return NULL;

        uint8_t *buf = malloc(max_size);
        if (!buf) { fat32_file_close(fh); return NULL; }

        uint32_t total = 0;
        while (total < max_size) {
            int n = fat32_file_read(fh, buf + total, 4096);
            if (n <= 0) break;
            total += (uint32_t)n;
        }
        fat32_file_close(fh);

        *out_size = total;
        return buf;
    } else {
        /* exFAT — readfile gives us the whole thing */
        size_t fsz;
        void *data = exfat_readfile(s_exvol, path, &fsz);
        if (!data) return NULL;
        if (fsz > max_size) { free(data); return NULL; }
        *out_size = (uint32_t)fsz;
        return data;
    }
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

/* --- Text File Viewer --- */

#define TEXT_MAX_SIZE   (32 * 1024)
#define TEXT_MAX_LINES  1000
#define TEXT_LINES_PAGE 12
#define TEXT_CHARS_LINE 40
#define TEXT_CONTENT_Y  24
#define TEXT_FOOTER_Y   216

static void view_text_file(const char *path, const char *filename, uint32_t size)
{
    if (size > TEXT_MAX_SIZE) size = TEXT_MAX_SIZE;

    uint32_t actual = 0;
    char *buf = read_file(path, TEXT_MAX_SIZE, &actual);
    if (!buf) {
        ui_show_error("Cannot read file.");
        ui_wait_for_tap();
        return;
    }

    /* Parse line offsets: scan for '\n', replace with '\0' */
    uint16_t *line_off = malloc(TEXT_MAX_LINES * sizeof(uint16_t));
    if (!line_off) { free(buf); return; }

    int nlines = 0;
    line_off[0] = 0;
    nlines = 1;
    for (uint32_t i = 0; i < actual && nlines < TEXT_MAX_LINES; i++) {
        if (buf[i] == '\n') {
            buf[i] = '\0';
            if (i + 1 < actual) {
                line_off[nlines++] = (uint16_t)(i + 1);
            }
        } else if (buf[i] == '\r') {
            buf[i] = '\0';  /* strip CR */
        }
    }
    /* Null-terminate the last line */
    if (actual < TEXT_MAX_SIZE)
        buf[actual] = '\0';
    else
        buf[TEXT_MAX_SIZE - 1] = '\0';

    int top_line = 0;

    /* Draw function */
    #define TV_DRAW() do { \
        /* Header */ \
        display_fill_rect(0, 0, BACK_W, HEADER_H, COLOR_DGRAY); \
        display_string(4, 4, "< Back", COLOR_WHITE, COLOR_DGRAY); \
        display_fill_rect(BACK_W, 0, DISPLAY_WIDTH - BACK_W, HEADER_H, COLOR_BLACK); \
        { \
            char tb[34]; \
            strncpy(tb, filename, 33); tb[33] = '\0'; \
            display_string(BACK_W + 4, 4, tb, COLOR_CYAN, COLOR_BLACK); \
        } \
        /* Content */ \
        display_fill_rect(0, TEXT_CONTENT_Y, DISPLAY_WIDTH, \
                          TEXT_LINES_PAGE * ROW_HEIGHT, COLOR_BLACK); \
        for (int _i = 0; _i < TEXT_LINES_PAGE; _i++) { \
            int _ln = top_line + _i; \
            if (_ln >= nlines) break; \
            char _linebuf[TEXT_CHARS_LINE + 1]; \
            strncpy(_linebuf, buf + line_off[_ln], TEXT_CHARS_LINE); \
            _linebuf[TEXT_CHARS_LINE] = '\0'; \
            display_string(0, TEXT_CONTENT_Y + _i * ROW_HEIGHT, \
                           _linebuf, COLOR_GRAY, COLOR_BLACK); \
        } \
        /* Footer */ \
        display_fill_rect(0, TEXT_FOOTER_Y, DISPLAY_WIDTH, FOOTER_H, COLOR_BLACK); \
        if (top_line > 0) { \
            display_fill_rect(0, TEXT_FOOTER_Y, PGUP_W, FOOTER_H, COLOR_DGRAY); \
            display_string(8, TEXT_FOOTER_Y + 4, "< Pg Up", COLOR_WHITE, COLOR_DGRAY); \
        } \
        { \
            char _ind[32]; \
            snprintf(_ind, sizeof(_ind), "%d-%d / %d", \
                     top_line + 1, \
                     (top_line + TEXT_LINES_PAGE < nlines) ? \
                         top_line + TEXT_LINES_PAGE : nlines, \
                     nlines); \
            int _sw = (int)strlen(_ind) * FONT_WIDTH; \
            display_string((DISPLAY_WIDTH - _sw) / 2, TEXT_FOOTER_Y + 4, \
                           _ind, COLOR_GRAY, COLOR_BLACK); \
        } \
        if (top_line + TEXT_LINES_PAGE < nlines) { \
            display_fill_rect(PGDN_X, TEXT_FOOTER_Y, \
                              DISPLAY_WIDTH - PGDN_X, FOOTER_H, COLOR_DGRAY); \
            display_string(PGDN_X + 8, TEXT_FOOTER_Y + 4, "Pg Dn >", \
                           COLOR_WHITE, COLOR_DGRAY); \
        } \
    } while(0)

    display_clear(COLOR_BLACK);
    TV_DRAW();

    while (1) {
        int tx, ty;
        touch_wait_tap(&tx, &ty);

        /* Back */
        if (tx < BACK_W && ty < HEADER_H)
            break;

        /* Page Up */
        if (ty >= TEXT_FOOTER_Y && tx < PGUP_W && top_line > 0) {
            top_line -= TEXT_LINES_PAGE;
            if (top_line < 0) top_line = 0;
            TV_DRAW();
            continue;
        }

        /* Page Down */
        if (ty >= TEXT_FOOTER_Y && tx >= PGDN_X &&
            top_line + TEXT_LINES_PAGE < nlines) {
            top_line += TEXT_LINES_PAGE;
            TV_DRAW();
            continue;
        }
    }

    #undef TV_DRAW
    free(line_off);
    free(buf);
}

/* --- BMP Image Viewer --- */

#define BMP_MAX_W 320
#define BMP_MAX_H 240
/* Max pixel data: 320*240*3 + padding ≈ 231 KB */
#define BMP_MAX_FILE (320u * 240u * 4u)

static void view_bmp_file(const char *path, const char *filename, uint32_t size)
{
    uint32_t actual = 0;
    uint8_t *buf = read_file(path, BMP_MAX_FILE, &actual);
    if (!buf || actual < 54) {
        if (buf) free(buf);
        ui_show_error("Cannot read BMP.");
        ui_wait_for_tap();
        return;
    }

    /* Parse BMP header */
    if (buf[0] != 'B' || buf[1] != 'M') {
        free(buf);
        ui_show_error("Not a valid BMP.");
        ui_wait_for_tap();
        return;
    }

    uint32_t data_offset = buf[10] | (buf[11] << 8) |
                           (buf[12] << 16) | (buf[13] << 24);
    int32_t bmp_w = (int32_t)(buf[18] | (buf[19] << 8) |
                              (buf[20] << 16) | (buf[21] << 24));
    int32_t bmp_h = (int32_t)(buf[22] | (buf[23] << 8) |
                              (buf[24] << 16) | (buf[25] << 24));
    uint16_t bpp = buf[28] | (buf[29] << 8);
    uint32_t compression = buf[30] | (buf[31] << 8) |
                           (buf[32] << 16) | (buf[33] << 24);

    if (bpp != 24 || compression != 0 || bmp_w <= 0) {
        free(buf);
        ui_show_error("Only 24-bit uncompressed BMP.");
        ui_wait_for_tap();
        return;
    }

    /* Handle bottom-up (positive height) or top-down (negative height) */
    int bottom_up = (bmp_h > 0);
    int img_h = bottom_up ? bmp_h : -bmp_h;
    int img_w = bmp_w;

    /* Clip to screen */
    int draw_w = (img_w > BMP_MAX_W) ? BMP_MAX_W : img_w;
    int draw_h = (img_h > BMP_MAX_H) ? BMP_MAX_H : img_h;

    /* Center on screen */
    int off_x = (DISPLAY_WIDTH - draw_w) / 2;
    int off_y = (DISPLAY_HEIGHT - draw_h) / 2;

    /* Row stride with padding */
    uint32_t row_bytes = ((uint32_t)(img_w * 3) + 3) & ~3u;

    /* Clear screen and render */
    display_clear(COLOR_BLACK);

    uint16_t line[BMP_MAX_W];
    for (int y = 0; y < draw_h; y++) {
        int src_row;
        if (bottom_up)
            src_row = (img_h - 1 - y);  /* bottom-up: first screen row = last BMP row */
        else
            src_row = y;

        uint32_t row_offset = data_offset + (uint32_t)src_row * row_bytes;
        if (row_offset + (uint32_t)(draw_w * 3) > actual)
            break;  /* out of data */

        const uint8_t *row = buf + row_offset;
        for (int x = 0; x < draw_w; x++) {
            uint8_t b = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            line[x] = display_rgb(r, g, b);
        }
        display_draw_rgb565_line(off_x, off_y + y, draw_w, line);
    }

    /* Overlay filename in top-left */
    char tb[34];
    strncpy(tb, filename, 33); tb[33] = '\0';
    int tw = (int)strlen(tb) * FONT_WIDTH;
    display_fill_rect(0, 0, tw + 8, FONT_HEIGHT + 4, COLOR_BLACK);
    display_string(4, 2, tb, COLOR_WHITE, COLOR_BLACK);

    /* Tap to dismiss */
    int tx, ty;
    touch_wait_tap(&tx, &ty);

    free(buf);
}

/* --- PNG/JPEG Image Viewer --- */

/* Max decoded image file size (~307 KB) */
#define IMG_MAX_FILE (320u * 240u * 4u)

/* Callback context for streaming image decoders */
struct img_draw_ctx {
    int off_x, off_y;     /* centering offset on screen */
    int max_w, max_h;     /* clip to screen bounds */
};

static void img_row_cb(int y, int w, const uint16_t *rgb565, void *user)
{
    struct img_draw_ctx *ctx = user;
    if (y >= ctx->max_h) return;
    int draw_w = (w > ctx->max_w) ? ctx->max_w : w;
    display_draw_rgb565_line(ctx->off_x, ctx->off_y + y, draw_w, rgb565);
}

static void view_decoded_image(const char *path, const char *filename,
                               uint32_t size, int is_png)
{
    uint32_t actual = 0;
    uint8_t *buf = read_file(path, IMG_MAX_FILE, &actual);
    if (!buf || actual < 8) {
        if (buf) free(buf);
        ui_show_error("Cannot read image.");
        ui_wait_for_tap();
        return;
    }

    /* Get image dimensions */
    int img_w = 0, img_h = 0;
    if (is_png) {
        sped_info_t info;
        if (sped_info(buf, actual, &info) != 0) {
            free(buf); ui_show_error("Bad PNG file."); ui_wait_for_tap(); return;
        }
        img_w = (int)info.width; img_h = (int)info.height;
    } else {
        fjpeg_info_t info;
        if (fjpeg_info(buf, actual, &info) != 0) {
            free(buf); ui_show_error("Bad JPEG file."); ui_wait_for_tap(); return;
        }
        img_w = info.width; img_h = info.height;
    }

    /* Center on screen, clip */
    int draw_w = (img_w > DISPLAY_WIDTH) ? DISPLAY_WIDTH : img_w;
    int draw_h = (img_h > DISPLAY_HEIGHT) ? DISPLAY_HEIGHT : img_h;

    struct img_draw_ctx ctx;
    ctx.off_x = (DISPLAY_WIDTH - draw_w) / 2;
    ctx.off_y = (DISPLAY_HEIGHT - draw_h) / 2;
    ctx.max_w = draw_w;
    ctx.max_h = draw_h;

    display_clear(COLOR_BLACK);

    /* Decode and render row-by-row */
    int ok;
    if (is_png)
        ok = sped_decode(buf, actual, img_row_cb, &ctx);
    else
        ok = fjpeg_decode(buf, actual, img_row_cb, &ctx);

    free(buf);

    if (ok != 0) {
        ui_show_error("Decode error.");
        ui_wait_for_tap();
        return;
    }

    /* Overlay filename */
    char tb[34];
    strncpy(tb, filename, 33); tb[33] = '\0';
    int tw = (int)strlen(tb) * FONT_WIDTH;
    display_fill_rect(0, 0, tw + 8, FONT_HEIGHT + 4, COLOR_BLACK);
    display_string(4, 2, tb, COLOR_WHITE, COLOR_BLACK);

    /* Tap to dismiss */
    int tx, ty;
    touch_wait_tap(&tx, &ty);
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

static void open_file(struct file_entry *e)
{
    char full_path[256];
    build_full_path(full_path, sizeof(full_path), e->name);

    if (is_text_file(e->name))
        view_text_file(full_path, e->name, e->size);
    else if (is_bmp_file(e->name))
        view_bmp_file(full_path, e->name, e->size);
    else if (is_png_file(e->name))
        view_decoded_image(full_path, e->name, e->size, 1);
    else if (is_jpg_file(e->name))
        view_decoded_image(full_path, e->name, e->size, 0);
    else
        show_file_info(e);
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
                    open_file(e);
                    display_clear(COLOR_BLACK);
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
