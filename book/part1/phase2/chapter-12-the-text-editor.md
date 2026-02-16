---
layout: default
title: "Chapter 12: The Text Editor"
parent: "Phase 2: Filesystem & Editor"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 4
---

# Chapter 12: The Text Editor

## From Viewer to Editor

The file viewer from Chapter 10 displays text but cannot change it. It reads a file into a buffer, replaces newline characters with null terminators, and renders the resulting array of C strings to the screen. This is fast and memory-efficient for reading, but useless for editing. You cannot insert a character into the middle of a flat buffer without shifting everything after it. You cannot add a new line without reallocating the entire buffer. And worst of all, the viewer destroys the original data -- those null terminators overwrite the newlines, so you cannot even write the file back.

We need a different data structure. The classic approach for a text editor is an array of lines, where each line is an independently allocated growable buffer. This gives O(1) random access to any line by index, makes insertion and deletion of entire lines straightforward (shift the pointer array), and confines character-level operations to a single line's buffer. The tradeoff is more allocations -- one per line instead of one for the whole file -- but for text files with short lines, this is negligible.

The editor we build in this chapter handles everything you need for practical text editing: cursor movement in all directions, character insertion and deletion, line splitting and joining, text selection with a mark-based model, clipboard operations (copy, cut, paste), file saving with disk space checking, and an unsaved-changes guard on exit. It runs in about 700 lines of C.

## The Header

Create `src/edit.h`:

```c
#ifndef EDIT_H
#define EDIT_H

#include "boot.h"

/* Launch the text editor on a file.
   path: CHAR16 directory path (e.g. L"\\notes")
   filename: ASCII filename (e.g. "todo.txt")
   If file doesn't exist, starts with empty document and creates on save. */
void edit_run(const CHAR16 *path, const char *filename);

#endif /* EDIT_H */
```

Same pattern as `browse.h` -- one public function that takes over the screen and returns when done. The caller provides the directory path as a CHAR16 string (for UEFI filesystem calls) and the filename as ASCII (for display in the header bar). The editor manages all its own state internally through static variables.

## The Line Buffer

Now `src/edit.c`. Start with the includes and constants:

```c
#include "edit.h"
#include "fb.h"
#include "kbd.h"
#include "fs.h"
#include "mem.h"

#define EDIT_MAX_LINES  4096
#define EDIT_MAX_PATH   512
#define EDIT_INIT_CAP   80
```

`EDIT_MAX_LINES` caps the document at 4096 lines. This is a static array limit -- we allocate the line pointer array at compile time rather than dynamically growing it. For the kind of files you edit on a survival workstation (notes, configuration, short programs), 4096 lines is generous. `EDIT_MAX_PATH` limits the full file path length. `EDIT_INIT_CAP` is the starting capacity of each line buffer -- 80 characters, matching a standard terminal width. Most lines are shorter than this, so the initial allocation is usually sufficient and no reallocation is needed.

The core data structure is the line buffer:

```c
struct edit_line {
    char  *data;
    UINTN  len;
    UINTN  cap;
};
```

Each line is an independently allocated character buffer with three fields. `data` points to the heap-allocated character array. `len` is the current number of characters in the line (not counting the null terminator). `cap` is the total allocated capacity. When `len` approaches `cap`, we allocate a larger buffer and copy. This is the classic dynamic array pattern -- start small, double when full.

Why not one big buffer for the whole file? Because inserting a character at line 5 of a 1000-line file would require shifting hundreds of kilobytes of data. With independent line buffers, insertion only affects a single line. The cost is more small allocations, but each line is typically under 200 bytes, and our UEFI allocator handles small allocations efficiently.

## Editor State

The editor's state is held in static variables, grouped by function:

```c
static struct edit_line s_lines[EDIT_MAX_LINES];
static int    s_line_count;
static int    s_cx, s_cy;
static int    s_scroll_x, s_scroll_y;
static int    s_modified;
static CHAR16 s_filepath[EDIT_MAX_PATH];
static char   s_filename[128];
```

`s_lines` is the document -- an array of line buffers. The array itself is statically allocated (each element is just a pointer, a length, and a capacity -- 24 bytes), but the character data each line points to is heap-allocated. `s_line_count` tracks how many lines are in use.

`s_cx` and `s_cy` are the cursor position in document coordinates -- column and row. These are independent of the screen; the cursor can be at document line 500 even if the screen only shows 25 rows at a time. `s_scroll_x` and `s_scroll_y` track the viewport offset -- which portion of the document is currently visible. `s_modified` is the dirty flag, set to 1 whenever the document changes and cleared to 0 on save. `s_filepath` holds the full UEFI path for saving, and `s_filename` holds the display name for the header bar.

### Selection State

```c
static int    s_sel_active;
static int    s_sel_anchor_y, s_sel_anchor_x;
```

The selection uses a mark-based model. `s_sel_active` is 0 or 1. When the user presses F3, the editor records the current cursor position as the anchor (`s_sel_anchor_y`, `s_sel_anchor_x`) and sets `s_sel_active` to 1. As the cursor moves, everything between the anchor and the current cursor position is highlighted. The anchor is where the user pressed F3; the cursor is where they moved to. Press F3 again or Escape to deactivate.

Why F3 instead of Shift+arrow keys? Because basic UEFI keyboard input cannot reliably detect Shift on arrow keys. The `EFI_SIMPLE_TEXT_INPUT_PROTOCOL` reports scancodes and Unicode characters, but modifier keys on arrow presses are not consistently available across firmware implementations. Some boards report them, some do not. F3 always works. This is the same approach used by mc (Midnight Commander)'s internal editor and the Borland Turbo editors of the 1980s.

### Clipboard

```c
static char  *s_clipboard;
static UINTN  s_clip_len;
#define CLIP_MAX 65536
```

The clipboard is a flat character buffer, allocated lazily on first use. `s_clip_len` tracks how many bytes are in it. `CLIP_MAX` at 64KB is generous for a text editor on a survival workstation -- that is enough to hold a substantial code file. The clipboard persists across copy/paste operations within a single editing session but is not saved to disk.

### Screen Geometry

```c
static int s_text_top;
static int s_text_rows;
static int s_text_cols;
```

These are computed once when the editor starts, based on the framebuffer dimensions in `g_boot`. `s_text_top` is the first screen row used for text (always 1, since row 0 is the header). `s_text_rows` is the number of rows available for text content. `s_text_cols` is the screen width in characters.

## Line Buffer Management

Five functions manage individual line buffers. Each is shown in full.

### line_init

```c
static void line_init(struct edit_line *ln) {
    ln->cap = EDIT_INIT_CAP;
    ln->data = (char *)mem_alloc(ln->cap);
    ln->data[0] = '\0';
    ln->len = 0;
}
```

Allocate a fresh buffer with the default initial capacity of 80 characters. Set the length to 0 and null-terminate. This is called whenever a new line is created -- by `doc_insert_line`, by `doc_load` for empty files, or during initialization.

### line_free

```c
static void line_free(struct edit_line *ln) {
    if (ln->data) {
        mem_free(ln->data);
        ln->data = NULL;
    }
    ln->len = 0;
    ln->cap = 0;
}
```

Free the line's character buffer and zero the struct. The NULL check guards against double-frees. After freeing, the struct is in a clean state -- all fields zeroed, data pointer NULL.

### line_ensure

```c
static void line_ensure(struct edit_line *ln, UINTN need) {
    if (need + 1 <= ln->cap)
        return;
    UINTN new_cap = ln->cap * 2;
    if (new_cap < need + 1) new_cap = need + 1;
    char *new_data = (char *)mem_alloc(new_cap);
    mem_copy(new_data, ln->data, ln->len + 1);
    mem_free(ln->data);
    ln->data = new_data;
    ln->cap = new_cap;
}
```

Ensure the buffer can hold at least `need` characters plus a null terminator. If the current capacity is sufficient, return immediately. Otherwise, double the capacity (or use `need + 1` if doubling is not enough), allocate a new buffer, copy the existing data including the null terminator, and free the old buffer.

We do not have `realloc` -- UEFI's `AllocatePool` does not support it. So we always allocate-copy-free. The doubling strategy means a line that grows character by character performs O(n) total copies instead of O(n^2). The `+ 1` throughout accounts for the null terminator that we always maintain at the end of the data.

### line_insert_char

```c
static void line_insert_char(struct edit_line *ln, int pos, char c) {
    line_ensure(ln, ln->len + 1);
    for (int i = (int)ln->len; i > pos; i--)
        ln->data[i] = ln->data[i - 1];
    ln->data[pos] = c;
    ln->len++;
    ln->data[ln->len] = '\0';
}
```

Insert a character at position `pos` by first ensuring there is room for one more character, then shifting all characters from `pos` to the end one position to the right, placing the new character in the gap, incrementing the length, and re-null-terminating. The shift loop runs backward from the end to avoid overwriting characters before they are moved. This is the `memmove` pattern implemented as a manual loop -- we avoid calling `memmove` because our shim is not yet linked at this stage of the build.

The operation is O(n) in line length, which is fine -- lines are rarely longer than a few hundred characters.

### line_delete_char

```c
static void line_delete_char(struct edit_line *ln, int pos) {
    if (pos < 0 || pos >= (int)ln->len)
        return;
    for (int i = pos; i < (int)ln->len - 1; i++)
        ln->data[i] = ln->data[i + 1];
    ln->len--;
    ln->data[ln->len] = '\0';
}
```

Delete the character at position `pos` by shifting all characters after it one position to the left, decrementing the length, and re-null-terminating. The bounds check at the top silently ignores invalid positions. Like `line_insert_char`, this is O(n) in line length.

## Document Operations

Four functions manage the document's line structure -- inserting, deleting, splitting, and joining lines.

### doc_insert_line

```c
static void doc_insert_line(int idx) {
    if (s_line_count >= EDIT_MAX_LINES)
        return;
    for (int i = s_line_count; i > idx; i--)
        s_lines[i] = s_lines[i - 1];
    s_line_count++;
    mem_set(&s_lines[idx], 0, sizeof(struct edit_line));
    line_init(&s_lines[idx]);
}
```

Insert a blank line at position `idx`. First check the limit. Then shift all lines from `idx` to the end down by one slot. Since `s_lines` is an array of structs (each containing a pointer, length, and capacity -- 24 bytes), we are moving struct values, not copying character data. The shift is O(n) in the number of lines, but for documents under 4096 lines, this completes in microseconds. After shifting, zero the slot at `idx` and initialize it with a fresh empty buffer.

### doc_delete_line

```c
static void doc_delete_line(int idx) {
    if (idx < 0 || idx >= s_line_count)
        return;
    line_free(&s_lines[idx]);
    for (int i = idx; i < s_line_count - 1; i++)
        s_lines[i] = s_lines[i + 1];
    s_line_count--;
    mem_set(&s_lines[s_line_count], 0, sizeof(struct edit_line));
}
```

Free the line's character buffer, shift all lines above it down by one slot, decrement the count, and zero the now-unused slot at the end. The bounds check guards against invalid indices.

### doc_split_line

```c
static void doc_split_line(void) {
    struct edit_line *cur = &s_lines[s_cy];
    int right_len = (int)cur->len - s_cx;

    doc_insert_line(s_cy + 1);
    struct edit_line *next = &s_lines[s_cy + 1];

    if (right_len > 0) {
        line_ensure(next, (UINTN)right_len);
        mem_copy(next->data, &cur->data[s_cx], (UINTN)right_len);
        next->len = (UINTN)right_len;
        next->data[next->len] = '\0';
        cur->len = (UINTN)s_cx;
        cur->data[cur->len] = '\0';
    }

    s_cy++;
    s_cx = 0;
    s_modified = 1;
}
```

This is what the Enter key does. The current line is split at the cursor position into two lines. First, calculate how many characters are to the right of the cursor (`right_len`). Insert a new blank line below the current one. If there are characters to the right, copy them into the new line, then truncate the current line at the cursor position.

For example, if the cursor is at "Hello|World" (pipe represents the cursor), we create a new line containing "World" and truncate the current line to "Hello". The cursor moves to column 0 of the new line. If the cursor is at the end of the line, the new line is empty -- we just created a blank line below.

The sequence is careful: we call `doc_insert_line` before accessing `next`, because `doc_insert_line` shifts the array and the pointer to `cur` remains valid (it is at index `s_cy`, which did not move). The `next` pointer is obtained after the insert.

### doc_join_lines

```c
static void doc_join_lines(int line_idx) {
    if (line_idx < 0 || line_idx + 1 >= s_line_count)
        return;
    struct edit_line *top = &s_lines[line_idx];
    struct edit_line *bot = &s_lines[line_idx + 1];

    if (bot->len > 0) {
        line_ensure(top, top->len + bot->len);
        mem_copy(&top->data[top->len], bot->data, bot->len);
        top->len += bot->len;
        top->data[top->len] = '\0';
    }

    doc_delete_line(line_idx + 1);
    s_modified = 1;
}
```

The reverse of `doc_split_line`. Append the bottom line's content to the end of the top line, then delete the bottom line. This is used by Backspace at column 0 (join current line onto the one above) and Delete at end of line (join the line below onto the current one).

The function grows the top line's buffer if needed via `line_ensure`, copies the bottom line's characters after the top line's existing content, updates the length, null-terminates, then deletes the bottom line (which frees its buffer and shifts the array).

## Loading a File

```c
static void doc_load(void) {
    UINTN file_size = 0;
    char *data = (char *)fs_readfile(s_filepath, &file_size);

    s_line_count = 0;
    mem_set(s_lines, 0, sizeof(s_lines));

    if (!data || file_size == 0) {
        line_init(&s_lines[0]);
        s_line_count = 1;
        if (data) mem_free(data);
        return;
    }
```

For new or empty files, start with one blank line. The editor always has at least one line -- this means the cursor always has somewhere to be and the rendering code never has to handle an empty document.

```c
    UINTN start = 0;
    for (UINTN i = 0; i <= file_size && s_line_count < EDIT_MAX_LINES; i++) {
        int is_end = (i == file_size);
        int is_nl = (!is_end && (data[i] == '\n' || data[i] == '\r'));

        if (is_end || is_nl) {
            UINTN seg_len = i - start;
            struct edit_line *ln = &s_lines[s_line_count];
            ln->cap = (seg_len < EDIT_INIT_CAP) ? EDIT_INIT_CAP : seg_len + 1;
            ln->data = (char *)mem_alloc(ln->cap);
            if (seg_len > 0)
                mem_copy(ln->data, &data[start], seg_len);
            ln->len = seg_len;
            ln->data[ln->len] = '\0';
            s_line_count++;

            if (!is_end && data[i] == '\r' && i + 1 < file_size && data[i + 1] == '\n')
                i++;
            start = i + 1;
        }
    }

    if (s_line_count == 0) {
        line_init(&s_lines[0]);
        s_line_count = 1;
    }

    mem_free(data);
}
```

Unlike the viewer's destructive parse (which replaces newlines with null terminators in-place), we copy each line segment into its own independently allocated buffer. The original file data is freed after loading. Each line gets an initial capacity of either 80 characters or the actual line length plus one, whichever is larger. This means short lines have room to grow without an immediate reallocation, while long lines are not wasted with excess capacity.

The CRLF handling is straightforward: when we encounter a `\r` followed by `\n`, we skip the `\n` so both characters are consumed but neither appears in the line data. The loop condition `i <= file_size` ensures that the last line is captured even if the file does not end with a newline.

### doc_clear

```c
static void doc_clear(void) {
    for (int i = 0; i < s_line_count; i++)
        line_free(&s_lines[i]);
    s_line_count = 0;
}
```

Free all line buffers and reset the line count. Called when exiting the editor to release memory.

## Serialization and Saving

### doc_serialize

```c
static char *doc_serialize(UINTN *out_size) {
    UINTN total = 0;
    for (int i = 0; i < s_line_count; i++) {
        total += s_lines[i].len;
        if (i < s_line_count - 1)
            total++;
    }

    char *buf = (char *)mem_alloc(total + 1);
    if (!buf) {
        *out_size = 0;
        return NULL;
    }

    UINTN pos = 0;
    for (int i = 0; i < s_line_count; i++) {
        if (s_lines[i].len > 0) {
            mem_copy(&buf[pos], s_lines[i].data, s_lines[i].len);
            pos += s_lines[i].len;
        }
        if (i < s_line_count - 1)
            buf[pos++] = '\n';
    }
    buf[pos] = '\0';

    *out_size = total;
    return buf;
}
```

Convert the document from an array of lines back into a flat byte buffer for writing to disk. The function uses a two-pass approach: the first pass calculates the total size (all line lengths plus one newline between each pair of adjacent lines), then a single allocation is made, and the second pass fills the buffer.

Why two passes instead of growing a buffer incrementally? Because we do not have `realloc` in the standard sense, and even if we did, repeated reallocation would fragment memory. A single allocation is simpler and more predictable. We use Unix-style `\n` line endings. No trailing newline after the last line -- this matches how we loaded the file.

### doc_save

```c
static int doc_save(void) {
    UINTN size = 0;
    char *buf = doc_serialize(&size);
    if (!buf)
        return -1;

    UINT64 total_bytes, free_bytes;
    if (fs_volume_info(&total_bytes, &free_bytes) == 0) {
        UINT64 old_size = fs_file_size(s_filepath);
        if ((UINT64)size > free_bytes + old_size) {
            mem_free(buf);
            return -2;
        }
    }

    EFI_STATUS status = fs_writefile(s_filepath, buf, size);
    mem_free(buf);

    if (EFI_ERROR(status))
        return -1;

    s_modified = 0;
    return 0;
}
```

Serialize the document, check disk space, write the file, and clear the modified flag. The return value uses three codes: 0 for success, -1 for write failure, and -2 for insufficient disk space.

The disk space check deserves explanation. When saving, we query the filesystem for free space via `fs_volume_info()`. But we also account for the file we are about to replace -- if we are overwriting a 10KB file with a 12KB file, we only need 2KB of free space, not 12KB. The old file's bytes will be reclaimed when the new content is written. On an 8MB SD card, this kind of accounting matters. If the check fails, `doc_save()` returns -2 so the caller can display a specific error message.

### handle_save

```c
static int handle_save(void) {
    int ret = doc_save();
    if (ret == 0) {
        draw_header();
        draw_info("Saved.");
        return 0;
    } else if (ret == -2) {
        draw_info("Error: not enough disk space!");
        return -1;
    } else {
        draw_info("Error: save failed!");
        return -1;
    }
}
```

The user-facing save wrapper. Maps the return codes from `doc_save` to visible messages: "Saved." on success, "Error: not enough disk space!" for the -2 case, and a generic "Error: save failed!" for I/O errors. On success, we also redraw the header to clear the modified indicator (`*`).

## Helpers

Two small utility functions used throughout the drawing code.

### int_to_str

```c
static void int_to_str(int n, char *buf) {
    char tmp[16];
    int t = 0;
    if (n == 0) { tmp[t++] = '0'; }
    else {
        if (n < 0) n = 0;
        while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
    }
    int i = 0;
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = '\0';
}
```

Convert a non-negative integer to a decimal ASCII string. Used to display line and column numbers in the info bar. The digits are extracted in reverse order (least significant first) into a temporary buffer, then copied in reverse to produce the correct order. Negative values are clamped to 0 -- line and column numbers are never negative.

### pad_line

```c
static void pad_line(char *line, int cols) {
    int len = (int)str_len((CHAR8 *)line);
    while (len < cols) {
        line[len] = ' ';
        len++;
    }
    line[cols] = '\0';
}
```

Fill the remainder of a string with spaces to the full screen width and null-terminate. Used by every draw function to ensure old content is overwritten. Without this, if you draw a shorter string over a longer one, the tail end of the old string remains visible.

## Screen Layout and Drawing

The editor divides the screen into four zones:

```
Row 0:         Header — " SURVIVAL EDITOR - filename.txt *"
Rows 1..N-3:   Text content with cursor and selection
Row N-2:       Info bar — "Line 15, Col 23"
Row N-1:       Status bar — "F2:Save  F3:Select  Ctrl+C/X/V  ESC:Exit"
```

The header shows the filename and a `*` when the document has unsaved changes. The info bar shows cursor position (or a custom message during the exit dialog). The status bar shows available keys. The text area fills everything in between.

### draw_header

```c
static void draw_header(void) {
    char line[256];
    int i = 0;
    const char *prefix = " SURVIVAL EDITOR - ";

    while (prefix[i] && i < (int)g_boot.cols) {
        line[i] = prefix[i];
        i++;
    }
    int j = 0;
    while (s_filename[j] && i < (int)g_boot.cols) {
        line[i++] = s_filename[j++];
    }
    if (s_modified && i < (int)g_boot.cols) {
        line[i++] = ' ';
        if (i < (int)g_boot.cols)
            line[i++] = '*';
    }
    line[i] = '\0';
    pad_line(line, (int)g_boot.cols);

    fb_string(0, 0, line, COLOR_CYAN, COLOR_DGRAY);
}
```

Build the header string character by character: prefix, filename, and a ` *` suffix if the document is modified. Pad to full width, then render in cyan on dark gray. The character-by-character construction avoids `sprintf` (which lives in the shim layer, not yet linked at this build stage).

### draw_line

The most complex drawing function, `draw_line` has two paths -- a per-character slow path and a bulk fast path:

```c
static void draw_line(int screen_row, int doc_line) {
    int cols = s_text_cols;
    if (cols > 255) cols = 255;

    int need_per_char = (doc_line >= 0 && doc_line < s_line_count &&
                         (s_sel_active || doc_line == s_cy));
```

The decision: use the per-character path only when the line needs special treatment. That means the cursor line (which needs the cursor cell drawn in inverted colors) or any line that might be inside an active selection (which needs selection highlighting). All other lines take the fast path.

The fast path builds a string and renders it in one `fb_string` call:

```c
    if (!need_per_char) {
        char line[256];
        mem_set(line, ' ', (UINTN)cols);
        line[cols] = '\0';

        if (doc_line >= 0 && doc_line < s_line_count) {
            struct edit_line *ln = &s_lines[doc_line];
            int start = s_scroll_x;
            int i = 0;
            while (i < cols && start + i < (int)ln->len) {
                char ch = ln->data[start + i];
                line[i] = (ch >= 0x20 && ch <= 0x7E) ? ch : '.';
                i++;
            }
        }

        fb_string(0, (UINT32)screen_row, line, COLOR_WHITE, COLOR_BLACK);
    }
```

Fill with spaces first (clears any previous content), then copy visible characters from the line buffer. `s_scroll_x` handles horizontal scrolling -- if the viewport is scrolled 10 columns to the right, we start reading from position 10 in the line data. Non-printable characters are replaced with dots.

The per-character path draws each character individually with `fb_char()`, computing the foreground and background colors for each cell:

```c
    if (need_per_char) {
        struct edit_line *ln = (doc_line >= 0 && doc_line < s_line_count)
                               ? &s_lines[doc_line] : NULL;

        int sy = 0, sx = 0, ey = 0, ex = 0;
        if (s_sel_active)
            sel_get_range(&sy, &sx, &ey, &ex);

        for (int col = 0; col < cols; col++) {
            int doc_col = s_scroll_x + col;
            char ch = ' ';
            if (ln && doc_col < (int)ln->len) {
                ch = ln->data[doc_col];
                if (ch < 0x20 || ch > 0x7E) ch = '.';
            }

            UINT32 fg = COLOR_WHITE;
            UINT32 bg = COLOR_BLACK;

            /* Selection highlight */
            if (s_sel_active) {
                int in_sel = 0;
                if (doc_line > sy && doc_line < ey) {
                    in_sel = 1;
                } else if (doc_line == sy && doc_line == ey) {
                    if (doc_col >= sx && doc_col < ex) in_sel = 1;
                } else if (doc_line == sy && doc_col >= sx) {
                    in_sel = 1;
                } else if (doc_line == ey && doc_col < ex) {
                    in_sel = 1;
                }
                if (in_sel) bg = COLOR_BLUE;
            }

            /* Cursor overrides */
            if (doc_line == s_cy && doc_col == s_cx) {
                fg = COLOR_BLACK;
                bg = COLOR_WHITE;
            }

            fb_char((UINT32)col, (UINT32)screen_row, ch, fg, bg);
        }
    }
}
```

The selection highlight check has four cases. If the line is strictly between the selection start and end lines, every character is selected. If it is the only selected line (start and end on the same line), characters between `sx` and `ex` are selected. If it is the start line, everything from `sx` onward is selected. If it is the end line, everything before `ex` is selected. Selected characters get a blue background.

The cursor is drawn by rendering a single character cell with inverted colors -- black on white instead of white on black. This is the simplest possible cursor implementation: no blinking, no separate cursor sprite, just a color inversion. The cursor overrides selection highlight, so you can always see where you are even inside a selection. At the end of a line, the cursor appears as an inverted space character.

Why two paths? Performance. The per-character path calls `fb_char()` once per visible column -- potentially 80 or more calls per line. The fast path calls `fb_string()` once per line. For a 40-row display, the fast path saves hundreds of function calls per redraw on lines that do not need per-character attention.

### draw_text

```c
static void draw_text(void) {
    for (int r = 0; r < s_text_rows; r++) {
        int doc_line = s_scroll_y + r;
        draw_line(s_text_top + r, doc_line);
    }
}
```

Loop over every visible row in the text area. For each row, calculate which document line it corresponds to (adding the vertical scroll offset), and call `draw_line`. If `doc_line` exceeds `s_line_count`, `draw_line` renders a blank line -- this handles the case where the document is shorter than the screen.

### draw_info

```c
static void draw_info(const char *msg) {
    char line[256];
    int i = 0;

    if (msg) {
        line[i++] = ' ';
        int j = 0;
        while (msg[j] && i < (int)g_boot.cols) {
            line[i++] = msg[j++];
        }
    } else {
        char num[16];

        line[i++] = ' ';
        line[i++] = 'L';
        line[i++] = 'i';
        line[i++] = 'n';
        line[i++] = 'e';
        line[i++] = ' ';
        int_to_str(s_cy + 1, num);
        int j = 0;
        while (num[j] && i < (int)g_boot.cols) line[i++] = num[j++];
        line[i++] = ',';
        line[i++] = ' ';
        line[i++] = 'C';
        line[i++] = 'o';
        line[i++] = 'l';
        line[i++] = ' ';
        int_to_str(s_cx + 1, num);
        j = 0;
        while (num[j] && i < (int)g_boot.cols) line[i++] = num[j++];
    }
    line[i] = '\0';
    pad_line(line, (int)g_boot.cols);

    fb_string(0, g_boot.rows - 2, line, COLOR_GRAY, COLOR_BLACK);
}
```

When `msg` is NULL, display the default cursor position: "Line 15, Col 23" (1-indexed for human readability). When `msg` is non-NULL, display that message instead -- used during the exit dialog to show "Unsaved changes!" and after save to show "Saved." The leading space provides visual padding from the left edge.

### draw_status

```c
static void draw_status(const char *msg) {
    char line[256];
    int i = 0;

    if (!msg)
        msg = " F2:Save  F3:Select  Ctrl+C/X/V  ESC:Exit";

    while (msg[i] && i < (int)g_boot.cols) {
        line[i] = msg[i];
        i++;
    }
    line[i] = '\0';
    pad_line(line, (int)g_boot.cols);

    fb_string(0, g_boot.rows - 1, line, COLOR_GRAY, COLOR_DGRAY);
}
```

The status bar at the bottom of the screen. When `msg` is NULL, it shows the default key hints. The exit dialog temporarily replaces this with " F2:Save  F10:Discard  ESC:Cancel". Rendered in gray on dark gray for a subtle, non-distracting appearance.

### draw_all

```c
static void draw_all(void) {
    fb_clear(COLOR_BLACK);
    draw_header();
    draw_text();
    draw_info(NULL);
    draw_status(NULL);
}
```

Full screen redraw: clear the entire framebuffer to black, then draw all four zones. Called on editor startup and after any operation that invalidates the entire screen (like the exit dialog).

## Scrolling

```c
static void scroll_to_cursor(void) {
    if (s_cy < s_scroll_y)
        s_scroll_y = s_cy;
    if (s_cy >= s_scroll_y + s_text_rows)
        s_scroll_y = s_cy - s_text_rows + 1;

    if (s_cx < s_scroll_x)
        s_scroll_x = s_cx;
    if (s_cx >= s_scroll_x + s_text_cols)
        s_scroll_x = s_cx - s_text_cols + 1;
    if (s_scroll_x < 0)
        s_scroll_x = 0;
}
```

Adjust the viewport so the cursor is always visible. Four boundary checks on two axes. If the cursor moved above the viewport, snap the viewport's top to the cursor line. If below, scroll down just enough to make the cursor visible on the last row. The horizontal axis works identically.

This is called after every input event, before redrawing. It ensures the cursor never disappears off-screen regardless of how it got to its current position -- whether by arrow keys, page up/down, or jumping after a selection deletion.

## Cursor Movement

```c
static void handle_move(UINT16 key) {
    switch (key) {
    case KEY_UP:
        if (s_cy > 0) {
            s_cy--;
            if (s_cx > (int)s_lines[s_cy].len)
                s_cx = (int)s_lines[s_cy].len;
        }
        break;
    case KEY_DOWN:
        if (s_cy < s_line_count - 1) {
            s_cy++;
            if (s_cx > (int)s_lines[s_cy].len)
                s_cx = (int)s_lines[s_cy].len;
        }
        break;
    case KEY_LEFT:
        if (s_cx > 0) {
            s_cx--;
        } else if (s_cy > 0) {
            s_cy--;
            s_cx = (int)s_lines[s_cy].len;
        }
        break;
    case KEY_RIGHT:
        if (s_cx < (int)s_lines[s_cy].len) {
            s_cx++;
        } else if (s_cy < s_line_count - 1) {
            s_cy++;
            s_cx = 0;
        }
        break;
    case KEY_HOME:
        s_cx = 0;
        break;
    case KEY_END:
        s_cx = (int)s_lines[s_cy].len;
        break;
    case KEY_PGUP:
        s_cy -= s_text_rows;
        if (s_cy < 0) s_cy = 0;
        if (s_cx > (int)s_lines[s_cy].len)
            s_cx = (int)s_lines[s_cy].len;
        break;
    case KEY_PGDN:
        s_cy += s_text_rows;
        if (s_cy >= s_line_count) s_cy = s_line_count - 1;
        if (s_cx > (int)s_lines[s_cy].len)
            s_cx = (int)s_lines[s_cy].len;
        break;
    }
}
```

Eight movement directions, each with consistent behavior.

UP and DOWN move vertically and clamp the cursor's column to the new line's length. This prevents the cursor from landing past the end of a shorter line. If you are at column 40 on a 50-character line and move up to a 20-character line, the cursor snaps to column 20 (the end of the shorter line).

LEFT at column 0 wraps to the end of the previous line -- the cursor flows backward through the document as if it were a continuous stream of characters. RIGHT past the end of a line wraps to column 0 of the next line. These wrap behaviors make the cursor feel natural when navigating through text.

HOME jumps to column 0. END jumps to the last character position. Page Up and Page Down move by `s_text_rows` (the number of visible text rows), with the same column clamping as UP/DOWN.

## Editing Operations

Five functions handle text modification. Each checks for an active selection first.

### handle_char

```c
static void handle_char(char c) {
    if (s_sel_active) sel_delete_range();
    line_insert_char(&s_lines[s_cy], s_cx, c);
    s_cx++;
    s_modified = 1;
}
```

If a selection is active, delete it first, then insert the character at the cursor position and advance the cursor. This gives "replace selection by typing" behavior -- select a word, type a new word, and the old one disappears. The modified flag is set.

### handle_enter

```c
static void handle_enter(void) {
    if (s_sel_active) sel_delete_range();
    doc_split_line();
}
```

Delete any active selection first, then split the current line at the cursor. `doc_split_line` handles moving the cursor to the new line and setting the modified flag.

### handle_backspace

```c
static void handle_backspace(void) {
    if (s_sel_active) { sel_delete_range(); return; }
    if (s_cx > 0) {
        s_cx--;
        line_delete_char(&s_lines[s_cy], s_cx);
        s_modified = 1;
    } else if (s_cy > 0) {
        s_cy--;
        s_cx = (int)s_lines[s_cy].len;
        doc_join_lines(s_cy);
    }
}
```

With an active selection, delete the selection and do nothing further. Without a selection, two cases: if the cursor is not at column 0, move the cursor left and delete the character at the new position (the character that was just to the left of the cursor). If the cursor is at column 0, join with the previous line -- the cursor moves to the junction point (the end of the line above), and the current line's content is appended there.

### handle_delete

```c
static void handle_delete(void) {
    if (s_sel_active) { sel_delete_range(); return; }
    if (s_cx < (int)s_lines[s_cy].len) {
        line_delete_char(&s_lines[s_cy], s_cx);
        s_modified = 1;
    } else if (s_cy < s_line_count - 1) {
        doc_join_lines(s_cy);
    }
}
```

The mirror of Backspace. Delete the character at the cursor position (not before it). If the cursor is at the end of the line, join with the line below -- the content of the next line is appended to the current line, and the cursor stays put.

### handle_tab

```c
static void handle_tab(void) {
    for (int i = 0; i < 4; i++)
        handle_char(' ');
}
```

Insert 4 spaces instead of a tab character. This keeps display simple -- tab stops would require column-width calculations in every drawing function, and on a survival workstation, simplicity wins. The 4-space convention matches most modern code editors. Each space goes through `handle_char`, which handles selection deletion on the first call.

## The Exit Dialog

```c
static int handle_exit(void) {
    if (!s_modified)
        return 1;

    draw_status(" F2:Save  F10:Discard  ESC:Cancel");
    draw_info("Unsaved changes!");

    for (;;) {
        struct key_event ev;
        kbd_wait(&ev);

        if (ev.code == KEY_F2) {
            if (handle_save() == 0)
                return 1;
            draw_status(NULL);
            return 0;
        } else if (ev.code == KEY_F10) {
            return 1;
        } else if (ev.code == KEY_ESC) {
            draw_status(NULL);
            draw_info(NULL);
            return 0;
        }
    }
}
```

If the document has not been modified, exit immediately (return 1). Otherwise, present three choices by replacing the status bar and info bar with a dialog.

F2 attempts to save. If the save succeeds, exit. If it fails (disk full, write error), restore the normal status bar and stay in the editor -- the user does not lose their work. F10 discards changes and exits. ESC cancels the exit and returns to editing, restoring the normal status bar and info bar.

The function returns 1 to exit or 0 to stay. The caller checks this return value and either calls `doc_clear()` and returns from `edit_run`, or continues the main loop.

## Selection and Clipboard

Selection and clipboard form a substantial subsystem. Six functions implement the full workflow: normalize the selection range, copy selected text, delete selected text, paste from clipboard, and two line-level convenience operations.

### sel_get_range

```c
static void sel_get_range(int *sy, int *sx, int *ey, int *ex) {
    if (s_sel_anchor_y < s_cy ||
        (s_sel_anchor_y == s_cy && s_sel_anchor_x <= s_cx)) {
        *sy = s_sel_anchor_y; *sx = s_sel_anchor_x;
        *ey = s_cy; *ex = s_cx;
    } else {
        *sy = s_cy; *sx = s_cx;
        *ey = s_sel_anchor_y; *ex = s_sel_anchor_x;
    }
}
```

The anchor might be before or after the cursor -- the user can select forward or backward. This function normalizes the range so that `(sy, sx)` is always the start (earlier in the document) and `(ey, ex)` is always the end. If the anchor is on an earlier line, or on the same line with a smaller column, the anchor is the start. Otherwise the cursor is the start.

This normalization is used by every function that operates on the selection: drawing (to determine which characters are highlighted), copying (to know which characters to capture), and deleting (to know which characters to remove).

### sel_copy

```c
static void sel_copy(void) {
    if (!s_sel_active) return;

    int sy, sx, ey, ex;
    sel_get_range(&sy, &sx, &ey, &ex);

    if (!s_clipboard) s_clipboard = (char *)mem_alloc(CLIP_MAX);
    if (!s_clipboard) return;

    UINTN pos = 0;
    for (int y = sy; y <= ey && pos < CLIP_MAX - 1; y++) {
        int start = (y == sy) ? sx : 0;
        int end = (y == ey) ? ex : (int)s_lines[y].len;
        for (int x = start; x < end && pos < CLIP_MAX - 1; x++)
            s_clipboard[pos++] = s_lines[y].data[x];
        if (y < ey && pos < CLIP_MAX - 1)
            s_clipboard[pos++] = '\n';
    }
    s_clip_len = pos;
    s_clipboard[pos] = '\0';
}
```

Walk through each line in the selection range. For the first line, start at column `sx`. For the last line, stop at column `ex`. For lines in between, take the whole line. Between lines, insert a newline character. The result is a flat string in the clipboard buffer that faithfully represents the selected text, including its line structure.

The clipboard is allocated lazily -- `mem_alloc(CLIP_MAX)` is called on the first copy operation, not at editor startup. This saves 64KB of memory for users who never use copy/paste. If allocation fails (unlikely, but we are defensive), the copy silently does nothing. Every character copy checks `pos < CLIP_MAX - 1` to prevent buffer overrun.

### sel_delete_range

```c
static void sel_delete_range(void) {
    if (!s_sel_active) return;

    int sy, sx, ey, ex;
    sel_get_range(&sy, &sx, &ey, &ex);

    if (sy == ey) {
        for (int i = sx; i < ex; i++)
            line_delete_char(&s_lines[sy], sx);
    } else {
        struct edit_line *first = &s_lines[sy];
        struct edit_line *last = &s_lines[ey];

        first->len = (UINTN)sx;
        first->data[sx] = '\0';

        if (ex < (int)last->len) {
            UINTN remain = last->len - (UINTN)ex;
            line_ensure(first, first->len + remain);
            mem_copy(&first->data[first->len], &last->data[ex], remain);
            first->len += remain;
            first->data[first->len] = '\0';
        }

        for (int i = ey; i > sy; i--)
            doc_delete_line(i);
    }

    s_cy = sy;
    s_cx = sx;
    s_sel_active = 0;
    s_modified = 1;
}
```

Two cases. Single-line selection: delete characters between `sx` and `ex` by calling `line_delete_char` in a loop, always deleting at position `sx`. This works because each deletion shifts the remaining characters left, so the next character to delete slides into the same position.

Multi-line selection is more involved. Truncate the first line at `sx` (chop off everything after the selection start). Take the portion of the last line after `ex` (the unselected tail) and append it to the first line using `line_ensure` and `mem_copy`. Then delete all lines from `ey` down to `sy + 1` -- deleting in reverse order so the indices stay valid. If we deleted forward, each deletion would shift the array and invalidate the remaining indices.

After the deletion, the cursor sits at the selection start `(sy, sx)`, the selection is deactivated, and the document is marked modified.

### sel_paste

```c
static void sel_paste(void) {
    if (!s_clipboard || s_clip_len == 0) return;

    if (s_sel_active)
        sel_delete_range();

    for (UINTN i = 0; i < s_clip_len; i++) {
        if (s_clipboard[i] == '\n') {
            doc_split_line();
        } else {
            line_insert_char(&s_lines[s_cy], s_cx, s_clipboard[i]);
            s_cx++;
        }
    }
    s_modified = 1;
}
```

If a selection is active when the user pastes, delete it first -- this gives "paste over selection" behavior. Then replay the clipboard contents character by character: regular characters are inserted at the cursor position, newlines trigger `doc_split_line()`.

This approach reuses the existing document manipulation functions rather than implementing a separate multi-line insert. The tradeoff is speed -- character-by-character insertion is slower than a bulk operation. But for a 64KB clipboard, the insertion completes in well under a second, which is acceptable for a survival workstation.

### handle_copy_line

```c
static void handle_copy_line(void) {
    struct edit_line *ln = &s_lines[s_cy];
    if (!s_clipboard) s_clipboard = (char *)mem_alloc(CLIP_MAX);
    if (!s_clipboard) return;

    UINTN len = ln->len;
    if (len > CLIP_MAX - 2) len = CLIP_MAX - 2;
    mem_copy(s_clipboard, ln->data, len);
    s_clipboard[len] = '\n';
    s_clip_len = len + 1;
    s_clipboard[s_clip_len] = '\0';
}
```

Copy the entire current line plus a trailing newline into the clipboard, without requiring a selection. The trailing newline is important -- when you paste a copied line, it inserts as a complete line (the newline triggers `doc_split_line`), rather than appending to the current line's content. This is used when Ctrl+C is pressed with no active selection.

### handle_cut_line

```c
static void handle_cut_line(void) {
    handle_copy_line();

    if (s_line_count > 1) {
        doc_delete_line(s_cy);
        if (s_cy >= s_line_count) s_cy = s_line_count - 1;
        if (s_cx > (int)s_lines[s_cy].len) s_cx = (int)s_lines[s_cy].len;
    } else {
        s_lines[0].len = 0;
        s_lines[0].data[0] = '\0';
        s_cx = 0;
    }
    s_modified = 1;
}
```

Copy the line first (into the clipboard), then delete it. If it is the last line in the document, we clear it rather than deleting it -- the editor always needs at least one line. After deletion, clamp the cursor to valid coordinates: if the cursor was on the last line and that line was deleted, move up; if the cursor column exceeds the new line's length, clamp to the end.

Ctrl+K is bound to `handle_cut_line` -- a quick way to remove lines without selecting them first. Each Ctrl+K replaces the clipboard contents. If you need to cut multiple lines at once, select them with F3 first, then use Ctrl+X.

## The Main Loop

```c
void edit_run(const CHAR16 *path, const char *filename) {
    /* Build full filepath */
    int i = 0;
    while (path[i] && i < EDIT_MAX_PATH - 1) {
        s_filepath[i] = path[i];
        i++;
    }
    if (i > 1 || s_filepath[0] != L'\\')
        s_filepath[i++] = L'\\';
    int j = 0;
    while (filename[j] && i < EDIT_MAX_PATH - 1) {
        s_filepath[i++] = (CHAR16)filename[j++];
    }
    s_filepath[i] = 0;

    str_copy(s_filename, filename, sizeof(s_filename));
```

First, construct the full UEFI path by concatenating the directory path, a backslash separator, and the filename. The ASCII filename characters are widened to CHAR16 (UEFI uses UTF-16). The separator is added conditionally -- if the path is just the root `\`, we do not add a double separator.

```c
    /* Setup screen geometry */
    s_text_top = 1;
    s_text_rows = (int)g_boot.rows - 3;
    s_text_cols = (int)g_boot.cols;

    /* Init cursor */
    s_cx = 0;
    s_cy = 0;
    s_scroll_x = 0;
    s_scroll_y = 0;
    s_modified = 0;
    s_sel_active = 0;
```

Calculate the text area dimensions from the screen geometry. The text area starts at row 1 (below the header) and extends to row N-3 (leaving room for the info bar and status bar). Initialize all cursor and scroll state to zero.

```c
    doc_load();
    draw_all();
```

Load the file (or create an empty document if the file does not exist) and draw the full screen.

```c
    struct key_event ev;
    int prev_scroll_y = s_scroll_y;
    int prev_scroll_x = s_scroll_x;
    for (;;) {
        kbd_wait(&ev);

        int s_cy_before = s_cy;
        int cursor_only = 0;
```

The main event loop. `kbd_wait` blocks until a key is pressed. We save the cursor's line before processing the key (`s_cy_before`) and set a flag (`cursor_only`) to track whether the operation was a pure cursor movement. These are used for the incremental redraw optimization below.

```c
        /* Remap CUA shortcuts to Ctrl equivalents */
        UINT16 code = ev.code;
        if (code == KEY_INS && (ev.modifiers & KMOD_SHIFT))
            code = 0x16;  /* Shift+Insert -> Ctrl+V (paste) */
        else if (code == KEY_INS && (ev.modifiers & KMOD_CTRL))
            code = 0x03;  /* Ctrl+Insert -> Ctrl+C (copy) */
        else if (code == KEY_DEL && (ev.modifiers & KMOD_SHIFT))
            code = 0x18;  /* Shift+Delete -> Ctrl+X (cut) */
```

Before dispatching the key, we remap CUA keyboard shortcuts. CUA -- Common User Access -- is a set of keyboard conventions IBM defined for the PS/2 in 1987. Shift+Insert for paste, Ctrl+Insert for copy, Shift+Delete for cut. These predate the Ctrl+C/Ctrl+V conventions that Windows later popularized, and they are still supported by most editors and terminals today. Many experienced users have these in muscle memory.

We remap them to their Ctrl equivalents (0x16 for Ctrl+V, 0x03 for Ctrl+C, 0x18 for Ctrl+X) before the switch statement. This way each operation only needs one case -- the Ctrl version -- and the CUA versions are handled transparently by the remapping.

Now the main dispatch:

```c
        switch (code) {
        case KEY_F2:
            handle_save();
            break;

        case KEY_F3:
            if (s_sel_active) {
                s_sel_active = 0;
            } else {
                s_sel_active = 1;
                s_sel_anchor_y = s_cy;
                s_sel_anchor_x = s_cx;
            }
            break;

        case KEY_ESC:
            if (s_sel_active) {
                s_sel_active = 0;
                break;
            }
            if (handle_exit()) {
                doc_clear();
                return;
            }
            break;

        case KEY_F10:
            if (handle_exit()) {
                doc_clear();
                return;
            }
            break;
```

F2 saves. F3 toggles selection -- on first press, it records the anchor at the current cursor position and activates the selection; on second press, it deactivates. ESC has a two-level behavior: if a selection is active, it cancels the selection (a natural "cancel the current thing" meaning); if no selection is active, it triggers the exit dialog. F10 also triggers the exit dialog (providing a second way to exit, consistent with the exit dialog where F10 means "discard").

```c
        case 0x03: /* Ctrl+C -- copy */
            if (s_sel_active) {
                sel_copy();
                s_sel_active = 0;
            } else {
                handle_copy_line();
            }
            draw_info("Copied.");
            break;

        case 0x18: /* Ctrl+X -- cut */
            if (s_sel_active) {
                sel_copy();
                sel_delete_range();
            } else {
                handle_cut_line();
            }
            break;

        case 0x16: /* Ctrl+V -- paste */
            sel_paste();
            break;

        case 0x0B: /* Ctrl+K -- cut line */
            handle_cut_line();
            break;
```

Ctrl+C (0x03) copies. With an active selection, it copies the selected text to the clipboard and deactivates the selection. Without a selection, it copies the entire current line -- a useful shortcut for quickly grabbing a line.

Ctrl+X (0x18) cuts. With a selection, it copies the text to the clipboard and then deletes the selection. Without a selection, it cuts the entire current line.

Ctrl+V (0x16) pastes. If a selection is active, `sel_paste` replaces the selection with the clipboard contents. Otherwise it inserts at the cursor position.

Ctrl+K (0x0B) always cuts the entire current line, regardless of selection state. This is a quick "delete this line" operation -- one keystroke to remove a line without needing to select it first.

```c
        case KEY_ENTER:
            handle_enter();
            break;

        case KEY_BS:
            handle_backspace();
            break;

        case KEY_DEL:
            handle_delete();
            break;

        case KEY_TAB:
            handle_tab();
            break;

        case KEY_UP:
        case KEY_DOWN:
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_HOME:
        case KEY_END:
        case KEY_PGUP:
        case KEY_PGDN:
            handle_move(ev.code);
            cursor_only = 1;
            break;

        default:
            if (ev.code >= 0x20 && ev.code <= 0x7E)
                handle_char((char)ev.code);
            break;
        }
```

The editing keys dispatch to their respective handlers. Movement keys set `cursor_only = 1` so the redraw optimization knows it only needs to update the cursor line(s). Printable ASCII characters (0x20 through 0x7E) go through `handle_char`. Everything else -- function keys we do not handle, control characters we do not recognize -- is silently ignored.

```c
        int prev_cy = s_cy_before;
        scroll_to_cursor();

        if (cursor_only && !s_sel_active &&
            s_scroll_y == prev_scroll_y && s_scroll_x == prev_scroll_x) {
            int old_row = prev_cy - s_scroll_y;
            int new_row = s_cy - s_scroll_y;
            if (old_row >= 0 && old_row < s_text_rows)
                draw_line(s_text_top + old_row, s_scroll_y + old_row);
            if (new_row != old_row && new_row >= 0 && new_row < s_text_rows)
                draw_line(s_text_top + new_row, s_scroll_y + new_row);
            draw_info(NULL);
        } else {
            draw_header();
            draw_text();
            draw_info(NULL);
        }

        prev_scroll_y = s_scroll_y;
        prev_scroll_x = s_scroll_x;
    }
}
```

After processing the key, scroll the viewport if needed, then redraw. The redraw has two paths:

**Incremental redraw** (the fast path): if the operation was a pure cursor movement, there is no active selection, and the scroll position did not change, then we only need to redraw two lines -- the line the cursor was on (to remove the old cursor rendering) and the line the cursor is now on (to draw the new cursor). If the cursor stayed on the same line, only one line needs redrawing. The info bar is always updated (to show the new line/column numbers).

**Full redraw** (the slow path): for everything else -- text edits, selection changes, scroll changes -- redraw the header, all text lines, and the info bar. This is simple but not wasteful: at 80x25 characters, redrawing the full text area takes under a millisecond.

The incremental optimization matters more than you might think. On UEFI firmware, each `fb_char` call writes directly to the linear framebuffer -- there is no hardware text mode, no GPU acceleration, just CPU writes to memory-mapped video RAM. Reducing the number of cells redrawn from 2000 (full screen) to 160 (two lines) makes cursor movement noticeably smoother, especially on slower ARM boards.

## Integrating with the Browser

In `browse.c`, we replace the file viewer from Chapter 10 with the editor. The old `view_file()` function (125 lines of read-only display code) is removed entirely and replaced with a simple wrapper:

```c
#include "edit.h"

static void open_file(void) {
    if (s_count <= 0 || s_entries[s_cursor].is_dir)
        return;
    edit_run(s_path, s_entries[s_cursor].name);
}
```

ENTER on a file now launches the editor instead of the viewer. The editor receives the current directory path and the selected filename. When the editor returns (the user pressed ESC or F10), the browser reloads the directory listing (in case a file was created or modified) and redraws.

We also add F4 for creating new files:

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

A minimal inline text input rendered on the status bar. The prompt shows " New filename: " followed by the characters typed so far and an underscore cursor. Type a filename, press ENTER to create, ESC to cancel. The editor opens with an empty document -- the file does not exist on disk until the user first saves.

This reuses the same input pattern we developed for the browser's navigation: a character-by-character loop with `kbd_wait`, handling printable characters, backspace, enter, and escape. The `pad_line` call ensures the previous status bar content is overwritten.

The browser's status bar updates to show the new key:

```c
const char *status = " ENTER:Open  F4:New  BS:Back  ESC:Exit";
```

## What We Built

About 700 lines of C, implementing a complete text editor that runs on bare UEFI hardware with no operating system. It handles:

- Loading existing files with proper line splitting and CRLF handling
- Cursor movement in all eight directions with line-length clamping and line wrapping
- Character insertion and deletion with automatic buffer growth
- Line splitting (Enter) and joining (Backspace/Delete at line boundaries)
- Text selection via F3 mark/unmark toggle, with visual highlighting
- Clipboard operations: copy, cut, and paste (Ctrl+C, Ctrl+X, Ctrl+V)
- CUA keyboard shortcuts (Shift+Insert, Ctrl+Insert, Shift+Delete)
- Line-level copy and cut (Ctrl+C with no selection, Ctrl+K)
- Horizontal scrolling for lines longer than the screen
- File saving with disk space checking and overwrite accounting
- Unsaved changes protection with a save/discard/cancel dialog
- New file creation from the browser via F4
- Incremental redraw optimization for smooth cursor movement

This completes Phase 2. The workstation is now a usable read/write system -- you can browse files, open them in an editor, make changes, save, and create new files. It is not fancy, but it works. You could write survival notes, document procedures, or draft code with it.

Phase 3 will add TinyCC, turning the workstation into a system that can compile and run its own programs. The editor will gain F5 (compile and run) and F6 (rebuild the workstation from its own source code). But the foundation we built here -- the line buffer, the document operations, the selection model, the drawing engine -- remains unchanged. Everything we add in Phase 3 builds on top of this editor.
