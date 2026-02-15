#include "edit.h"
#include "fb.h"
#include "kbd.h"
#include "fs.h"
#include "mem.h"
#include "tcc.h"
#include "shim.h"
#include "libtcc.h"

#define EDIT_MAX_LINES  4096
#define EDIT_MAX_PATH   512
#define EDIT_INIT_CAP   80

/* ---- Line buffer ---- */

struct edit_line {
    char  *data;
    UINTN  len;
    UINTN  cap;
};

/* ---- Editor state ---- */

static struct edit_line s_lines[EDIT_MAX_LINES];
static int    s_line_count;
static int    s_cx, s_cy;           /* cursor column, row in document */
static int    s_scroll_x, s_scroll_y;
static int    s_modified;
static CHAR16 s_filepath[EDIT_MAX_PATH];
static char   s_filename[128];

/* Selection and clipboard */
static int    s_sel_active;
static int    s_sel_anchor_y, s_sel_anchor_x;
static char  *s_clipboard;
static UINTN  s_clip_len;
#define CLIP_MAX 65536

/* Screen geometry */
static int s_text_top;    /* first screen row for text (1) */
static int s_text_rows;   /* rows available for text */
static int s_text_cols;   /* columns available for text */

/* Syntax highlighting */
static int   s_highlight_mode;             /* 1 for .c/.h files */
#define HL_NORMAL  0
#define HL_COMMENT 1
static UINT8 s_line_state[EDIT_MAX_LINES]; /* start-of-line comment state */

/* Syntax colors (VS Code Dark+ inspired) */
#define SYN_DEFAULT   0x00D4D4D4  /* light gray */
#define SYN_KEYWORD   0x00569CD6  /* blue */
#define SYN_TYPE      0x004EC9B0  /* teal */
#define SYN_STRING    0x00CE9178  /* warm brown */
#define SYN_NUMBER    0x00B5CEA8  /* light green */
#define SYN_COMMENT   0x006A9955  /* olive green */
#define SYN_PREPROC   0x00C586C0  /* purple */

/* ---- Line buffer management ---- */

static void line_init(struct edit_line *ln) {
    ln->cap = EDIT_INIT_CAP;
    ln->data = (char *)mem_alloc(ln->cap);
    ln->data[0] = '\0';
    ln->len = 0;
}

static void line_free(struct edit_line *ln) {
    if (ln->data) {
        mem_free(ln->data);
        ln->data = NULL;
    }
    ln->len = 0;
    ln->cap = 0;
}

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

static void line_insert_char(struct edit_line *ln, int pos, char c) {
    line_ensure(ln, ln->len + 1);
    /* Shift right from end to pos */
    for (int i = (int)ln->len; i > pos; i--)
        ln->data[i] = ln->data[i - 1];
    ln->data[pos] = c;
    ln->len++;
    ln->data[ln->len] = '\0';
}

static void line_delete_char(struct edit_line *ln, int pos) {
    if (pos < 0 || pos >= (int)ln->len)
        return;
    for (int i = pos; i < (int)ln->len - 1; i++)
        ln->data[i] = ln->data[i + 1];
    ln->len--;
    ln->data[ln->len] = '\0';
}

/* ---- Document operations ---- */

static void doc_insert_line(int idx) {
    if (s_line_count >= EDIT_MAX_LINES)
        return;
    /* Shift lines down */
    for (int i = s_line_count; i > idx; i--)
        s_lines[i] = s_lines[i - 1];
    s_line_count++;
    /* Initialize the new slot */
    mem_set(&s_lines[idx], 0, sizeof(struct edit_line));
    line_init(&s_lines[idx]);
}

static void doc_delete_line(int idx) {
    if (idx < 0 || idx >= s_line_count)
        return;
    line_free(&s_lines[idx]);
    for (int i = idx; i < s_line_count - 1; i++)
        s_lines[i] = s_lines[i + 1];
    s_line_count--;
    mem_set(&s_lines[s_line_count], 0, sizeof(struct edit_line));
}

static void doc_split_line(void) {
    /* Split current line at cursor into two lines */
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

static void doc_join_lines(int line_idx) {
    /* Append line_idx+1 onto line_idx, remove line_idx+1 */
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

static void doc_load(void) {
    UINTN file_size = 0;
    char *data = (char *)fs_readfile(s_filepath, &file_size);

    s_line_count = 0;
    mem_set(s_lines, 0, sizeof(s_lines));

    if (!data || file_size == 0) {
        /* Empty / new file — start with one blank line */
        line_init(&s_lines[0]);
        s_line_count = 1;
        if (data) mem_free(data);
        return;
    }

    /* Split into independent line buffers */
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

            /* Skip \n after \r */
            if (!is_end && data[i] == '\r' && i + 1 < file_size && data[i + 1] == '\n')
                i++;
            start = i + 1;
        }
    }

    /* Ensure at least one line */
    if (s_line_count == 0) {
        line_init(&s_lines[0]);
        s_line_count = 1;
    }

    mem_free(data);
}

static char *doc_serialize(UINTN *out_size) {
    /* Calculate total size: all lines + newlines */
    UINTN total = 0;
    for (int i = 0; i < s_line_count; i++) {
        total += s_lines[i].len;
        if (i < s_line_count - 1)
            total++;  /* newline between lines */
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

static int doc_save(void) {
    UINTN size = 0;
    char *buf = doc_serialize(&size);
    if (!buf)
        return -1;

    /* Check disk space (account for existing file we'll replace) */
    UINT64 total_bytes, free_bytes;
    if (fs_volume_info(&total_bytes, &free_bytes) == 0) {
        UINT64 old_size = fs_file_size(s_filepath);
        if ((UINT64)size > free_bytes + old_size) {
            mem_free(buf);
            return -2;  /* out of space */
        }
    }

    EFI_STATUS status = fs_writefile(s_filepath, buf, size);
    mem_free(buf);

    if (EFI_ERROR(status))
        return -1;

    s_modified = 0;
    return 0;
}

static void doc_clear(void) {
    for (int i = 0; i < s_line_count; i++)
        line_free(&s_lines[i]);
    s_line_count = 0;
}

/* ---- Selection and clipboard ---- */

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

static void sel_delete_range(void) {
    if (!s_sel_active) return;

    int sy, sx, ey, ex;
    sel_get_range(&sy, &sx, &ey, &ex);

    if (sy == ey) {
        /* Single line deletion */
        for (int i = sx; i < ex; i++)
            line_delete_char(&s_lines[sy], sx);
    } else {
        /* Multi-line: keep start of first, keep end of last */
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

/* ---- Helpers ---- */

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

static void pad_line(char *line, int cols) {
    int len = (int)str_len((CHAR8 *)line);
    while (len < cols) {
        line[len] = ' ';
        len++;
    }
    line[cols] = '\0';
}

/* ---- Syntax highlighting ---- */

static int is_c_or_h_file(void) {
    int len = (int)str_len((CHAR8 *)s_filename);
    if (len >= 2 && s_filename[len - 2] == '.' && s_filename[len - 1] == 'c')
        return 1;
    if (len >= 2 && s_filename[len - 2] == '.' && s_filename[len - 1] == 'h')
        return 1;
    return 0;
}

/* Scan all lines to determine per-line block comment state */
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

/* Check if identifier matches any in a table */
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

/* Colorize one line into a color array.
   colors[] must have at least max_cols entries.
   Returns with colors filled for visible columns. */
static void hl_colorize_line(int doc_line, UINT32 *colors, int max_cols,
                             int scroll_x) {
    struct edit_line *ln = &s_lines[doc_line];
    int in_comment = (s_line_state[doc_line] == HL_COMMENT);
    (void)0;  /* string/char handled inline below */

    /* Default all to normal text color */
    for (int i = 0; i < max_cols; i++)
        colors[i] = SYN_DEFAULT;

    /* Walk the line */
    int i = 0;
    while (i < (int)ln->len) {
        int col = i - scroll_x;  /* screen column */

        /* Block comment */
        if (in_comment) {
            if (i + 1 < (int)ln->len && ln->data[i] == '*' && ln->data[i + 1] == '/') {
                if (col >= 0 && col < max_cols) colors[col] = SYN_COMMENT;
                if (col + 1 >= 0 && col + 1 < max_cols) colors[col + 1] = SYN_COMMENT;
                in_comment = 0;
                i += 2;
                continue;
            }
            if (col >= 0 && col < max_cols) colors[col] = SYN_COMMENT;
            i++;
            continue;
        }

        /* Line comment */
        if (ln->data[i] == '/' && i + 1 < (int)ln->len && ln->data[i + 1] == '/') {
            while (i < (int)ln->len) {
                col = i - scroll_x;
                if (col >= 0 && col < max_cols) colors[col] = SYN_COMMENT;
                i++;
            }
            break;
        }

        /* Block comment start */
        if (ln->data[i] == '/' && i + 1 < (int)ln->len && ln->data[i + 1] == '*') {
            if (col >= 0 && col < max_cols) colors[col] = SYN_COMMENT;
            if (col + 1 >= 0 && col + 1 < max_cols) colors[col + 1] = SYN_COMMENT;
            in_comment = 1;
            i += 2;
            continue;
        }

        /* String literal */
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
            continue;
        }

        /* Preprocessor */
        if (ln->data[i] == '#') {
            /* Check if # is at start of line (ignoring whitespace) */
            int is_pp = 1;
            for (int j = 0; j < i; j++) {
                if (ln->data[j] != ' ' && ln->data[j] != '\t') {
                    is_pp = 0;
                    break;
                }
            }
            if (is_pp) {
                while (i < (int)ln->len) {
                    col = i - scroll_x;
                    if (col >= 0 && col < max_cols) colors[col] = SYN_PREPROC;
                    i++;
                }
                break;
            }
        }

        /* Numbers */
        if ((ln->data[i] >= '0' && ln->data[i] <= '9') ||
            (ln->data[i] == '.' && i + 1 < (int)ln->len &&
             ln->data[i + 1] >= '0' && ln->data[i + 1] <= '9')) {
            /* Check not part of identifier */
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

        /* Identifiers → keyword/type check */
        if ((ln->data[i] >= 'a' && ln->data[i] <= 'z') ||
            (ln->data[i] >= 'A' && ln->data[i] <= 'Z') || ln->data[i] == '_') {
            int start = i;
            while (i < (int)ln->len) {
                char c = ln->data[i];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_')
                    i++;
                else
                    break;
            }
            int wlen = i - start;
            UINT32 wcolor = SYN_DEFAULT;
            if (match_word(&ln->data[start], wlen, s_keywords))
                wcolor = SYN_KEYWORD;
            else if (match_word(&ln->data[start], wlen, s_types))
                wcolor = SYN_TYPE;
            if (wcolor != SYN_DEFAULT) {
                for (int j = start; j < i; j++) {
                    col = j - scroll_x;
                    if (col >= 0 && col < max_cols)
                        colors[col] = wcolor;
                }
            }
            continue;
        }

        /* Everything else: default white */
        i++;
    }
}

/* ---- Drawing ---- */

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
    if (fs_is_read_only() && i + 12 < (int)g_boot.cols) {
        const char *ro = " [READ-ONLY]";
        int k = 0;
        while (ro[k] && i < (int)g_boot.cols)
            line[i++] = ro[k++];
    }
    line[i] = '\0';
    pad_line(line, (int)g_boot.cols);

    fb_string(0, 0, line, COLOR_CYAN, COLOR_DGRAY);
}

static void draw_line(int screen_row, int doc_line) {
    int cols = s_text_cols;
    if (cols > 255) cols = 255;

    int need_per_char = (doc_line >= 0 && doc_line < s_line_count &&
                         (s_sel_active || doc_line == s_cy || s_highlight_mode));

    if (need_per_char) {
        struct edit_line *ln = (doc_line >= 0 && doc_line < s_line_count)
                               ? &s_lines[doc_line] : NULL;

        int sy = 0, sx = 0, ey = 0, ex = 0;
        if (s_sel_active)
            sel_get_range(&sy, &sx, &ey, &ex);

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
    } else {
        /* Fast path: no selection, no highlight, not cursor line */
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
}

static void draw_text(void) {
    if (s_highlight_mode)
        hl_compute_states();
    for (int r = 0; r < s_text_rows; r++) {
        int doc_line = s_scroll_y + r;
        draw_line(s_text_top + r, doc_line);
    }
}

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
        /* Default: show line/col */
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

static void draw_status(const char *msg) {
    char line[256];
    int i = 0;

    if (!msg) {
        if (fs_is_read_only())
            msg = " F3:Select  ESC:Exit  [READ-ONLY]";
        else
            msg = " F2:Save  F3:Select  F5:Run  F6:Rebuild  ESC:Exit";
    }

    while (msg[i] && i < (int)g_boot.cols) {
        line[i] = msg[i];
        i++;
    }
    line[i] = '\0';
    pad_line(line, (int)g_boot.cols);

    fb_string(0, g_boot.rows - 1, line, COLOR_GRAY, COLOR_DGRAY);
}

static void draw_all(void) {
    fb_clear(COLOR_BLACK);
    draw_header();
    draw_text();
    draw_info(NULL);
    draw_status(NULL);
}

/* ---- Scrolling ---- */

static void scroll_to_cursor(void) {
    /* Vertical */
    if (s_cy < s_scroll_y)
        s_scroll_y = s_cy;
    if (s_cy >= s_scroll_y + s_text_rows)
        s_scroll_y = s_cy - s_text_rows + 1;

    /* Horizontal */
    if (s_cx < s_scroll_x)
        s_scroll_x = s_cx;
    if (s_cx >= s_scroll_x + s_text_cols)
        s_scroll_x = s_cx - s_text_cols + 1;
    if (s_scroll_x < 0)
        s_scroll_x = 0;
}

/* ---- Input handling ---- */

static void handle_char(char c) {
    if (s_sel_active) sel_delete_range();
    line_insert_char(&s_lines[s_cy], s_cx, c);
    s_cx++;
    s_modified = 1;
}

static void handle_enter(void) {
    if (s_sel_active) sel_delete_range();
    doc_split_line();
}

static void handle_backspace(void) {
    if (s_sel_active) { sel_delete_range(); return; }
    if (s_cx > 0) {
        s_cx--;
        line_delete_char(&s_lines[s_cy], s_cx);
        s_modified = 1;
    } else if (s_cy > 0) {
        /* Join with line above */
        s_cy--;
        s_cx = (int)s_lines[s_cy].len;
        doc_join_lines(s_cy);
    }
}

static void handle_delete(void) {
    if (s_sel_active) { sel_delete_range(); return; }
    if (s_cx < (int)s_lines[s_cy].len) {
        line_delete_char(&s_lines[s_cy], s_cx);
        s_modified = 1;
    } else if (s_cy < s_line_count - 1) {
        /* Join with line below */
        doc_join_lines(s_cy);
    }
}

static void handle_tab(void) {
    for (int i = 0; i < 4; i++)
        handle_char(' ');
}

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

/* Returns: 1 = exit, 0 = stay */
static int handle_exit(void) {
    if (!s_modified)
        return 1;

    /* Prompt for unsaved changes */
    draw_status(" F2:Save  F10:Discard  ESC:Cancel");
    draw_info("Unsaved changes!");

    for (;;) {
        struct key_event ev;
        kbd_wait(&ev);

        if (ev.code == KEY_F2) {
            if (handle_save() == 0)
                return 1;
            /* Save failed — restore status and stay */
            draw_status(NULL);
            return 0;
        } else if (ev.code == KEY_F10) {
            return 1;  /* discard */
        } else if (ev.code == KEY_ESC) {
            /* Cancel — restore normal status */
            draw_status(NULL);
            draw_info(NULL);
            return 0;
        }
    }
}

/* ---- Compile and Run (F5) ---- */

static int is_c_file(void) {
    int len = (int)str_len((CHAR8 *)s_filename);
    return len >= 2 && s_filename[len - 2] == '.' && s_filename[len - 1] == 'c';
}

static void handle_compile_run(void) {
    if (!is_c_file()) {
        draw_info("Not a .c file");
        return;
    }

    /* Auto-save before compiling */
    if (s_modified) {
        if (doc_save() != 0) {
            draw_info("Save failed — cannot compile");
            return;
        }
    }

    /* Serialize document to source string */
    UINTN src_size = 0;
    char *source = doc_serialize(&src_size);
    if (!source) {
        draw_info("Out of memory");
        return;
    }

    /* Clear screen and show compile message */
    fb_clear(COLOR_BLACK);
    fb_print("  Compiling ", COLOR_CYAN);
    fb_print(s_filename, COLOR_CYAN);
    fb_print("...\n\n", COLOR_CYAN);

    /* Compile and run */
    struct tcc_result r = tcc_run_source(source, s_filename);
    mem_free(source);

    /* Display result */
    fb_print("\n", COLOR_WHITE);
    if (r.success) {
        fb_print("  --- Program exited with code ", COLOR_GRAY);
        char num[16];
        int_to_str(r.exit_code, num);
        fb_print(num, r.exit_code == 0 ? COLOR_GREEN : COLOR_YELLOW);
        fb_print(" ---\n", COLOR_GRAY);
    } else {
        fb_print("  --- Compile Error ---\n", COLOR_RED);
        if (r.error_msg[0])
            fb_print(r.error_msg, COLOR_RED);
    }

    fb_print("\n  Press any key to return to editor...\n", COLOR_DGRAY);

    /* Wait for keypress */
    struct key_event ev;
    kbd_wait(&ev);

    /* Redraw editor */
    draw_all();
}

/* ---- Rebuild Workstation (F6) ---- */

static char s_rebuild_err[4096];
static int  s_rebuild_err_pos;

static void rebuild_error_handler(void *opaque, const char *msg) {
    (void)opaque;
    int len = (int)strlen(msg);
    for (int i = 0; i < len && s_rebuild_err_pos < (int)sizeof(s_rebuild_err) - 2; i++)
        s_rebuild_err[s_rebuild_err_pos++] = msg[i];
    if (s_rebuild_err_pos < (int)sizeof(s_rebuild_err) - 1)
        s_rebuild_err[s_rebuild_err_pos++] = '\n';
    s_rebuild_err[s_rebuild_err_pos] = '\0';
    /* Also print to screen as we go */
    fb_print("  ", COLOR_RED);
    fb_print(msg, COLOR_RED);
    fb_print("\n", COLOR_RED);
}

static void handle_rebuild(void) {
    /* Auto-save if modified */
    if (s_modified) {
        if (doc_save() != 0) {
            draw_info("Save failed - cannot rebuild");
            return;
        }
    }

    fb_clear(COLOR_BLACK);
    g_boot.cursor_x = 0;
    g_boot.cursor_y = 0;

    fb_print("\n", COLOR_CYAN);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("       REBUILD WORKSTATION\n", COLOR_CYAN);
    fb_print("  ========================================\n\n", COLOR_CYAN);

    s_rebuild_err_pos = 0;
    s_rebuild_err[0] = '\0';

    /* Source files to compile */
    static const char *sources[] = {
        "/src/main.c", "/src/fb.c", "/src/kbd.c", "/src/mem.c",
        "/src/font.c", "/src/fs.c", "/src/browse.c", "/src/edit.c",
        "/src/shim.c", "/src/tcc.c",
        "/src/disk.c", "/src/fat32.c", "/src/iso.c",
        "/src/exfat.c", "/src/ntfs.c",
        NULL
    };

    /* Output path */
#ifdef __aarch64__
    const char *out_path = "/EFI/BOOT/BOOTAA64.EFI";
#else
    const char *out_path = "/EFI/BOOT/BOOTX64.EFI";
#endif

    /* Create TCC state for PE/COFF output */
    TCCState *tcc = tcc_new();
    if (!tcc) {
        fb_print("  Failed to create TCC context\n", COLOR_RED);
        goto wait;
    }

    tcc_set_error_func(tcc, NULL, rebuild_error_handler);
    tcc_set_options(tcc, "-nostdlib -nostdinc -Werror"
                        " -Wl,-subsystem=efiapp -Wl,-e=efi_main");
    tcc_set_output_type(tcc, TCC_OUTPUT_DLL);

    /* Include paths: our stub headers, then source dir */
    tcc_add_include_path(tcc, "/src/tcc-headers");
    tcc_add_include_path(tcc, "/src");
    tcc_add_include_path(tcc, "/tools/tinycc");

    /* Compile each workstation source file */
    for (int i = 0; sources[i]; i++) {
        fb_print("  Compiling ", COLOR_WHITE);
        fb_print(sources[i], COLOR_YELLOW);
        fb_print("...\n", COLOR_WHITE);

        if (tcc_add_file(tcc, sources[i]) < 0) {
            fb_print("\n  BUILD FAILED\n", COLOR_RED);
            tcc_delete(tcc);
            goto wait;
        }
    }

    /* Compile TCC library (unity build) — suppress warnings for TCC source */
    tcc_set_options(tcc, "-w");
    tcc_define_symbol(tcc, "__UEFI__", "1");

    fb_print("  Compiling ", COLOR_WHITE);
    fb_print("/tools/tinycc/libtcc.c", COLOR_YELLOW);
    fb_print("...\n", COLOR_WHITE);
    if (tcc_add_file(tcc, "/tools/tinycc/libtcc.c") < 0) {
        fb_print("\n  BUILD FAILED (TCC library)\n", COLOR_RED);
        tcc_delete(tcc);
        goto wait;
    }

    /* Compile setjmp/longjmp */
#ifdef __aarch64__
    fb_print("  Compiling /src/setjmp_aarch64.c...\n", COLOR_WHITE);
    if (tcc_add_file(tcc, "/src/setjmp_aarch64.c") < 0) {
#else
    fb_print("  Compiling /src/setjmp_x86_64.S...\n", COLOR_WHITE);
    if (tcc_add_file(tcc, "/src/setjmp_x86_64.S") < 0) {
#endif
        fb_print("\n  BUILD FAILED (setjmp)\n", COLOR_RED);
        tcc_delete(tcc);
        goto wait;
    }

    fb_print("\n  Linking...\n", COLOR_WHITE);

    /* Generate PE output */
    if (tcc_output_file(tcc, out_path) < 0) {
        fb_print("  Output failed\n", COLOR_RED);
        tcc_delete(tcc);
        goto wait;
    }

    tcc_delete(tcc);

    fb_print("\n", COLOR_GREEN);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("    BUILD COMPLETE!\n", COLOR_GREEN);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("\n  Written to: ", COLOR_WHITE);
    fb_print(out_path, COLOR_YELLOW);
    fb_print("\n\n  Press R to reboot now, any other key for editor.\n", COLOR_YELLOW);
    {
        struct key_event ev;
        kbd_wait(&ev);
        if (ev.code == 'R' || ev.code == 'r') {
            fb_print("\n  Rebooting...\n", COLOR_CYAN);
            g_boot.rs->ResetSystem(EfiResetCold, 0, 0, NULL);
        }
    }
    draw_all();
    return;

wait:
    /* Re-print error summary so it's visible at bottom of screen */
    if (s_rebuild_err_pos > 0) {
        fb_print("\n  ---- Error Summary ----\n", COLOR_RED);
        fb_print(s_rebuild_err, COLOR_RED);
    }
    fb_print("\n  Press any key to return to editor...\n", COLOR_DGRAY);
    {
        struct key_event ev;
        kbd_wait(&ev);
    }
    draw_all();
}

/* ---- Public interface ---- */

void edit_run(const CHAR16 *path, const char *filename) {
    /* Build full filepath */
    int i = 0;
    while (path[i] && i < EDIT_MAX_PATH - 1) {
        s_filepath[i] = path[i];
        i++;
    }
    /* Add separator if needed */
    if (i > 1 || s_filepath[0] != L'\\')
        s_filepath[i++] = L'\\';
    int j = 0;
    while (filename[j] && i < EDIT_MAX_PATH - 1) {
        s_filepath[i++] = (CHAR16)filename[j++];
    }
    s_filepath[i] = 0;

    str_copy(s_filename, filename, sizeof(s_filename));

    /* Setup screen geometry */
    s_text_top = 1;
    s_text_rows = (int)g_boot.rows - 3;  /* header + info + status */
    s_text_cols = (int)g_boot.cols;

    /* Init cursor */
    s_cx = 0;
    s_cy = 0;
    s_scroll_x = 0;
    s_scroll_y = 0;
    s_modified = 0;
    s_sel_active = 0;
    s_highlight_mode = is_c_or_h_file();

    /* Load file */
    doc_load();

    /* Initial draw */
    draw_all();

    /* Main loop */
    struct key_event ev;
    int prev_scroll_y = s_scroll_y;
    int prev_scroll_x = s_scroll_x;
    for (;;) {
        kbd_wait(&ev);

        int s_cy_before = s_cy;
        int cursor_only = 0;

        /* Remap CUA shortcuts to Ctrl equivalents */
        UINT16 code = ev.code;
        if (code == KEY_INS && (ev.modifiers & KMOD_SHIFT))
            code = 0x16;  /* Shift+Insert → Ctrl+V (paste) */
        else if (code == KEY_INS && (ev.modifiers & KMOD_CTRL))
            code = 0x03;  /* Ctrl+Insert → Ctrl+C (copy) */
        else if (code == KEY_DEL && (ev.modifiers & KMOD_SHIFT))
            code = 0x18;  /* Shift+Delete → Ctrl+X (cut) */

        switch (code) {
        case KEY_F2:
            if (fs_is_read_only()) {
                draw_info("Volume is read-only");
            } else {
                handle_save();
            }
            break;

        case KEY_F5:
            handle_compile_run();
            break;

        case KEY_F6:
            handle_rebuild();
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

        case 0x03: /* Ctrl+C — copy */
            if (s_sel_active) {
                sel_copy();
                s_sel_active = 0;
            } else {
                handle_copy_line();
            }
            draw_info("Copied.");
            break;

        case 0x18: /* Ctrl+X — cut */
            if (s_sel_active) {
                sel_copy();
                sel_delete_range();
            } else {
                handle_cut_line();
            }
            break;

        case 0x16: /* Ctrl+V — paste */
            sel_paste();
            break;

        case 0x0B: /* Ctrl+K — cut line */
            handle_cut_line();
            break;

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

        int prev_cy = s_cy_before;
        scroll_to_cursor();

        if (cursor_only && !s_sel_active &&
            s_scroll_y == prev_scroll_y && s_scroll_x == prev_scroll_x) {
            /* Incremental: only redraw old and new cursor lines */
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
