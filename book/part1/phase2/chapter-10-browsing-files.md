---
layout: default
title: "Chapter 10: Browsing Files"
parent: "Phase 2: Filesystem & Editor"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 2
---

# Chapter 10: Browsing Files

## From API to Interface

Chapter 9 gave us three filesystem functions: `fs_init` to open the boot volume, `fs_readdir` to list a directory's contents, and `fs_readfile` to load a file into memory. Those are building blocks. A human cannot use them directly -- they accept CHAR16 paths and return raw structs. We need to turn them into something interactive: a full-screen file browser with keyboard navigation, a file viewer, and basic file management operations.

This is the first time our application looks like an actual application instead of a demo. The file browser becomes the workstation's primary interface -- the place users go to find files, organize them, and eventually open them in the editor. It needs to be responsive, easy to navigate, and tolerant of the limited filesystem we are running on. We will build it incrementally: first the display and navigation, then a file viewer, then copy/paste and rename support.

The design follows a pattern common to all our UI modules: one function takes over the screen, runs its own event loop, and returns when the user is done. No callbacks, no event system, no framework. Just a loop.

## The Module

Create `src/browse.h`:

```c
#ifndef BROWSE_H
#define BROWSE_H

#include "boot.h"

/* Launch the file browser. Returns when user presses ESC at root. */
void browse_run(void);

#endif /* BROWSE_H */
```

One function. The browser takes over the entire screen when called and returns only when the user presses ESC at the root directory. The caller does not need to know anything about browser internals -- the directory listing, cursor position, scroll state, copy buffer, and all drawing are encapsulated inside `browse.c`. This is the same pattern we used for every module: a minimal header exposing only what other modules need.

The `boot.h` include provides the UEFI types (`CHAR16`, `UINT32`, etc.) that internal functions will use. External callers just see `void browse_run(void)`.

## State

Create `src/browse.c` and start with the includes and constants:

```c
#include "browse.h"
#include "edit.h"
#include "fs.h"
#include "fb.h"
#include "kbd.h"
#include "mem.h"

#define MAX_ENTRIES  FS_MAX_ENTRIES
#define MAX_PATH     512
```

We pull in every module we have built so far. The editor module (`edit.h`) is included because the browser's ENTER key will eventually hand files off to the editor. `MAX_ENTRIES` inherits the filesystem's limit (256 entries per directory). `MAX_PATH` is the buffer size for our CHAR16 path -- 512 characters is far more than FAT32's actual path limit.

Now the state variables:

```c
static struct fs_entry s_entries[MAX_ENTRIES];
static int s_count;
static int s_cursor;
static int s_scroll;
static CHAR16 s_path[MAX_PATH];

static UINT32 s_list_top;
static UINT32 s_list_rows;

static CHAR16 s_copy_src[MAX_PATH];
static char s_copy_name[128];
```

All state is module-level static -- no dynamic allocation for browser state. Everything lives in the BSS segment, which the UEFI loader zeroes before our code runs. The variables break down into four groups:

`s_entries` is a flat array holding the current directory listing. Each entry contains a name (ASCII, up to 128 characters), a file size, and a directory flag. `s_count` tracks how many entries are valid in this array. The filesystem module fills this array via `fs_readdir`, which sorts directories first and then alphabetically.

`s_cursor` is which entry is currently selected (0-indexed from the top of the directory). `s_scroll` is the index of the first visible entry. When the directory has more entries than the screen can display, `s_scroll` advances to keep the cursor visible. The relationship is simple: visible entries are `s_entries[s_scroll]` through `s_entries[s_scroll + s_list_rows - 1]`, and the cursor is always within that range.

`s_path` is the current directory path as CHAR16, because UEFI's filesystem protocol requires wide strings. In UEFI, the root is `\` (a single backslash). A subdirectory is `\EFI\BOOT`. We manipulate this path with helper functions that handle separator logic.

`s_list_top` and `s_list_rows` define the file listing area on screen. The top three rows are reserved for chrome (header, path, column headers), and the bottom row is the status bar. Everything in between is the file list. These are computed once from the display dimensions when the browser starts.

`s_copy_src` holds the full CHAR16 path to a copied file, and `s_copy_name` holds its ASCII filename. Both persist as long as the browser is running. You can copy a file in one directory, navigate elsewhere, and paste it there.

## Helpers

Before the main logic, we need a few small utility functions. These handle numeric formatting, string conversion, and line padding that the drawing functions rely on.

```c
static void uint_to_str(UINT64 n, char *buf) {
    char tmp[24];
    int t = 0;
    if (n == 0) { tmp[t++] = '0'; }
    else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; } }
    int i = 0;
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = '\0';
}
```

This is the same integer-to-string helper we used in `main.c`, but taking `UINT64` for file sizes. It divides by 10 repeatedly to extract digits in reverse order, then reverses them. File sizes on FAT32 can reach 4 GB, so 64-bit input is necessary. The `tmp` array of 24 characters is more than enough for the largest 64-bit decimal number (20 digits).

```c
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
```

Produces human-readable size strings: "37 B", "4 KB", "2 MB". Integer division means we round down -- a 1500-byte file shows as "1 KB", not "1.46 KB". Close enough for a file browser. The three ranges cover bytes, kilobytes, and megabytes, which spans every file we will encounter on our FAT32 volume.

```c
static void path_to_ascii(char *dst, int max) {
    int i = 0;
    while (s_path[i] && i < max - 1) {
        dst[i] = (char)(s_path[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}
```

Convert the CHAR16 path to ASCII for display. FAT32 filenames are effectively ASCII, so masking with `0x7F` is safe -- it strips the upper byte of each CHAR16 character. This avoids the complexity of full Unicode rendering. We need this because our framebuffer draws 8-bit ASCII characters, not 16-bit UEFI strings.

```c
static void pad_line(char *line, UINT32 cols) {
    UINTN len = str_len((CHAR8 *)line);
    while (len < cols) {
        line[len] = ' ';
        len++;
    }
    line[cols] = '\0';
}
```

Pad a string with spaces to fill the full screen width. This function appears throughout the drawing code. When we render a line with `fb_string`, the background color only extends to the end of the string content. Without padding, a 20-character line on an 80-column screen would leave 60 columns showing whatever was previously drawn there. By padding to the full width, the background color covers the entire row, giving us clean, consistent bars and list entries. The null terminator is placed exactly at `cols` to prevent overflows.

## Path Manipulation

The browser navigates a directory tree by maintaining a path string. Four operations cover everything we need: set to root, check if at root, append a directory name, and go up one level.

```c
static void path_set_root(void) {
    s_path[0] = L'\\';
    s_path[1] = 0;
}
```

Set path to root. In UEFI, root is `\` -- a single backslash character followed by a null terminator. This is the starting point when the browser opens and the destination when the user backs all the way out of nested directories.

```c
static int path_is_root(void) {
    return s_path[0] == L'\\' && s_path[1] == 0;
}
```

Check if we are at root. This controls two behaviors: whether ESC exits the browser (at root) or goes up one level (in subdirectories), and whether Backspace does anything (it does nothing at root).

```c
static void path_append(const char *name) {
    int i = 0;
    while (s_path[i]) i++;
    if (i > 1 || s_path[0] != L'\\') {
        s_path[i++] = L'\\';
    }
    int j = 0;
    while (name[j] && i < MAX_PATH - 1) {
        s_path[i++] = (CHAR16)name[j++];
    }
    s_path[i] = 0;
}
```

Append a directory name to the current path. The separator logic handles the root edge case: when the path is `\` (root), we do not add another backslash -- `\` + `EFI` becomes `\EFI`, not `\\EFI`. When the path is anything else (like `\EFI`), we add a separator first -- `\EFI` + `BOOT` becomes `\EFI\BOOT`.

The name comes in as ASCII (from our `fs_entry` struct), cast to `CHAR16` as we copy. For ASCII characters, this zero-extends each byte to 16 bits, which is correct for UEFI path strings. The `MAX_PATH - 1` guard prevents buffer overflow.

```c
static void path_up(void) {
    int i = 0;
    while (s_path[i]) i++;
    i--;
    while (i > 0 && s_path[i] != L'\\') i--;
    if (i == 0)
        path_set_root();
    else
        s_path[i] = 0;
}
```

Go up one directory. This scans backward from the end of the path to find the last backslash, then truncates there. If the last backslash is at position 0, we are one level above root, so we set the path to root instead of leaving it as an empty string.

The path itself serves as the navigation stack. There is no separate stack data structure tracking which directories we have visited. Going up just removes the last component from the string. This is simple and it is correct because we never need to go "forward" -- the user always navigates explicitly.

## Drawing the Screen

The screen is divided into five zones:

```
Row 0:          Header bar (cyan text on dark gray background)
Row 1:          Current path (yellow text on black)
Row 2:          Column headers (gray text on black)
Rows 3..N-2:   File listing with selection highlight
Row N-1:        Status bar with key hints (gray text on dark gray)
```

Each zone is drawn by a separate function. This separation lets us redraw individual parts without touching the rest -- critical for performance. Moving the cursor one position only needs to redraw two lines (the old and new cursor positions), not the entire screen.

### The Header

```c
static void draw_header(void) {
    char line[256];
    mem_set(line, ' ', g_boot.cols);
    line[g_boot.cols] = '\0';

    const char *title = " SURVIVAL FILE BROWSER";
    int i = 0;
    while (title[i] && i < (int)g_boot.cols) {
        line[i] = title[i];
        i++;
    }

    fb_string(0, 0, line, COLOR_CYAN, COLOR_DGRAY);
}
```

The pattern for every bar in the browser: create a line filled with spaces so the background color covers the full screen width, copy the text content into it, and render with `fb_string`. Without the space-filling, the dark gray background would only extend to the last character of the title, leaving the rest of row 0 black. The leading space in the title string provides a one-character left margin.

### The Path Bar

```c
static void draw_path(void) {
    char line[256];
    char path_ascii[256];
    int i = 0;

    line[i++] = ' ';
    path_to_ascii(path_ascii, 240);
    int j = 0;
    while (path_ascii[j] && i < (int)g_boot.cols) {
        line[i++] = path_ascii[j++];
    }
    line[i] = '\0';
    pad_line(line, g_boot.cols);

    fb_string(0, 1, line, COLOR_YELLOW, COLOR_BLACK);
}
```

The path bar shows the current directory in yellow on black. It converts the CHAR16 path to ASCII via `path_to_ascii`, then pads to the full width. The leading space provides the same left margin as the header. When the user navigates into `\EFI\BOOT`, the path bar shows ` \EFI\BOOT`. At root it shows ` \`.

### Column Headers

```c
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
```

A simple labeled header row. The "Name" and "Size" columns are positioned with literal spacing in the string constant. Gray text on black provides a visual separator between the path bar and the file listing without being too prominent. The exact column positions do not need to match the data rows precisely -- the headers serve as rough labels, not aligned column markers.

### Drawing a Single Entry

This is the most complex drawing function. It renders one row of the file listing with the correct content, alignment, and colors:

```c
static void draw_entry_line(int list_idx) {
    int entry_idx = s_scroll + list_idx;
    UINT32 row = s_list_top + (UINT32)list_idx;
    char line[256];

    mem_set(line, ' ', g_boot.cols);
    line[g_boot.cols] = '\0';
```

`list_idx` is the position on screen (0 = first visible row of the file list). `entry_idx` is the position in the entries array. The difference between them is `s_scroll`. For example, if the user has scrolled down 10 entries, `list_idx` 0 corresponds to `entry_idx` 10. The actual screen row is `s_list_top + list_idx` because the file list starts below the header, path bar, and column headers.

We start with a line filled with spaces so the background color covers the full width regardless of how much content we draw into it.

```c
    if (entry_idx >= s_count) {
        fb_string(0, row, line, COLOR_WHITE, COLOR_BLACK);
        return;
    }
```

Past the end of entries? Draw a blank line. This clears any stale content from a previous directory that had more entries. Without this, switching from a directory with 20 entries to one with 5 would leave the old entries 6-20 still visible.

```c
    struct fs_entry *e = &s_entries[entry_idx];
    int pos = 1;

    if (e->is_dir) {
        const char *dir_tag = "[DIR] ";
        int k = 0;
        while (dir_tag[k] && pos < (int)g_boot.cols)
            line[pos++] = dir_tag[k++];
    }

    int k = 0;
    int name_limit = (int)g_boot.cols - 15;
    while (e->name[k] && pos < name_limit)
        line[pos++] = e->name[k++];
```

Build the line content. Position starts at 1 (column 0 is the left margin space). Directories get a `[DIR]` prefix, making them immediately identifiable. File names are limited to `cols - 15` characters to leave room for the size display. Long filenames are silently truncated -- no ellipsis or scrolling, just a hard cutoff. This is acceptable because FAT32's 8.3 short names and typical long filenames rarely exceed 40 characters.

```c
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
```

For files (not directories), format the size and right-align it near the right edge. The position `g_boot.cols - slen - 2` places the size string two characters from the right edge. The `if (size_col > pos)` check prevents the size from overlapping a long filename -- if the name reaches all the way to the size column, we skip the size display rather than creating a jumbled line.

```c
    UINT32 fg, bg;
    if (entry_idx == s_cursor) {
        fg = COLOR_BLACK;
        bg = COLOR_CYAN;
    } else if (e->is_dir) {
        fg = COLOR_GREEN;
        bg = COLOR_BLACK;
    } else {
        fg = COLOR_WHITE;
        bg = COLOR_BLACK;
    }

    fb_string(0, row, line, fg, bg);
}
```

Three color schemes: the selected entry gets a cyan highlight bar with black text (inverted), directories are green on black, and regular files are white on black. The cyan highlight is full-width because we pre-filled the line buffer with spaces -- the user sees a solid cyan bar spanning the entire row, immediately showing which entry is selected.

### The File List

```c
static void draw_list(void) {
    for (UINT32 i = 0; i < s_list_rows; i++)
        draw_entry_line((int)i);
}
```

Draw every visible row of the file listing. This iterates over screen positions (0 to `s_list_rows - 1`), and `draw_entry_line` maps each to the correct entry index via `s_scroll`. Rows beyond the end of the directory get blank lines. This function is called when the scroll position changes -- any time the visible window shifts, we redraw all rows because we do not know which ones changed.

### Status Messages

The status bar at the bottom of the screen serves two purposes: it normally shows the available key bindings, and it temporarily shows feedback messages like "Copied: hello.c" after an action.

```c
static void draw_status_msg(const char *msg) {
    char line[256];
    mem_set(line, ' ', g_boot.cols);
    line[g_boot.cols] = '\0';

    if (!msg)
        msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename BS:Back ESC:Exit";

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
```

When `msg` is NULL, `draw_status_msg` falls through to the default key hints. When it is a string like `" Copied: hello.c"`, that message replaces the key hints on the status bar.

The trick to making temporary messages visible is in the main loop's structure. After an action function like `do_copy` calls `draw_status_msg` with a custom message, the message stays on screen for the entire duration the user is reading it -- from the moment we draw it until they press the next key. Then `draw_status()` is called to restore the normal key hints, and the loop processes whatever key was pressed. We get timed display for free without any actual timer, because the "timer" is the user's reading speed.

### Full Screen Drawing

```c
static void draw_all(void) {
    fb_clear(COLOR_BLACK);
    draw_header();
    draw_path();
    draw_col_headers();
    draw_list();
    draw_status();
}
```

`draw_all` redraws everything from scratch -- clear the screen, then draw each zone in order. This is used when changing directories (the entire display changes), after paste or rename operations (the directory contents changed), and when returning from the file viewer or editor. The `fb_clear` ensures no artifacts remain from the previous screen state.

## Loading and Scrolling

```c
static void load_dir(void) {
    s_count = fs_readdir(s_path, s_entries, MAX_ENTRIES);
    if (s_count < 0) s_count = 0;
    s_cursor = 0;
    s_scroll = 0;
}
```

Load a directory and reset the cursor to the top. `fs_readdir` fills the `s_entries` array and returns the count. If the read fails (invalid path, corrupted filesystem), we treat it as an empty directory rather than crashing. The cursor and scroll are both reset to zero because the previous position has no meaning in a new directory -- entry 5 in the old directory is not related to entry 5 in the new one.

Note that `fs_readdir` returns entries already sorted: directories first, then files, both groups sorted alphabetically. This means we do not need to sort here -- the display order comes directly from the filesystem module.

```c
static void clamp_scroll(void) {
    if (s_cursor < s_scroll)
        s_scroll = s_cursor;
    if (s_cursor >= s_scroll + (int)s_list_rows)
        s_scroll = s_cursor - (int)s_list_rows + 1;
    if (s_scroll < 0)
        s_scroll = 0;
}
```

Ensure the cursor is always visible within the displayed window. If the cursor has moved above the visible area (cursor < scroll), move the window up to show it. If the cursor has moved below the visible area (cursor >= scroll + visible rows), move the window down so the cursor is at the bottom of the visible area.

This function is called after every cursor movement. The third check (`s_scroll < 0`) catches the edge case where `s_cursor - s_list_rows + 1` underflows because the cursor is near the top of a short list.

## The File Viewer

When the user presses ENTER on a file, we open a simple read-only viewer. Later in this chapter, we will replace this with the editor, but the viewer teaches the line-splitting technique that the editor also uses.

```c
static void view_file(void) {
    CHAR16 full_path[MAX_PATH];
    int i = 0;
    while (s_path[i]) { full_path[i] = s_path[i]; i++; }
    if (i > 1 || full_path[0] != L'\\') full_path[i++] = L'\\';
    int j = 0;
    while (s_entries[s_cursor].name[j] && i < MAX_PATH - 1) {
        full_path[i++] = (CHAR16)s_entries[s_cursor].name[j++];
    }
    full_path[i] = 0;
```

Construct the full path by combining the current directory with the selected filename. This is the same separator logic as `path_append` -- at root, do not double the backslash; everywhere else, add a separator. We build the path in a local variable rather than modifying `s_path`, so the browser's current directory is preserved when we return.

```c
    UINTN file_size = 0;
    char *data = (char *)fs_readfile(full_path, &file_size);
    if (!data) {
        fb_string(0, g_boot.rows - 1, " Error: Could not read file",
                  COLOR_RED, COLOR_DGRAY);
        struct key_event ev;
        kbd_wait(&ev);
        draw_all();
        return;
    }
```

Load the entire file into memory. On failure, show a red error message on the status bar, wait for any keypress as an acknowledgment, then redraw the browser and return. The "wait for keypress" ensures the user actually sees the error -- without it, the error would flash and immediately be replaced by the browser display.

### Line Splitting

```c
    #define MAX_LINES 4096

    char **lines = (char **)mem_alloc(MAX_LINES * sizeof(char *));
    if (!lines) {
        mem_free(data);
        return;
    }

    int line_count = 0;
    lines[0] = data;
    line_count = 1;
    for (UINTN k = 0; k < file_size && line_count < MAX_LINES; k++) {
        if (data[k] == '\n') {
            data[k] = '\0';
            if (k + 1 < file_size)
                lines[line_count++] = &data[k + 1];
        } else if (data[k] == '\r') {
            data[k] = '\0';
            if (k + 1 < file_size && data[k + 1] == '\n')
                k++;
            if (k + 1 < file_size)
                lines[line_count++] = &data[k + 1];
        }
    }
```

Walk through the file data, replacing newline characters with null terminators. Record the start of each line in the `lines` array. This converts one big string into an array of individual C strings -- making it trivial to render any range of lines for scrolling.

We handle both Unix (`\n`) and Windows (`\r\n`) line endings. The `\r` handler first nulls the carriage return, then checks if a `\n` follows and skips it. This prevents blank lines in Windows-format files. A bare `\r` (old Mac format) also works correctly -- it becomes a line break.

The `MAX_LINES` limit of 4096 prevents runaway memory use on very large files. Beyond 4096 lines, additional content is simply not indexed. For a survival workstation, this is more than enough -- configuration files and documentation rarely exceed a few hundred lines.

### The Viewer Loop

```c
    int view_scroll = 0;
    int view_rows = (int)g_boot.rows - 2;
    char line_buf[256];

    for (;;) {
        fb_clear(COLOR_BLACK);

        /* Header: filename */
        mem_set(line_buf, ' ', g_boot.cols);
        line_buf[g_boot.cols] = '\0';
        const char *fn = s_entries[s_cursor].name;
        int fi = 1;
        while (fn[fi - 1] && fi < (int)g_boot.cols) {
            line_buf[fi] = fn[fi - 1];
            fi++;
        }
        fb_string(0, 0, line_buf, COLOR_CYAN, COLOR_DGRAY);

        /* File content */
        for (int r = 0; r < view_rows; r++) {
            int li = view_scroll + r;
            if (li >= line_count) break;

            int c = 0;
            const char *src = lines[li];
            while (src[c] && c < (int)g_boot.cols - 1) {
                char ch = src[c];
                if (ch >= 0x20 && ch <= 0x7E)
                    line_buf[c] = ch;
                else
                    line_buf[c] = '.';
                c++;
            }
            line_buf[c] = '\0';

            fb_string(0, (UINT32)(r + 1), line_buf, COLOR_WHITE, COLOR_BLACK);
        }

        /* Status bar */
        mem_set(line_buf, ' ', g_boot.cols);
        line_buf[g_boot.cols] = '\0';
        const char *hint = " UP/DOWN:Scroll PGUP/PGDN:Page HOME/END ESC:Close";
        int hi = 0;
        while (hint[hi] && hi < (int)g_boot.cols) {
            line_buf[hi] = hint[hi];
            hi++;
        }
        fb_string(0, g_boot.rows - 1, line_buf, COLOR_GRAY, COLOR_DGRAY);

        struct key_event ev;
        kbd_wait(&ev);

        if (ev.code == KEY_ESC || ev.code == KEY_BS)
            break;
        else if (ev.code == KEY_UP && view_scroll > 0)
            view_scroll--;
        else if (ev.code == KEY_DOWN && view_scroll < line_count - view_rows)
            view_scroll++;
        else if (ev.code == KEY_PGUP) {
            view_scroll -= view_rows;
            if (view_scroll < 0) view_scroll = 0;
        } else if (ev.code == KEY_PGDN) {
            view_scroll += view_rows;
            if (view_scroll > line_count - view_rows)
                view_scroll = line_count - view_rows;
            if (view_scroll < 0) view_scroll = 0;
        } else if (ev.code == KEY_HOME)
            view_scroll = 0;
        else if (ev.code == KEY_END) {
            view_scroll = line_count - view_rows;
            if (view_scroll < 0) view_scroll = 0;
        }
    }

    mem_free(lines);
    mem_free(data);
}
```

The viewer is simpler than the browser -- no cursor, just scrolling. Each iteration clears the screen, draws the filename header, renders the visible range of lines, draws a status bar with key hints, waits for a key, and adjusts `view_scroll` accordingly.

Non-printable characters (outside ASCII 0x20 to 0x7E) are replaced with `.` on display. This handles binary files gracefully -- they show as dots instead of producing garbled output or crashing the font renderer. Tabs are also replaced with dots, which is imperfect but safe.

The `view_rows` calculation reserves row 0 for the header and the bottom row for the status bar. Everything else is file content. The `view_scroll` clamping in Page Down and End ensures we never scroll past the end of the file -- `line_count - view_rows` positions the last line at the bottom of the screen.

ESC or Backspace exits back to the browser. We free the line pointer array and the file data buffer on exit. The browser's `draw_all` will be called after we return to restore the normal display.

## Copy and Paste

File management needs more than just viewing. We need to be able to duplicate files -- copy a configuration, back up a source file before editing, or move survival documents between directories. The browser gets copy and paste support through a deferred copy model: F3 records what to copy, F8 performs the actual I/O.

### Copying a File

```c
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
```

Copy does no I/O at all. It just records *what* to copy -- the full source path in `s_copy_src` and the filename in `s_copy_name`. The actual file reading happens at paste time. This means copy is instant, and it also means the source file does not need to fit in memory twice (once for a copy buffer, once for the paste write).

We skip directories. Recursive directory copy would require walking the entire subtree, and that is significant complexity for a survival workstation. Copying individual files covers the common case.

The confirmation message `" Copied: hello.c"` appears on the status bar. The leading space matches the indentation of the normal key hints. This message stays visible until the user presses the next key, at which point the main loop's `draw_status()` replaces it with the default bar.

### Unique Name Generation

When pasting, we cannot blindly write a file with the same name as the original. If the user copies `hello.c` and pastes into the same directory, writing to `hello.c` would silently overwrite the original -- exactly the opposite of what paste should do.

We need two helpers. First, case-insensitive name comparison. FAT32 is case-insensitive: `HELLO.C` and `hello.c` are the same file. Our comparisons need to reflect that:

```c
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
```

Simple ASCII case folding. Adding 32 converts uppercase to lowercase (the distance between 'A' and 'a' in ASCII). We do not need locale-aware Unicode comparison -- FAT32 filenames are effectively ASCII.

Second, a function to check if a name already exists in the current directory listing:

```c
static int name_exists(const char *name) {
    for (int i = 0; i < s_count; i++)
        if (names_equal(name, s_entries[i].name))
            return 1;
    return 0;
}
```

This scans the already-loaded `s_entries` array. No additional filesystem calls are needed -- `load_dir` already fetched the full listing. The case-insensitive comparison ensures we detect conflicts regardless of case differences.

Now we can generate unique paste names:

```c
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
```

The algorithm: try the original name first. If it is not taken, use it -- this handles pasting into a different directory than the source, where no conflict exists. If the name exists, split it at the last dot to separate the base name from the extension. Then try `base_2.ext`, `base_3.ext`, and so on up to `base_99.ext`.

For example, pasting `hello.c` into a directory that already has `hello.c`:
- `hello.c` -- exists, skip
- `hello_2.c` -- free, use it

Pasting again:
- `hello.c` -- exists
- `hello_2.c` -- exists
- `hello_3.c` -- free, use it

Files without extensions (like `Makefile`) get `Makefile_2`, `Makefile_3`, etc. The `max - 12` limit in the copy loop leaves room for `_99` plus a reasonable extension. The scan backward for the last dot handles filenames with multiple dots correctly -- `archive.tar.gz` splits into base `archive.tar` and extension `.gz`, producing `archive.tar_2.gz`.

### Pasting a File

```c
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
```

`do_paste` is the workhorse of file management. It performs five steps: read the source file into memory, check if there is enough disk space, generate a unique name in the current directory, write the file, and show a confirmation.

The return value matters: 0 on success, -1 on failure. The main loop uses this to decide whether to reload the directory listing. On failure, nothing changed on disk, so there is no need to reload and redraw.

Each failure path shows a specific message so the user knows *why* the paste failed. "Nothing to paste" means they have not copied anything yet. "Cannot read source" means the source file was deleted or the volume was removed since it was copied. "Not enough disk space" is self-explanatory. "Write error" catches everything else -- firmware bugs, file system corruption, write-protected media.

The disk space check using `fs_volume_info` is a courtesy. Without it, `fs_writefile` would still fail, but with a generic UEFI status code. Checking first lets us give a clear, human-readable "not enough disk space" message instead of a cryptic write error. The `fs_volume_info` function queries the UEFI filesystem's `GetInfo` with the `EFI_FILE_SYSTEM_INFO_ID` GUID:

```c
int fs_volume_info(UINT64 *total_bytes, UINT64 *free_bytes) {
    if (!s_root) return -1;

    EFI_GUID fsi_guid = EFI_FILE_SYSTEM_INFO_ID;
    UINTN buf_size = 0;

    /* First call to get required size */
    s_root->GetInfo(s_root, &fsi_guid, &buf_size, NULL);
    if (buf_size == 0) return -1;

    EFI_FILE_SYSTEM_INFO *info = (EFI_FILE_SYSTEM_INFO *)mem_alloc(buf_size);
    if (!info) return -1;

    EFI_STATUS status = s_root->GetInfo(s_root, &fsi_guid, &buf_size, info);
    if (EFI_ERROR(status)) {
        mem_free(info);
        return -1;
    }

    *total_bytes = info->VolumeSize;
    *free_bytes = info->FreeSpace;
    mem_free(info);
    return 0;
}
```

This follows the standard UEFI two-call pattern: first call with a NULL buffer to learn the required size, then allocate and call again. The `EFI_FILE_SYSTEM_INFO` structure contains the total volume size and free space, both in bytes.

## Renaming Files

### The Filesystem Layer

Before we can add rename support to the browser, we need the filesystem to support it. UEFI renames files through an indirect mechanism: you open the file, modify the filename field in its `EFI_FILE_INFO` structure, and call `SetInfo` to apply the change. There is no dedicated rename call.

Add this to `fs.c`:

```c
EFI_STATUS fs_rename(const CHAR16 *path, const CHAR16 *new_name) {
    if (!s_root) return EFI_NOT_READY;

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = s_root->Open(s_root, &file, (CHAR16 *)path,
                                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status)) return status;

    /* Get current file info */
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    file->GetInfo(file, &info_guid, &info_size, NULL);

    /* Allocate extra space for the new name */
    UINTN alloc_size = info_size + 256 * sizeof(CHAR16);
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)mem_alloc(alloc_size);
    if (!info) { file->Close(file); return EFI_OUT_OF_RESOURCES; }

    status = file->GetInfo(file, &info_guid, &info_size, info);
    if (EFI_ERROR(status)) {
        mem_free(info);
        file->Close(file);
        return status;
    }

    /* Set new filename */
    int i = 0;
    while (new_name[i] && i < 255) {
        info->FileName[i] = new_name[i];
        i++;
    }
    info->FileName[i] = 0;
    info->Size = SIZE_OF_EFI_FILE_INFO + (UINT64)(i + 1) * sizeof(CHAR16);

    UINTN set_size = (UINTN)info->Size;
    status = file->SetInfo(file, &info_guid, set_size, info);

    mem_free(info);
    file->Close(file);
    return status;
}
```

The tricky part is that `EFI_FILE_INFO` is a variable-size structure. The fixed fields (size, timestamps, attributes) occupy a known number of bytes -- that is `SIZE_OF_EFI_FILE_INFO`. But the `FileName` array extends past the end of those fixed fields, and its length varies per file. So `GetInfo` returns different amounts of data depending on how long the current filename is.

When we rename, the new filename might be longer than the old one. If we only allocated `info_size` bytes (what `GetInfo` reported), we could overflow the buffer when copying a longer name in. That is why we allocate `info_size + 256 * sizeof(CHAR16)` -- room for the old info plus a generous margin for a longer new name.

After copying the new name into `info->FileName`, we update `info->Size` to reflect the new total structure size. This is critical: `SetInfo` reads this field to know how many bytes of the structure are valid. If `Size` still reflects the old shorter name, `SetInfo` might not read far enough to see the full new filename.

The formula `SIZE_OF_EFI_FILE_INFO + (i + 1) * sizeof(CHAR16)` adds the fixed-fields size to the new filename length (including the null terminator, hence `i + 1`).

And the corresponding declaration in `fs.h`:

```c
/* Rename a file. new_name is just the filename, not a full path. */
EFI_STATUS fs_rename(const CHAR16 *path, const CHAR16 *new_name);
```

Note that `path` is the full path to the existing file, while `new_name` is just the bare filename -- not a path. UEFI's `SetInfo` changes the name of the file within its current directory; it does not move files between directories.

### The Browser Rename Function

Rename uses an inline text input on the status bar, pre-filled with the current filename:

```c
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
```

Several design decisions are worth explaining.

**Pre-filling.** The name buffer starts with the current filename already in it, and `len` is set to its length. Backspace deletes from the end. The user sees `Rename: hello.c_` and can backspace to `Rename: hello_` and type `.txt` to get `hello.txt`. This is much faster than typing a whole new name from scratch.

**The underscore cursor.** The `_` character at position `i` serves as a visual cursor indicator. It sits right after the last character of the current name, showing the user where the next typed character will appear. When the user types, the cursor advances. When they backspace, it retreats.

**Early exit on no change.** If the user just presses Enter without editing, `names_equal` catches that the name has not changed, and we skip the filesystem call entirely. No point calling `SetInfo` to rename a file to its current name. The case-insensitive comparison means changing only the case (e.g., `Hello.c` to `hello.c`) is also detected as "unchanged" -- which is correct for FAT32 where case does not matter.

**CHAR16 conversion.** The user types ASCII characters, but `fs_rename` expects a CHAR16 string. We do the conversion inline, casting each `char` to `CHAR16`. For ASCII-range characters (which is all FAT32 really supports), this is a simple zero-extension.

**Error feedback.** If `fs_rename` fails (file locked, firmware quirk, read-only volume), we show "Rename failed" on the status bar. The user can try again or give up.

Unlike copy, rename works on both files and directories. There is no `if (e->is_dir) return` guard -- `fs_rename` calls `SetInfo`, which works on any file handle regardless of whether it refers to a file or directory.

## The Main Browser Loop

Now we assemble everything into the main loop. The browser initializes the layout, opens the filesystem, starts at root, and enters an event loop that processes keyboard input:

```c
void browse_run(void) {
    /* Init layout */
    s_list_top = 3;
    s_list_rows = g_boot.rows - 4;

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
```

The layout computation: three rows of chrome at the top (header, path, column headers) and one row at the bottom (status bar). Everything between is file list. On a 30-row display, that gives 26 rows of file listing -- enough to show a typical directory without scrolling.

If `fs_init` fails (no SimpleFileSystem protocol found, no boot volume), we show an error and return immediately. There is nothing a file browser can do without a filesystem.

```c
    struct key_event ev;
    for (;;) {
        kbd_wait(&ev);

        /* Restore normal status bar (clears previous temp messages) */
        draw_status();

        int old_cursor = s_cursor;

        switch (ev.code) {
```

The main loop waits for a key, immediately restores the normal status bar (clearing any temporary message from the previous action), saves the current cursor position for efficient redraws, and dispatches based on the key code.

The `draw_status()` call right after `kbd_wait` is the key to the temporary message system. When `do_copy` shows "Copied: hello.c", that message stays visible while the user reads it. Then the next keypress arrives, `kbd_wait` returns, `draw_status()` immediately replaces the message with normal key hints, and the loop processes whatever key was pressed.

### Navigation Keys

```c
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
```

**Efficient cursor movement.** When scrolling did not change (the cursor moved within the visible window), we redraw exactly two lines -- the old position (remove the cyan highlight) and the new position (add the cyan highlight). When scrolling did change (cursor crossed the edge of the visible window), we redraw the entire list because every visible entry shifted.

This two-line redraw makes a noticeable difference on slow serial consoles and low-powered hardware. Redrawing 2 lines instead of 26 is 13x less work.

```c
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
```

Page Up and Page Down move the cursor by a full screenful. Home jumps to the first entry, End to the last. All four always redraw the full list because they move the cursor by large amounts, which almost always changes the scroll position.

The `s_cursor < 0` guard after Page Down handles the edge case of an empty directory where `s_count` is 0, making `s_count - 1` equal to -1.

### Enter, Backspace, and Escape

```c
        case KEY_ENTER:
            if (s_count > 0 && s_entries[s_cursor].is_dir) {
                path_append(s_entries[s_cursor].name);
                load_dir();
                draw_all();
            } else if (s_count > 0) {
                view_file();
                draw_all();
            }
            break;

        case KEY_BS:
            if (!path_is_root()) {
                path_up();
                load_dir();
                draw_all();
            }
            break;

        case KEY_ESC:
            if (!path_is_root()) {
                path_up();
                load_dir();
                draw_all();
            } else {
                return;
            }
            break;
```

ENTER on a directory appends the name to the path and reloads. ENTER on a file opens the viewer. After either action, we call `draw_all` to fully redraw the screen.

Backspace always goes up one level (if not at root). At root, it does nothing. ESC goes up in subdirectories, but exits the browser entirely at root -- returning control to `main.c`. This gives the user two escape velocities: Backspace navigates within the browser, ESC is the ultimate exit.

### File Management Keys

```c
        case KEY_F3:
            do_copy();
            break;

        case KEY_F4:
            prompt_new_file();
            load_dir();
            draw_all();
            break;

        case KEY_F8:
            if (do_paste() == 0) {
                load_dir();
                draw_all();
            }
            break;

        case KEY_F9:
            do_rename();
            load_dir();
            draw_all();
            break;
        }
    }
}
```

The F3 handler is the simplest -- `do_copy` just records state in `s_copy_src` and `s_copy_name`. No directory reload is needed because nothing on disk changed.

The F4 handler calls `prompt_new_file` (shown in the next section), which prompts for a filename and opens the editor on a new, empty file. After the editor returns, we reload the directory because the file may have been created and saved.

The F8 handler conditionally reloads. If `do_paste` returns 0 (success), a new file exists on disk, so we reload the directory and redraw. If it returns -1 (failure), nothing changed, and the error message is already visible on the status bar.

The F9 handler always reloads after the rename prompt returns. Even if the user pressed ESC to cancel, reloading is cheap and ensures the display is correct.

## Integrating the Editor

The file viewer shown earlier is a temporary solution. Once we have the text editor from Chapter 11, we replace the viewer with the editor. This requires three changes: a function to open files in the editor, a function to create new files, and an updated ENTER handler.

```c
static void open_file(void) {
    if (s_count <= 0 || s_entries[s_cursor].is_dir)
        return;
    edit_run(s_path, s_entries[s_cursor].name);
}
```

`open_file` replaces `view_file`. It passes the current directory path and the filename to the editor module. The editor takes over the screen, lets the user view and edit the file, and returns when they press ESC. The browser's path and cursor state are preserved because `edit_run` does not modify them.

```c
static void prompt_new_file(void) {
    char name[128];
    int len = 0;
    name[0] = '\0';

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
            if (len > 0)
                break;
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
```

`prompt_new_file` uses the same inline text input pattern as `do_rename`, but starting with an empty name instead of a pre-filled one. The user types a filename on the status bar and presses Enter. ESC cancels. The name is then passed to `edit_run`, which opens the editor on a file that does not yet exist -- the user creates it by saving from the editor.

The ENTER handler in the main loop changes accordingly:

```c
        case KEY_ENTER:
            if (s_count > 0 && s_entries[s_cursor].is_dir) {
                path_append(s_entries[s_cursor].name);
                load_dir();
                draw_all();
            } else if (s_count > 0) {
                open_file();
                load_dir();
                draw_all();
            }
            break;
```

The only difference from the earlier version: `view_file()` is now `open_file()`, and we call `load_dir()` after returning from the editor because the file may have been modified or a new file may have been created.

## What We Built

The file browser is now a complete file management tool running on bare-metal UEFI:

- Full-screen display with header, path bar, column headers, file listing, and status bar
- Keyboard navigation with UP/DOWN, Page Up/Down, Home/End
- Directory navigation with Enter (into) and Backspace/Escape (out)
- Sorted directory listings (directories first, then alphabetical)
- File size display with human-readable formatting (B, KB, MB)
- Efficient partial redraws -- two lines for cursor movement, full list for scrolling
- Text file viewer with line splitting and scrolling (later replaced by the editor)
- File copy with deferred I/O (F3 records, F8 executes)
- Unique name generation for paste conflicts (hello_2.c, hello_3.c)
- Disk space checking before paste operations
- File and directory rename with pre-filled inline editing (F9)
- UEFI SetInfo-based rename in the filesystem layer
- New file creation via F4 prompt feeding into the editor
- Temporary status bar messages with keypress-driven timeout
- Case-insensitive name comparison matching FAT32 semantics

The codebase grew by about 900 lines -- `browse.c` at roughly 700 lines plus `fs_rename` and `fs_volume_info` in `fs.c`. The path manipulation, drawing, and file management functions are all self-contained in `browse.c` with no global state leaking to other modules. Every function is small enough to understand in isolation, and the main loop is a straightforward switch statement mapping keys to actions.

In the next chapter, we build the text editor -- the companion to this browser. Together, they form the core user interface of the survival workstation: browse to find files, press Enter to edit them, save and return. Everything after that -- the compiler, the self-hosting, the USB tools -- builds on this foundation.

---

**Next:** [Chapter 11: Writing to Disk](chapter-11-writing-to-disk)
