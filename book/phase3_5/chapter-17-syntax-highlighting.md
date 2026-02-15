# Chapter 17: Syntax Highlighting

## Phase 3.5: Quality of Life

Phase 3 achieved self-hosting. The workstation can compile its own source code, write the resulting binary to disk, and reboot into the new version. It carries a C compiler, a text editor, a file browser, and a FAT32 filesystem driver — all in under 700KB.

Phase 3.5 adds quality-of-life features. These aren't necessary for survival, but they make the workstation bearable to use for extended periods. This chapter adds syntax highlighting to the editor. 

## Why Syntax Highlighting

Reading dense C code on a monochrome white-on-black screen is hard. Every character has the same weight. Your eyes have to parse the structure — find where comments end, where strings begin, which words are keywords and which are variables — with no visual cues. It's like reading a wall of text with no paragraph breaks.

Colors change this. Keywords stand out in blue. Strings are immediately obvious in warm brown. Comments fade into the background in olive green. You can glance at a screenful of code and instantly see the structure: here's the logic, there's the commentary, that's a string literal.

We're staring at code all day on this workstation. Colors make it bearable.

## The Color Scheme

We use a palette inspired by VS Code's Dark+ theme — a widely familiar scheme that works well on a dark background. The colors are defined as `SYN_*` constants at the top of `edit.c`, not in `boot.h`. These are editor-specific — the file browser and other modules don't need them.

```c
/* Syntax colors (VS Code Dark+ inspired) */
#define SYN_DEFAULT   0x00D4D4D4  /* light gray */
#define SYN_KEYWORD   0x00569CD6  /* blue */
#define SYN_TYPE      0x004EC9B0  /* teal */
#define SYN_STRING    0x00CE9178  /* warm brown */
#define SYN_NUMBER    0x00B5CEA8  /* light green */
#define SYN_COMMENT   0x006A9955  /* olive green */
#define SYN_PREPROC   0x00C586C0  /* purple */
```

Here's what each token type looks like:

| Token | Constant | Hex Value | Visual |
|-------|----------|-----------|--------|
| Default text | SYN_DEFAULT | 0x00D4D4D4 | Light gray |
| Keywords (`if`, `return`, `while`) | SYN_KEYWORD | 0x00569CD6 | Blue |
| Types (`int`, `UINT32`, `EFI_STATUS`) | SYN_TYPE | 0x004EC9B0 | Teal |
| String and char literals | SYN_STRING | 0x00CE9178 | Warm brown |
| Numeric literals | SYN_NUMBER | 0x00B5CEA8 | Light green |
| Comments (`//` and `/* */`) | SYN_COMMENT | 0x006A9955 | Olive green |
| Preprocessor lines (`#include`, `#define`) | SYN_PREPROC | 0x00C586C0 | Purple |

A note on the pixel format. Our framebuffer uses BGRX 32-bit pixels — blue in the low byte, green in the middle, red high. But these hex constants are written as `0x00RRGGBB` because on a little-endian machine, the bytes are stored in memory as BB GG RR 00, which is exactly the BGRX layout the framebuffer expects. The `COLOR_*` constants in `boot.h` follow the same convention. So `0x00569CD6` means R=0x56, G=0x9C, B=0xD6 — a medium blue.

## Deciding When to Highlight

Not every file should be highlighted. Opening a plain text file or a binary in the editor should show normal white-on-black text. We only colorize C source:

```c
static int is_c_or_h_file(void) {
    int len = (int)str_len((CHAR8 *)s_filename);
    if (len >= 2 && s_filename[len - 2] == '.' && s_filename[len - 1] == 'c')
        return 1;
    if (len >= 2 && s_filename[len - 2] == '.' && s_filename[len - 1] == 'h')
        return 1;
    return 0;
}
```

Simple suffix check — `.c` or `.h`. No fancy MIME type detection, no file content sniffing. When `edit_run()` opens a file, it calls this once and stores the result:

```c
s_highlight_mode = is_c_or_h_file();
```

The `s_highlight_mode` flag controls everything downstream. When it's zero, the editor behaves exactly as before — no performance cost, no color changes.

## Cross-Line Block Comment State

Most syntax tokens are contained within a single line. Keywords, numbers, strings (usually), preprocessor directives — they start and end on the same line. But block comments don't. A `/*` on line 10 might not close until `*/` on line 50. Every line in between is entirely comment, and the highlighter needs to know that.

Before rendering the text, `hl_compute_states()` scans the entire document from top to bottom, tracking whether we're inside a block comment or not. It stores the result for each line in `s_line_state[]`:

```c
static UINT8 s_line_state[EDIT_MAX_LINES]; /* start-of-line comment state */

static void hl_compute_states(void) {
    int in_comment = 0;
    for (int i = 0; i < s_line_count; i++) {
        s_line_state[i] = in_comment ? HL_COMMENT : HL_NORMAL;
        struct edit_line *ln = &s_lines[i];
        for (int j = 0; j < (int)ln->len - 1; j++) {
            if (!in_comment && ln->data[j] == '/' && ln->data[j + 1] == '*') {
                in_comment = 1;
                j++;
            } else if (in_comment && ln->data[j] == '*' && ln->data[j + 1] == '/') {
                in_comment = 0;
                j++;
            }
        }
    }
}
```

The state array has two possible values: `HL_NORMAL` (0) and `HL_COMMENT` (1). Each entry records whether the line starts inside a block comment. The per-line tokenizer reads this to know whether to begin in comment mode.

Why rescan the entire document every render? Because it's simple and fast enough. The scan visits every character in every line — but it only checks two-character sequences (`/*` and `*/`), with no allocations and no branching beyond the inner `if`. For our 4096-line limit, this completes in microseconds. The alternative — incrementally updating comment state when edits happen — would be correct, but far more code. Every insertion, deletion, and line split could potentially open or close a comment that changes the state of hundreds of subsequent lines. Tracking that incrementally is a classic source of subtle bugs. The brute-force rescan is always correct by construction.

The function is called once per `draw_text()`:

```c
static void draw_text(void) {
    if (s_highlight_mode)
        hl_compute_states();
    for (int r = 0; r < s_text_rows; r++) {
        int doc_line = s_scroll_y + r;
        draw_line(s_text_top + r, doc_line);
    }
}
```

## The Per-Line Tokenizer

`hl_colorize_line()` is the core of the highlighting system. It walks one line of text character by character, filling a `colors[]` array with a UINT32 color value for each visible screen column. The `draw_line()` function then uses these colors as foreground values when rendering each character.

The function signature:

```c
static void hl_colorize_line(int doc_line, UINT32 *colors, int max_cols,
                             int scroll_x);
```

It takes the document line index, a color output array, the number of visible columns, and the horizontal scroll offset. The scroll offset matters because we only need colors for the visible portion of the line — a 200-character line scrolled to show columns 80-159 only needs colors for those 80 columns.

Processing order matters. The tokenizer checks for each token type in a specific sequence, and each check consumes characters before the next check runs. Here's the order:

1. **Block comment continuation** — if `s_line_state[]` says we enter this line inside a block comment, color everything `SYN_COMMENT` until we find `*/`.

2. **Line comment `//`** — two slashes color the rest of the line as comment and stop.

3. **Block comment start `/*`** — color the opening pair and switch into comment mode for the remainder of the line.

4. **String and char literals** — opening `"` or `'` colors everything through the closing quote, including backslash escapes like `\"` and `\\`.

5. **Preprocessor `#`** — if the `#` appears at the start of the line (ignoring leading whitespace), color the entire line purple.

6. **Numbers** — decimal digits, hex with `0x` prefix, float suffixes. But only if not preceded by a letter or underscore (so `x86` doesn't highlight `86`).

7. **Identifiers** — alphanumeric words are collected and checked against the keyword and type tables.

8. **Everything else** — operators, punctuation, whitespace — gets the default light gray.

This order is deliberate. Comments must be checked first because `//` inside a string should not start a comment, but we handle that by consuming the string before reaching the comment check. Preprocessor lines are checked before numbers and identifiers because `#define FOO 42` should color the entire line purple, not just `#define` purple with `FOO` default and `42` green.

### The Keyword and Type Tables

```c
static const char *s_keywords[] = {
    "auto", "break", "case", "const", "continue", "default", "do",
    "else", "enum", "extern", "for", "goto", "if", "inline",
    "register", "return", "sizeof", "static", "struct", "switch",
    "typedef", "union", "volatile", "while", NULL
};

static const char *s_types[] = {
    "char", "double", "float", "int", "long", "short", "signed",
    "unsigned", "void",
    "UINT8", "UINT16", "UINT32", "UINT64", "INT8", "INT16", "INT32", "INT64",
    "UINTN", "INTN", "CHAR8", "CHAR16", "BOOLEAN",
    "EFI_STATUS", "EFI_HANDLE", "EFI_GUID", "EFI_EVENT",
    "EFI_FILE_HANDLE", "EFI_FILE_INFO",
    "NULL", "TRUE", "FALSE",
    NULL
};
```

The keyword table has the 24 standard C keywords (minus the types, which go in the type table). The type table has standard C types (`int`, `char`, `void`, etc.) plus UEFI types (`UINT32`, `EFI_STATUS`, `BOOLEAN`) plus constants that act like types in practice (`NULL`, `TRUE`, `FALSE`). These are the words we see most often in our workstation source code — having them colored distinctly from regular identifiers makes the code much easier to scan.

Both tables are NULL-terminated arrays of string pointers. A helper function checks whether an identifier matches any entry:

```c
static int match_word(const char *word, int wlen, const char **table) {
    for (int i = 0; table[i]; i++) {
        const char *t = table[i];
        int tlen = 0;
        while (t[tlen]) tlen++;
        if (tlen == wlen) {
            int ok = 1;
            for (int j = 0; j < wlen; j++) {
                if (word[j] != t[j]) { ok = 0; break; }
            }
            if (ok) return 1;
        }
    }
    return 0;
}
```

Linear scan, manual string comparison. No hash table, no sorting, no binary search. With ~55 total entries and identifiers averaging 5-6 characters, this runs in microseconds. We could optimize if profiling showed it mattered, but it won't — the inner loop breaks immediately on length mismatch, and most identifiers are not keywords.

### Strings and Escapes

The string literal handling deserves a closer look:

```c
if (ln->data[i] == '"' || ln->data[i] == '\'') {
    char quote = ln->data[i];
    if (col >= 0 && col < max_cols) colors[col] = SYN_STRING;
    i++;
    while (i < (int)ln->len && ln->data[i] != quote) {
        col = i - scroll_x;
        if (col >= 0 && col < max_cols) colors[col] = SYN_STRING;
        if (ln->data[i] == '\\' && i + 1 < (int)ln->len) {
            i++;
            col = i - scroll_x;
            if (col >= 0 && col < max_cols) colors[col] = SYN_STRING;
        }
        i++;
    }
    if (i < (int)ln->len) {
        col = i - scroll_x;
        if (col >= 0 && col < max_cols) colors[col] = SYN_STRING;
        i++;
    }
}
```

When we see a `"` or `'`, we remember which quote character opened the literal. Everything until the matching close quote is colored as string — including backslash escape sequences. The `\\` check skips the character after the backslash, so `\"` doesn't end the string. If the line ends without a closing quote (unterminated string), we just stop — the next line starts fresh, which is the correct behavior for C (strings can't span lines without a backslash continuation).

### Numbers

The number recognition handles several formats:

```c
if ((ln->data[i] >= '0' && ln->data[i] <= '9') ||
    (ln->data[i] == '.' && i + 1 < (int)ln->len &&
     ln->data[i + 1] >= '0' && ln->data[i + 1] <= '9')) {
    int prev_ident = 0;
    if (i > 0) {
        char p = ln->data[i - 1];
        if ((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z') || p == '_')
            prev_ident = 1;
    }
    if (!prev_ident) {
        while (i < (int)ln->len) {
            char c = ln->data[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F') || c == 'x' || c == 'X' ||
                c == '.' || c == 'U' || c == 'L' || c == 'u' || c == 'l')
            {
                col = i - scroll_x;
                if (col >= 0 && col < max_cols)
                    colors[col] = SYN_NUMBER;
                i++;
            } else {
                break;
            }
        }
        continue;
    }
}
```

Decimal (`42`), hexadecimal (`0xFF`), floating point (`3.14`), and suffixed (`100UL`, `2.0f`) numbers are all recognized. The `prev_ident` check prevents false matches — `x86` should not highlight `86` as a number, because the preceding `x` means this is part of an identifier, not a standalone number.

## Integration with draw_line()

Chapter 12 built `draw_line()` with two rendering paths: a per-character path (using `fb_char()`) for lines that need the cursor or selection highlight, and a fast path (using `fb_string()`) for everything else. Syntax highlighting hooks into the existing per-character path.

The key change is in the condition that decides which path to use:

```c
int need_per_char = (doc_line >= 0 && doc_line < s_line_count &&
                     (s_sel_active || doc_line == s_cy || s_highlight_mode));
```

When `s_highlight_mode` is set, `need_per_char` is true for every valid document line. Every line needs per-character rendering because every character might have a different color.

Inside the per-character path, the color computation feeds into the existing loop:

```c
/* Compute syntax colors for this line */
UINT32 syn_colors[256];
if (s_highlight_mode && ln) {
    hl_colorize_line(doc_line, syn_colors, cols, s_scroll_x);
} else {
    for (int i = 0; i < cols; i++) syn_colors[i] = COLOR_WHITE;
}

for (int col = 0; col < cols; col++) {
    int doc_col = s_scroll_x + col;
    char ch = ' ';
    if (ln && doc_col < (int)ln->len) {
        ch = ln->data[doc_col];
        if (ch < 0x20 || ch > 0x7E) ch = '.';
    }

    UINT32 fg = syn_colors[col];
    UINT32 bg = COLOR_BLACK;

    /* Selection highlight */
    if (s_sel_active) {
        /* ... selection check ... */
        if (in_sel) bg = COLOR_BLUE;
    }

    /* Cursor overrides */
    if (doc_line == s_cy && doc_col == s_cx) {
        fg = COLOR_BLACK;
        bg = COLOR_WHITE;
    }

    fb_char((UINT32)col, (UINT32)screen_row, ch, fg, bg);
}
```

The foreground color starts from `syn_colors[col]` instead of a fixed `COLOR_WHITE`. Selection sets the background to blue but preserves the syntax color as the foreground — so selected keywords are still blue text, just on a blue background (which is admittedly subtle, but selection is transient). The cursor overrides everything with inverted black-on-white, because you always need to see where the cursor is.

The fast path (`fb_string()`) is preserved for non-C files that have no active selection and where the cursor isn't on the line. A plain text file still renders with the same speed as before.

## The Complete Module

The syntax highlighting system adds approximately 200 lines to `edit.c`:

- **Two tables** — `s_keywords[]` (24 entries) and `s_types[]` (31 entries), both NULL-terminated.
- **One state array** — `s_line_state[4096]`, one byte per line, recording block comment state.
- **Three functions** — `hl_compute_states()` (15 lines), `match_word()` (15 lines), and `hl_colorize_line()` (120 lines).
- **One predicate** — `is_c_or_h_file()` (6 lines).

No allocations. No persistent parse tree. No incremental update logic. Colors are computed fresh from scratch on every render — this means editing never puts the highlighting into an inconsistent state. Insert a `/*` on line 10 and every line below it immediately turns olive green. Delete it and they all snap back. The brute-force approach is always correct.

There's also no separate syntax highlighting module or header file. Everything lives in `edit.c` as static functions and static data. The highlighting is an implementation detail of the editor's rendering, not a reusable library. Keeping it local means no API surface to maintain and no cross-file dependencies to manage.

Binary size barely changed: 689K aarch64, 662K x86_64. The tables are small (a few hundred bytes of string pointers), the state array is 4KB, and the functions are straightforward loops. The keyword strings themselves live in `.rodata`.

## What We Have

The editor now shows C code in color. Open any `.c` or `.h` file and highlighting activates automatically. Keywords appear in blue, types in teal, strings in warm brown, comments in olive green, preprocessor lines in purple, numbers in light green. Everything else stays light gray.

Open a plain text file and you get the same white-on-black rendering as before. The highlighting is invisible unless you're editing C source.

This is a small feature measured by lines of code, but a large one measured by hours of use. Every minute spent reading code in the editor benefits from the visual structure that colors provide.
