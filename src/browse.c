#include "browse.h"
#include "edit.h"
#include "fs.h"
#include "fb.h"
#include "kbd.h"
#include "mem.h"
#include "iso.h"
#include "disk.h"
#include "fat32.h"

#define MAX_ENTRIES  FS_MAX_ENTRIES
#define MAX_PATH     512

/* Browser state */
static struct fs_entry s_entries[MAX_ENTRIES];
static int s_count;
static int s_cursor;
static int s_scroll;
static CHAR16 s_path[MAX_PATH];

/* USB browsing state */
static int s_on_usb;                                /* browsing a USB volume? */
static int s_usb_vol_idx;                            /* which USB volume */
static struct fs_usb_volume s_usb_vols[FS_MAX_USB];
static int s_usb_count;
static int s_real_count;                             /* fs entries before USB entries */

/* Raw block device state (non-FAT devices shown as [DISK]) */
static struct disk_device s_disk_devs[DISK_MAX_DEVICES];
static int s_disk_count;
static int s_disk_start_idx;

/* Custom volume state (exFAT/NTFS) */
static struct fs_custom_volume s_custom_vols[8];
static int s_custom_count;
static int s_custom_start_idx;
static int s_on_custom;           /* browsing exFAT or NTFS volume? */

/* Close USB volume root handles to avoid UEFI handle leaks */
static void close_usb_handles(void) {
    for (int i = 0; i < s_usb_count; i++) {
        if (s_usb_vols[i].root) {
            s_usb_vols[i].root->Close(s_usb_vols[i].root);
            s_usb_vols[i].root = NULL;
        }
    }
}

/* Copy/paste state */
static CHAR16 s_copy_src[MAX_PATH];
static char s_copy_name[128];

/* Layout constants (computed from g_boot.cols/rows) */
static UINT32 s_list_top;     /* first row of file list */
static UINT32 s_list_rows;    /* number of visible rows */

/* ---- ISO detection ---- */

static int is_iso_file(const char *name) {
    int len = 0;
    while (name[len]) len++;
    if (len < 5) return 0;  /* need at least "x.iso" */
    char c1 = name[len - 4];
    char c2 = name[len - 3];
    char c3 = name[len - 2];
    char c4 = name[len - 1];
    /* Case-insensitive .iso check */
    if (c1 != '.') return 0;
    if (c2 != 'i' && c2 != 'I') return 0;
    if (c3 != 's' && c3 != 'S') return 0;
    if (c4 != 'o' && c4 != 'O') return 0;
    return 1;
}

/* ---- Helpers ---- */

static void uint_to_str(UINT64 n, char *buf) {
    char tmp[24];
    int t = 0;
    if (n == 0) { tmp[t++] = '0'; }
    else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; } }
    int i = 0;
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = '\0';
}

static void format_size(UINT64 size, char *buf) {
    if (size < 1024) {
        uint_to_str(size, buf);
        UINTN len = str_len((CHAR8 *)buf);
        buf[len] = ' '; buf[len+1] = 'B'; buf[len+2] = '\0';
    } else if (size < 1048576) {
        uint_to_str(size / 1024, buf);
        UINTN len = str_len((CHAR8 *)buf);
        buf[len] = ' '; buf[len+1] = 'K'; buf[len+2] = 'B'; buf[len+3] = '\0';
    } else {
        uint_to_str(size / 1048576, buf);
        UINTN len = str_len((CHAR8 *)buf);
        buf[len] = ' '; buf[len+1] = 'M'; buf[len+2] = 'B'; buf[len+3] = '\0';
    }
}

/* Convert CHAR16 path to ASCII for display */
static void path_to_ascii(char *dst, int max) {
    int i = 0;
    while (s_path[i] && i < max - 1) {
        dst[i] = (char)(s_path[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

/* Pad a string with spaces to fill cols width */
static void pad_line(char *line, UINT32 cols) {
    UINTN len = str_len((CHAR8 *)line);
    while (len < cols) {
        line[len] = ' ';
        len++;
    }
    line[cols] = '\0';
}

/* Path manipulation */
static void path_set_root(void) {
    s_path[0] = L'\\';
    s_path[1] = 0;
}

static int path_is_root(void) {
    return s_path[0] == L'\\' && s_path[1] == 0;
}

static void path_append(const char *name) {
    int i = 0;
    while (s_path[i]) i++;
    /* Add separator if not at root */
    if (i > 1 || s_path[0] != L'\\') {
        s_path[i++] = L'\\';
    }
    int j = 0;
    while (name[j] && i < MAX_PATH - 1) {
        s_path[i++] = (CHAR16)name[j++];
    }
    s_path[i] = 0;
}

static void path_up(void) {
    int i = 0;
    while (s_path[i]) i++;
    /* Scan backwards to find last backslash */
    i--;
    while (i > 0 && s_path[i] != L'\\') i--;
    if (i == 0)
        path_set_root();
    else
        s_path[i] = 0;
}

/* ---- Drawing ---- */

static void draw_header(void) {
    char line[256];
    mem_set(line, ' ', g_boot.cols);
    line[g_boot.cols] = '\0';

    /* Copy title into line */
    const char *title = " SURVIVAL FILE BROWSER";
    int i = 0;
    while (title[i] && i < (int)g_boot.cols) {
        line[i] = title[i];
        i++;
    }

    fb_string(0, 0, line, COLOR_CYAN, COLOR_DGRAY);
}

static void draw_path(void) {
    char line[256];
    char path_ascii[256];
    int i = 0;

    line[i++] = ' ';
    if (s_on_custom) {
        enum fs_vol_type vt = fs_get_vol_type();
        const char *tag = (vt == FS_VOL_NTFS) ? "[NTFS] " :
                          (vt == FS_VOL_EXFAT) ? "[exFAT] " : "[USB] ";
        int k = 0;
        while (tag[k] && i < (int)g_boot.cols)
            line[i++] = tag[k++];
    } else if (s_on_usb) {
        const char *tag = "[USB] ";
        int k = 0;
        while (tag[k] && i < (int)g_boot.cols)
            line[i++] = tag[k++];
    }
    path_to_ascii(path_ascii, 240);
    int j = 0;
    while (path_ascii[j] && i < (int)g_boot.cols) {
        line[i++] = path_ascii[j++];
    }
    line[i] = '\0';
    pad_line(line, g_boot.cols);

    fb_string(0, 1, line, COLOR_YELLOW, COLOR_BLACK);
}

static void draw_col_headers(void) {
    char line[256];
    mem_set(line, ' ', g_boot.cols);
    line[g_boot.cols] = '\0';

    const char *hdr = " Name                                    Size";
    int i = 0;
    while (hdr[i] && i < (int)g_boot.cols) {
        line[i] = hdr[i];
        i++;
    }

    fb_string(0, 2, line, COLOR_GRAY, COLOR_BLACK);
}

static void draw_status_msg(const char *msg) {
    char line[256];
    mem_set(line, ' ', g_boot.cols);
    line[g_boot.cols] = '\0';

    if (!msg) {
        /* Show F10:WriteISO when cursor is on a .iso file */
        int on_iso = (s_count > 0 && s_cursor < s_count
                      && !s_entries[s_cursor].is_dir
                      && is_iso_file(s_entries[s_cursor].name));
        int on_disk = (!s_on_usb && !s_on_custom && s_disk_count > 0
                       && s_cursor >= s_disk_start_idx
                       && s_cursor < s_disk_start_idx + s_disk_count);
        int on_usb_entry = (!s_on_usb && !s_on_custom && s_usb_count > 0
                            && s_cursor >= s_real_count
                            && s_cursor < s_real_count + s_usb_count);
        int on_custom_entry = (!s_on_usb && !s_on_custom && s_custom_count > 0
                               && s_cursor >= s_custom_start_idx
                               && s_cursor < s_custom_start_idx + s_custom_count);
        if (on_disk) {
            msg = " ENTER:Format F11:Format                           BS:Back ESC:Exit";
        } else if (on_custom_entry) {
            msg = " ENTER:Open                                        BS:Back ESC:Exit";
        } else if (on_usb_entry) {
            msg = " ENTER:Open F11:Format                             BS:Back ESC:Exit";
        } else if (s_on_custom && fs_is_read_only()) {
            if (on_iso)
                msg = " ENTER:Open F3:Copy F10:WriteISO               BS:Back";
            else
                msg = " ENTER:Open F3:Copy                            BS:Back";
        } else if (s_on_custom) {
            if (on_iso)
                msg = " ENTER:Open F3:Copy F10:WriteISO               BS:Back";
            else
                msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename  BS:Back";
        } else if (s_on_usb) {
            if (on_iso)
                msg = " ENTER:Open F3:Copy F10:WriteISO F12:Clone BS:Back";
            else
                msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename F12:Clone BS:Back";
        } else {
            if (on_iso)
                msg = " ENTER:Open F3:Copy F10:WriteISO BS:Back ESC:Exit";
            else
                msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename BS:Back ESC:Exit";
        }
    }

    int i = 0;
    while (msg[i] && i < (int)g_boot.cols) {
        line[i] = msg[i];
        i++;
    }

    fb_string(0, g_boot.rows - 1, line, COLOR_GRAY, COLOR_DGRAY);
}

static void draw_status(void) {
    draw_status_msg(NULL);
}

static void draw_entry_line(int list_idx) {
    int entry_idx = s_scroll + list_idx;
    UINT32 row = s_list_top + (UINT32)list_idx;
    char line[256];

    mem_set(line, ' ', g_boot.cols);
    line[g_boot.cols] = '\0';

    if (entry_idx >= s_count) {
        /* Empty line */
        fb_string(0, row, line, COLOR_WHITE, COLOR_BLACK);
        return;
    }

    struct fs_entry *e = &s_entries[entry_idx];
    int pos = 1;  /* start at column 1 for padding */

    if (e->is_dir) {
        const char *dir_tag = "[DIR] ";
        int k = 0;
        while (dir_tag[k] && pos < (int)g_boot.cols)
            line[pos++] = dir_tag[k++];
    }

    /* Name */
    int k = 0;
    int name_limit = (int)g_boot.cols - 15;  /* leave room for size column */
    while (e->name[k] && pos < name_limit)
        line[pos++] = e->name[k++];

    /* Size (right-aligned area, skip for directories) */
    if (!e->is_dir) {
        char size_str[24];
        format_size(e->size, size_str);
        UINTN slen = str_len((CHAR8 *)size_str);
        int size_col = (int)g_boot.cols - (int)slen - 2;
        if (size_col > pos) {
            for (UINTN s = 0; s < slen; s++)
                line[size_col + (int)s] = size_str[s];
        }
    }

    /* Choose colors based on selection */
    UINT32 fg, bg;
    int is_usb_entry = (!s_on_usb && !s_on_custom && entry_idx >= s_real_count
                        && entry_idx < s_real_count + s_usb_count);
    int is_custom_entry = (!s_on_usb && !s_on_custom && s_custom_count > 0
                           && entry_idx >= s_custom_start_idx
                           && entry_idx < s_custom_start_idx + s_custom_count);
    int is_disk_entry = (!s_on_usb && !s_on_custom && s_disk_count > 0
                         && entry_idx >= s_disk_start_idx
                         && entry_idx < s_disk_start_idx + s_disk_count);
    if (entry_idx == s_cursor) {
        fg = COLOR_BLACK;
        bg = COLOR_CYAN;
    } else if (is_disk_entry) {
        fg = COLOR_RED;
        bg = COLOR_BLACK;
    } else if (is_custom_entry) {
        /* Color by volume type: orange for exFAT, magenta for NTFS */
        int ci = entry_idx - s_custom_start_idx;
        fg = (s_custom_vols[ci].type == FS_VOL_NTFS) ? COLOR_MAGENTA : COLOR_ORANGE;
        bg = COLOR_BLACK;
    } else if (is_usb_entry) {
        fg = COLOR_ORANGE;
        bg = COLOR_BLACK;
    } else if (e->is_dir) {
        fg = COLOR_GREEN;
        bg = COLOR_BLACK;
    } else {
        fg = COLOR_WHITE;
        bg = COLOR_BLACK;
    }

    fb_string(0, row, line, fg, bg);
}

static void draw_list(void) {
    for (UINT32 i = 0; i < s_list_rows; i++)
        draw_entry_line((int)i);
}

static void draw_all(void) {
    fb_clear(COLOR_BLACK);
    draw_header();
    draw_path();
    draw_col_headers();
    draw_list();
    draw_status();
}

/* ---- Directory loading ---- */

static void load_dir(void) {
    s_count = fs_readdir(s_path, s_entries, MAX_ENTRIES);
    if (s_count < 0) s_count = 0;
    s_real_count = s_count;

    /* Append USB volume entries at boot volume root */
    if (!s_on_usb && !s_on_custom && path_is_root()) {
        close_usb_handles();  /* close any previously opened USB handles */
        s_usb_count = fs_enumerate_usb(s_usb_vols, FS_MAX_USB);

        /* Validate each USB volume — remove those with destroyed FAT32.
           UEFI caches SFS protocol, so a device whose FAT32 was overwritten
           (e.g. by an ISO write) may still appear as a valid volume. */
        for (int i = 0; i < s_usb_count; ) {
            if (!fs_has_valid_fat32(s_usb_vols[i].handle)) {
                if (s_usb_vols[i].root) {
                    s_usb_vols[i].root->Close(s_usb_vols[i].root);
                    s_usb_vols[i].root = NULL;
                }
                for (int j = i; j < s_usb_count - 1; j++)
                    s_usb_vols[j] = s_usb_vols[j + 1];
                s_usb_count--;
            } else {
                i++;
            }
        }

        for (int i = 0; i < s_usb_count && s_count < MAX_ENTRIES; i++) {
            struct fs_entry *e = &s_entries[s_count];
            /* Build "[USB] label" display name */
            int pos = 0;
            const char *tag = "[USB] ";
            while (tag[pos] && pos < FS_MAX_NAME - 2)
                e->name[pos] = tag[pos], pos++;
            int j = 0;
            while (s_usb_vols[i].label[j] && pos < FS_MAX_NAME - 1)
                e->name[pos++] = s_usb_vols[i].label[j++];
            e->name[pos] = '\0';
            e->size = 0;
            e->is_dir = 1;
            s_count++;
        }

        /* Enumerate exFAT/NTFS volumes */
        s_custom_count = 0;
        s_custom_start_idx = s_count;
        s_custom_count = fs_enumerate_custom_volumes(s_custom_vols, 8);

        for (int i = 0; i < s_custom_count && s_count < MAX_ENTRIES; i++) {
            struct fs_entry *e = &s_entries[s_count];
            int pos = 0;
            const char *tag = (s_custom_vols[i].type == FS_VOL_NTFS)
                              ? "[NTFS] " : "[exFAT] ";
            while (*tag && pos < FS_MAX_NAME - 2)
                e->name[pos++] = *tag++;
            int j = 0;
            while (s_custom_vols[i].label[j] && pos < FS_MAX_NAME - 1)
                e->name[pos++] = s_custom_vols[i].label[j++];
            e->name[pos] = '\0';
            e->size = s_custom_vols[i].size_bytes;
            e->is_dir = 1;
            s_count++;
        }

        /* Enumerate raw block devices (no filesystem).
         * disk_enumerate returns whole-disk handles, but USB/exFAT/NTFS
         * volumes are partition handles. Build a list of all claimed
         * partition handles so we can skip any whole disk that contains
         * an already-listed partition (device path prefix matching). */
        s_disk_count = 0;
        s_disk_start_idx = s_count;
        struct disk_device all_devs[DISK_MAX_DEVICES];
        int ndevs = disk_enumerate(all_devs, DISK_MAX_DEVICES);

        /* Collect claimed partition handles: USB volumes + custom volumes */
        EFI_HANDLE claimed[FS_MAX_USB + 8];
        int nclaimed = 0;
        for (int ci = 0; ci < s_usb_count && nclaimed < FS_MAX_USB + 8; ci++)
            claimed[nclaimed++] = s_usb_vols[ci].handle;
        for (int ci = 0; ci < s_custom_count && nclaimed < FS_MAX_USB + 8; ci++)
            claimed[nclaimed++] = s_custom_vols[ci].handle;

        EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        for (int i = 0; i < ndevs && s_count < MAX_ENTRIES; i++) {
            if (all_devs[i].is_boot_device) continue;

            /* Skip devices that have SimpleFileSystem AND valid FAT32.
               If SFS exists but FAT32 is invalid (stale cache), show as [DISK]. */
            void *sfs = NULL;
            EFI_STATUS st = g_boot.bs->HandleProtocol(
                all_devs[i].handle, &sfs_guid, &sfs);
            if (!EFI_ERROR(st) && sfs && fs_has_valid_fat32(all_devs[i].handle))
                continue;

            /* Skip whole-disk devices that have a partition already
               listed as a USB, exFAT, or NTFS volume */
            if (disk_has_claimed_partition(all_devs[i].handle,
                                           claimed, nclaimed))
                continue;

            /* This is a raw block device — show as [DISK] */
            s_disk_devs[s_disk_count] = all_devs[i];
            struct fs_entry *e = &s_entries[s_count];
            int pos = 0;
            const char *tag = "[DISK] ";
            while (tag[pos] && pos < FS_MAX_NAME - 2)
                e->name[pos] = tag[pos], pos++;
            int j = 0;
            while (all_devs[i].name[j] && pos < FS_MAX_NAME - 1)
                e->name[pos++] = all_devs[i].name[j++];
            e->name[pos] = '\0';
            e->size = all_devs[i].size_bytes;
            e->is_dir = 0;
            s_count++;
            s_disk_count++;
        }
    } else if (!s_on_usb && !s_on_custom) {
        close_usb_handles();  /* close USB handles when not at root */
        s_usb_count = 0;
        s_custom_count = 0;
        s_disk_count = 0;
    }

    s_cursor = 0;
    s_scroll = 0;
}

/* ---- Scroll clamping ---- */

static void clamp_scroll(void) {
    /* Ensure cursor is visible */
    if (s_cursor < s_scroll)
        s_scroll = s_cursor;
    if (s_cursor >= s_scroll + (int)s_list_rows)
        s_scroll = s_cursor - (int)s_list_rows + 1;
    if (s_scroll < 0)
        s_scroll = 0;
}

/* ---- Open file in editor ---- */

static void open_file(void) {
    if (s_count <= 0 || s_entries[s_cursor].is_dir)
        return;
    edit_run(s_path, s_entries[s_cursor].name);
}

/* ---- New file prompt ---- */

static void prompt_new_file(void) {
    char name[128];
    int len = 0;
    name[0] = '\0';

    /* Draw prompt on status bar */
    char line[256];
    for (;;) {
        int i = 0;
        const char *prompt = " New filename: ";
        while (prompt[i] && i < (int)g_boot.cols) {
            line[i] = prompt[i];
            i++;
        }
        int j = 0;
        while (j < len && i < (int)g_boot.cols) {
            line[i++] = name[j++];
        }
        /* Cursor */
        if (i < (int)g_boot.cols)
            line[i] = '_';
        i++;
        line[i] = '\0';
        pad_line(line, g_boot.cols);
        fb_string(0, g_boot.rows - 1, line, COLOR_WHITE, COLOR_DGRAY);

        struct key_event ev;
        kbd_wait(&ev);

        if (ev.code == KEY_ESC) {
            return;  /* cancelled */
        } else if (ev.code == KEY_ENTER) {
            if (len > 0)
                break;  /* got a name */
        } else if (ev.code == KEY_BS) {
            if (len > 0) {
                len--;
                name[len] = '\0';
            }
        } else if (ev.code >= 0x20 && ev.code <= 0x7E && len < 120) {
            name[len++] = (char)ev.code;
            name[len] = '\0';
        }
    }

    edit_run(s_path, name);
}

/* ---- Copy/Paste ---- */

static void do_copy(void) {
    if (s_count <= 0) return;
    struct fs_entry *e = &s_entries[s_cursor];
    if (e->is_dir) return;

    /* Build full source path */
    int i = 0;
    while (s_path[i] && i < MAX_PATH - 1) {
        s_copy_src[i] = s_path[i];
        i++;
    }
    if (i > 1 || s_copy_src[0] != L'\\')
        s_copy_src[i++] = L'\\';
    int j = 0;
    while (e->name[j] && i < MAX_PATH - 1) {
        s_copy_src[i++] = (CHAR16)e->name[j++];
    }
    s_copy_src[i] = 0;

    /* Copy name */
    int k = 0;
    while (e->name[k] && k < 127) {
        s_copy_name[k] = e->name[k];
        k++;
    }
    s_copy_name[k] = '\0';

    /* Show confirmation */
    char msg[256];
    int p = 0;
    const char *prefix = " Copied: ";
    while (prefix[p]) msg[p] = prefix[p], p++;
    k = 0;
    while (s_copy_name[k] && p < 250) msg[p++] = s_copy_name[k++];
    msg[p] = '\0';
    draw_status_msg(msg);
}

/* Case-insensitive name comparison (for FAT32) */
static int names_equal(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Check if a name already exists in current directory listing */
static int name_exists(const char *name) {
    for (int i = 0; i < s_count; i++)
        if (names_equal(name, s_entries[i].name))
            return 1;
    return 0;
}

/* Generate a unique paste name: hello.c → hello_2.c → hello_3.c ... */
static void make_unique_name(char *out, const char *src, int max) {
    /* Try original name first */
    int len = 0;
    while (src[len] && len < max - 1) { out[len] = src[len]; len++; }
    out[len] = '\0';
    if (!name_exists(out)) return;

    /* Find extension (last dot) */
    int dot = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (src[i] == '.') { dot = i; break; }
    }

    for (int n = 2; n < 100; n++) {
        int p = 0;
        int base_end = (dot >= 0) ? dot : len;
        for (int i = 0; i < base_end && p < max - 12; i++)
            out[p++] = src[i];
        out[p++] = '_';
        if (n >= 10) out[p++] = '0' + (n / 10);
        out[p++] = '0' + (n % 10);
        if (dot >= 0) {
            for (int i = dot; src[i] && p < max - 1; i++)
                out[p++] = src[i];
        }
        out[p] = '\0';
        if (!name_exists(out)) return;
    }
}

/* Returns 0 on success, -1 on failure */
static int do_paste(void) {
    if (s_copy_name[0] == '\0') {
        draw_status_msg(" Nothing to paste");
        return -1;
    }

    /* Read source file */
    UINTN file_size;
    void *data = fs_readfile(s_copy_src, &file_size);
    if (!data) {
        draw_status_msg(" Paste failed: cannot read source");
        return -1;
    }

    /* Check disk space */
    UINT64 total_bytes, free_bytes;
    if (fs_volume_info(&total_bytes, &free_bytes) == 0) {
        if ((UINT64)file_size > free_bytes) {
            mem_free(data);
            draw_status_msg(" Paste failed: not enough disk space");
            return -1;
        }
    }

    /* Generate unique destination name */
    char dest_name[128];
    make_unique_name(dest_name, s_copy_name, 128);

    /* Build full destination path */
    CHAR16 dest[MAX_PATH];
    int i = 0;
    while (s_path[i] && i < MAX_PATH - 1) {
        dest[i] = s_path[i];
        i++;
    }
    if (i > 1 || dest[0] != L'\\')
        dest[i++] = L'\\';
    int j = 0;
    while (dest_name[j] && i < MAX_PATH - 1) {
        dest[i++] = (CHAR16)dest_name[j++];
    }
    dest[i] = 0;

    EFI_STATUS status = fs_writefile(dest, data, file_size);
    mem_free(data);

    if (EFI_ERROR(status)) {
        draw_status_msg(" Paste failed: write error");
        return -1;
    }

    /* Show what name was used */
    char msg[256];
    int p = 0;
    const char *prefix = " Pasted: ";
    while (prefix[p]) { msg[p] = prefix[p]; p++; }
    j = 0;
    while (dest_name[j] && p < 250) msg[p++] = dest_name[j++];
    msg[p] = '\0';
    draw_status_msg(msg);
    return 0;
}

static void do_rename(void) {
    if (s_count <= 0) return;
    struct fs_entry *e = &s_entries[s_cursor];

    /* Pre-fill with current name */
    char name[128];
    int len = 0;
    while (e->name[len] && len < 127) {
        name[len] = e->name[len];
        len++;
    }
    name[len] = '\0';

    /* Prompt on status bar */
    char line[256];
    for (;;) {
        int i = 0;
        const char *prompt = " Rename: ";
        while (prompt[i] && i < (int)g_boot.cols) {
            line[i] = prompt[i];
            i++;
        }
        int j = 0;
        while (j < len && i < (int)g_boot.cols) {
            line[i++] = name[j++];
        }
        if (i < (int)g_boot.cols)
            line[i] = '_';
        i++;
        line[i] = '\0';
        pad_line(line, g_boot.cols);
        fb_string(0, g_boot.rows - 1, line, COLOR_WHITE, COLOR_DGRAY);

        struct key_event ev;
        kbd_wait(&ev);

        if (ev.code == KEY_ESC) {
            return;
        } else if (ev.code == KEY_ENTER) {
            if (len > 0) break;
        } else if (ev.code == KEY_BS) {
            if (len > 0) { len--; name[len] = '\0'; }
        } else if (ev.code >= 0x20 && ev.code <= 0x7E && len < 120) {
            name[len++] = (char)ev.code;
            name[len] = '\0';
        }
    }

    /* Skip if name unchanged */
    if (names_equal(name, e->name)) return;

    /* Build full old path */
    CHAR16 old_path[MAX_PATH];
    int i = 0;
    while (s_path[i] && i < MAX_PATH - 1) {
        old_path[i] = s_path[i];
        i++;
    }
    if (i > 1 || old_path[0] != L'\\')
        old_path[i++] = L'\\';
    int j = 0;
    while (e->name[j] && i < MAX_PATH - 1) {
        old_path[i++] = (CHAR16)e->name[j++];
    }
    old_path[i] = 0;

    /* Convert new name to CHAR16 */
    CHAR16 new_name_w[128];
    int k = 0;
    while (name[k] && k < 127) {
        new_name_w[k] = (CHAR16)name[k];
        k++;
    }
    new_name_w[k] = 0;

    EFI_STATUS status = fs_rename(old_path, new_name_w);
    if (EFI_ERROR(status))
        draw_status_msg(" Rename failed");
}

/* ---- USB clone ---- */

/* USB root handle used during clone — set before calling recursive copy */
static EFI_FILE_HANDLE s_clone_usb_root;

/* Recursively copy files from boot volume to USB volume using UEFI SFS.
   Caller must be on boot volume (fs_restore_boot_volume). */
static int clone_copy_recursive(const CHAR16 *src_path, const CHAR16 *dst_path) {
    /* Read source directory from boot volume */
    fs_restore_boot_volume();
    struct fs_entry entries[64];
    int n = fs_readdir(src_path, entries, 64);
    if (n < 0) return 0;

    for (int i = 0; i < n; i++) {
        /* Build source path (CHAR16) */
        CHAR16 src[MAX_PATH];
        int si = 0;
        while (src_path[si] && si < MAX_PATH - 1) { src[si] = src_path[si]; si++; }
        if (si > 1 || src[0] != L'\\') src[si++] = L'\\';
        int j = 0;
        while (entries[i].name[j] && si < MAX_PATH - 1)
            src[si++] = (CHAR16)entries[i].name[j++];
        src[si] = 0;

        /* Build destination path (CHAR16) */
        CHAR16 dst[MAX_PATH];
        int di = 0;
        while (dst_path[di] && di < MAX_PATH - 1) { dst[di] = dst_path[di]; di++; }
        if (di > 1 || dst[0] != L'\\') dst[di++] = L'\\';
        j = 0;
        while (entries[i].name[j] && di < MAX_PATH - 1)
            dst[di++] = (CHAR16)entries[i].name[j++];
        dst[di] = 0;

        if (entries[i].is_dir) {
            /* Create directory on USB via SFS */
            fs_set_volume(s_clone_usb_root);
            fs_mkdir(dst);
            fs_restore_boot_volume();
            clone_copy_recursive(src, dst);
        } else {
            /* Show progress */
            char msg[256];
            int mp = 0;
            const char *pre = " Copying ";
            while (pre[mp] && mp < 240) { msg[mp] = pre[mp]; mp++; }
            j = 0;
            while (dst[j] && mp < 250) msg[mp++] = (char)(dst[j++] & 0x7F);
            msg[mp] = '\0';
            draw_status_msg(msg);

            /* Read from boot volume */
            fs_restore_boot_volume();
            UINTN file_size;
            void *data = fs_readfile(src, &file_size);
            if (data) {
                /* Write to USB volume via SFS */
                fs_set_volume(s_clone_usb_root);
                fs_writefile(dst, data, file_size);
                fs_restore_boot_volume();
                mem_free(data);
            }
        }
    }
    return 0;
}

static void clone_to_usb(void) {
    if (!s_on_usb || s_usb_vol_idx < 0 || s_usb_vol_idx >= s_usb_count)
        return;

    /* Warning screen */
    fb_clear(COLOR_BLACK);
    fb_print("\n", COLOR_WHITE);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("       CLONE WORKSTATION TO USB\n", COLOR_CYAN);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("\n", COLOR_WHITE);
    fb_print("  This will copy the entire boot volume\n", COLOR_YELLOW);
    fb_print("  onto the USB drive, creating a bootable\n", COLOR_YELLOW);
    fb_print("  Survival Workstation clone.\n", COLOR_YELLOW);
    fb_print("\n", COLOR_WHITE);
    fb_print("  Press 'Y' to proceed, any other key to cancel.\n", COLOR_YELLOW);

    struct key_event ev;
    kbd_wait(&ev);
    if (ev.code != 'Y' && ev.code != 'y') {
        fb_print("\n  Cancelled.\n", COLOR_WHITE);
        fb_print("  Press any key to return.\n", COLOR_WHITE);
        kbd_wait(&ev);
        return;
    }

    /* Save USB root handle, then switch to boot volume for reading */
    s_clone_usb_root = s_usb_vols[s_usb_vol_idx].root;
    fs_restore_boot_volume();

    /* Recursively copy boot volume to USB via UEFI SFS */
    fb_print("\n  Copying files...\n", COLOR_WHITE);
    CHAR16 root_path[2] = { L'\\', 0 };
    clone_copy_recursive(root_path, root_path);

    fb_print("\n\n", COLOR_WHITE);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("    BOOTABLE USB CLONE CREATED!\n", COLOR_GREEN);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("\n", COLOR_WHITE);
    fb_print("  The USB drive is now a bootable copy of\n", COLOR_WHITE);
    fb_print("  the Survival Workstation.\n", COLOR_WHITE);
    fb_print("\n", COLOR_WHITE);
    fb_print("  Press any key to return.\n", COLOR_WHITE);
    kbd_wait(&ev);

    /* Return to boot volume root */
    s_on_usb = 0;
    path_set_root();
}

/* ---- Format block device ---- */

/* Find the whole-disk block device underlying a USB volume.
   Returns 0 on success and populates *out, or -1 if not found. */
static int find_disk_for_usb(int usb_idx, struct disk_device *out) {
    EFI_HANDLE vol_handle = s_usb_vols[usb_idx].handle;

    struct disk_device devs[DISK_MAX_DEVICES];
    int ndevs = disk_enumerate(devs, DISK_MAX_DEVICES);

    /* Case 1: direct handle match (superfloppy — no partition table) */
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].handle == vol_handle && !devs[i].is_boot_device) {
            *out = devs[i];
            return 0;
        }
    }

    /* Case 2: USB volume is a partition on the disk */
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO *vol_bio = NULL;
    g_boot.bs->HandleProtocol(vol_handle, &bio_guid, (VOID **)&vol_bio);
    if (vol_bio && vol_bio->Media && vol_bio->Media->LogicalPartition) {
        UINT64 vol_size = (UINT64)(vol_bio->Media->LastBlock + 1) *
                          (UINT64)vol_bio->Media->BlockSize;
        for (int i = 0; i < ndevs; i++) {
            if (devs[i].is_removable && !devs[i].is_boot_device
                && devs[i].size_bytes >= vol_size) {
                *out = devs[i];
                return 0;
            }
        }
    }

    return -1;
}

static void do_format_disk(struct disk_device *dev) {

    fb_clear(COLOR_BLACK);
    fb_print("\n", COLOR_WHITE);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("       FORMAT DEVICE AS FAT32\n", COLOR_CYAN);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("\n", COLOR_WHITE);

    /* Show device info */
    fb_print("  Device: ", COLOR_WHITE);
    fb_print(dev->name, COLOR_WHITE);
    fb_print("\n\n", COLOR_WHITE);
    fb_print("  This will ERASE all data on this device!\n", COLOR_RED);
    fb_print("  A new FAT32 filesystem will be created.\n", COLOR_YELLOW);
    fb_print("\n", COLOR_WHITE);
    fb_print("  Press 'Y' to proceed, any other key to cancel.\n", COLOR_YELLOW);

    struct key_event ev;
    kbd_wait(&ev);
    if (ev.code != 'Y' && ev.code != 'y') return;

    fb_print("\n  Formatting...\n", COLOR_WHITE);

    int rc = fat32_format(dev);

    /* Force firmware to re-probe — pick up the new FAT32 */
    disk_reconnect(dev);

    if (rc < 0) {
        fb_print("  Format FAILED.\n", COLOR_RED);
    } else {
        fb_print("\n", COLOR_WHITE);
        fb_print("  ========================================\n", COLOR_GREEN);
        fb_print("    FORMAT COMPLETE!\n", COLOR_GREEN);
        fb_print("  ========================================\n", COLOR_GREEN);
        fb_print("\n", COLOR_WHITE);
        fb_print("  The device is now FAT32.\n", COLOR_WHITE);
        fb_print("  It will appear as a [USB] volume.\n", COLOR_WHITE);
    }

    fb_print("\n  Press any key to return.\n", COLOR_DGRAY);
    kbd_wait(&ev);
}

/* ---- Main browser loop ---- */

void browse_run(void) {
    /* Init layout */
    s_list_top = 3;
    s_list_rows = g_boot.rows - 4;  /* header + path + colhdr + status */

    /* Init filesystem */
    EFI_STATUS status = fs_init();
    if (EFI_ERROR(status)) {
        fb_print("Error: Could not initialize filesystem\n", COLOR_RED);
        return;
    }

    /* Start at root */
    path_set_root();
    load_dir();
    draw_all();

    struct key_event ev;
    for (;;) {
        kbd_wait(&ev);

        int old_cursor = s_cursor;

        switch (ev.code) {
        case KEY_UP:
            if (s_cursor > 0) {
                s_cursor--;
                int old_scroll = s_scroll;
                clamp_scroll();
                if (old_scroll != s_scroll) {
                    draw_list();
                } else {
                    draw_entry_line(old_cursor - s_scroll);
                    draw_entry_line(s_cursor - s_scroll);
                }
            }
            break;

        case KEY_DOWN:
            if (s_cursor < s_count - 1) {
                s_cursor++;
                int prev_scroll = s_scroll;
                clamp_scroll();
                if (prev_scroll != s_scroll) {
                    draw_list();
                } else {
                    draw_entry_line(old_cursor - s_scroll);
                    draw_entry_line(s_cursor - s_scroll);
                }
            }
            break;

        case KEY_PGUP:
            s_cursor -= (int)s_list_rows;
            if (s_cursor < 0) s_cursor = 0;
            clamp_scroll();
            draw_list();
            break;

        case KEY_PGDN:
            s_cursor += (int)s_list_rows;
            if (s_cursor >= s_count) s_cursor = s_count - 1;
            if (s_cursor < 0) s_cursor = 0;
            clamp_scroll();
            draw_list();
            break;

        case KEY_HOME:
            s_cursor = 0;
            s_scroll = 0;
            draw_list();
            break;

        case KEY_END:
            s_cursor = s_count - 1;
            if (s_cursor < 0) s_cursor = 0;
            clamp_scroll();
            draw_list();
            break;

        case KEY_ENTER:
            if (!s_on_usb && !s_on_custom && s_disk_count > 0
                && s_cursor >= s_disk_start_idx
                && s_cursor < s_disk_start_idx + s_disk_count) {
                do_format_disk(&s_disk_devs[s_cursor - s_disk_start_idx]);
                load_dir();
                draw_all();
            } else if (!s_on_usb && !s_on_custom && s_custom_count > 0
                       && s_cursor >= s_custom_start_idx
                       && s_cursor < s_custom_start_idx + s_custom_count) {
                /* Entering an exFAT/NTFS volume */
                int ci = s_cursor - s_custom_start_idx;
                if (fs_set_custom_volume(s_custom_vols[ci].type,
                                          s_custom_vols[ci].handle) == 0) {
                    s_on_custom = 1;
                    path_set_root();
                    load_dir();
                    draw_all();
                } else {
                    draw_status_msg(" Failed to mount volume");
                }
            } else if (!s_on_usb && !s_on_custom && s_cursor >= s_real_count
                       && s_usb_count > 0
                       && s_cursor < s_real_count + s_usb_count) {
                /* Entering a USB volume */
                s_usb_vol_idx = s_cursor - s_real_count;
                s_on_usb = 1;
                fs_set_volume(s_usb_vols[s_usb_vol_idx].root);
                path_set_root();
                load_dir();
                draw_all();
            } else if (s_count > 0 && s_entries[s_cursor].is_dir) {
                path_append(s_entries[s_cursor].name);
                load_dir();
                draw_all();
            } else if (s_count > 0) {
                open_file();
                load_dir();  /* refresh — file may have been created/changed */
                draw_all();
            }
            break;

        case KEY_F3:
            do_copy();
            break;

        case KEY_F4:
            if (fs_is_read_only()) {
                draw_status_msg(" Volume is read-only");
                break;
            }
            prompt_new_file();
            load_dir();
            draw_all();
            break;

        case KEY_F8:
            if (fs_is_read_only()) {
                draw_status_msg(" Volume is read-only");
                break;
            }
            if (do_paste() == 0) {
                load_dir();
                draw_all();
            }
            break;

        case KEY_F9:
            if (fs_is_read_only()) {
                draw_status_msg(" Volume is read-only");
                break;
            }
            do_rename();
            load_dir();
            draw_all();
            break;

        case KEY_F10:
            if (s_count > 0 && !s_entries[s_cursor].is_dir
                && is_iso_file(s_entries[s_cursor].name)) {
                /* Build full CHAR16 path to the ISO file */
                CHAR16 iso_path[MAX_PATH];
                int ip = 0;
                while (s_path[ip] && ip < MAX_PATH - 1) {
                    iso_path[ip] = s_path[ip];
                    ip++;
                }
                if (ip > 1 || iso_path[0] != L'\\')
                    iso_path[ip++] = L'\\';
                int ij = 0;
                while (s_entries[s_cursor].name[ij] && ip < MAX_PATH - 1)
                    iso_path[ip++] = (CHAR16)s_entries[s_cursor].name[ij++];
                iso_path[ip] = 0;

                /* Determine volume root and handle */
                EFI_FILE_HANDLE vol_root = NULL;
                EFI_HANDLE vol_handle = NULL;
                if (s_on_usb && s_usb_vol_idx >= 0 && s_usb_vol_idx < s_usb_count) {
                    vol_root = s_usb_vols[s_usb_vol_idx].root;
                    vol_handle = s_usb_vols[s_usb_vol_idx].handle;
                } else {
                    vol_root = fs_get_boot_root();
                    vol_handle = NULL; /* boot volume — never same as target */
                }

                iso_write(vol_root, iso_path,
                          s_entries[s_cursor].name,
                          s_entries[s_cursor].size,
                          vol_handle);
                load_dir();
                draw_all();
            }
            break;

        case KEY_F11:
            if (!s_on_usb && s_disk_count > 0
                && s_cursor >= s_disk_start_idx
                && s_cursor < s_disk_start_idx + s_disk_count) {
                /* Format a raw [DISK] entry */
                do_format_disk(&s_disk_devs[s_cursor - s_disk_start_idx]);
                load_dir();
                draw_all();
            } else if (!s_on_usb && s_usb_count > 0
                       && s_cursor >= s_real_count
                       && s_cursor < s_real_count + s_usb_count) {
                /* Format a [USB] entry — find underlying block device */
                int usb_idx = s_cursor - s_real_count;
                struct disk_device dev;
                if (find_disk_for_usb(usb_idx, &dev) == 0) {
                    /* Close the volume handle before destroying its filesystem */
                    if (s_usb_vols[usb_idx].root) {
                        s_usb_vols[usb_idx].root->Close(s_usb_vols[usb_idx].root);
                        s_usb_vols[usb_idx].root = NULL;
                    }
                    do_format_disk(&dev);
                    load_dir();
                    draw_all();
                }
            }
            break;

        case KEY_F12:
            if (s_on_usb) {
                clone_to_usb();
                load_dir();
                draw_all();
            }
            break;

        case KEY_BS:
            if (s_on_custom && path_is_root()) {
                /* Leave custom volume, return to boot root */
                fs_restore_boot_volume();
                s_on_custom = 0;
                path_set_root();
                load_dir();
                draw_all();
            } else if (s_on_usb && path_is_root()) {
                /* Leave USB volume, return to boot root */
                fs_restore_boot_volume();
                s_on_usb = 0;
                path_set_root();
                load_dir();
                draw_all();
            } else if (!path_is_root()) {
                path_up();
                load_dir();
                draw_all();
            }
            break;

        case KEY_ESC:
            if (s_on_custom && path_is_root()) {
                /* Leave custom volume, return to boot root */
                fs_restore_boot_volume();
                s_on_custom = 0;
                path_set_root();
                load_dir();
                draw_all();
            } else if (s_on_usb && path_is_root()) {
                /* Leave USB volume, return to boot root */
                fs_restore_boot_volume();
                s_on_usb = 0;
                path_set_root();
                load_dir();
                draw_all();
            } else if (!path_is_root()) {
                path_up();
                load_dir();
                draw_all();
            } else {
                return;  /* exit browser */
            }
            break;
        }

        /* Update status bar after cursor/state changes */
        draw_status();
    }
}
