#include "fb.h"
#include "font.h"
#include "mem.h"

EFI_STATUS fb_init(void) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status;

    status = g_boot.bs->LocateProtocol(&gop_guid, NULL, (void **)&g_boot.gop);
    if (EFI_ERROR(status))
        return status;

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = g_boot.gop;

    /* GOP method casts (protocol stores them as void *) */
    typedef EFI_STATUS (*GOP_QUERY)(EFI_GRAPHICS_OUTPUT_PROTOCOL *,
                                    UINT32, UINTN *,
                                    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    typedef EFI_STATUS (*GOP_SET)(EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32);
    GOP_QUERY query_mode = (GOP_QUERY)gop->QueryMode;
    GOP_SET   set_mode   = (GOP_SET)gop->SetMode;

    /* Hard-limit resolution to 800x600 to keep framebuffer draws fast */
    UINT32 max_w = 800;
    UINT32 max_h = 600;

    UINT32 best_mode = gop->Mode->Mode;
    UINT32 best_pixels = 0;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN info_size;
        if (EFI_ERROR(query_mode(gop, m, &info_size, &info)))
            continue;
        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;
        if (w <= max_w && h <= max_h && w * h > best_pixels) {
            best_pixels = w * h;
            best_mode = m;
        }
    }

    if (best_mode != gop->Mode->Mode)
        set_mode(gop, best_mode);

    g_boot.framebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    g_boot.fb_width = gop->Mode->Info->HorizontalResolution;
    g_boot.fb_height = gop->Mode->Info->VerticalResolution;
    g_boot.fb_pitch = gop->Mode->Info->PixelsPerScanLine;
    g_boot.fb_size = gop->Mode->FrameBufferSize;

    if (g_boot.framebuffer == NULL || g_boot.fb_size == 0)
        return EFI_UNSUPPORTED;

    g_boot.scale = 1;

    g_boot.cols = g_boot.fb_width / (FONT_WIDTH * g_boot.scale);
    g_boot.rows = g_boot.fb_height / (FONT_HEIGHT * g_boot.scale);
    g_boot.cursor_x = 0;
    g_boot.cursor_y = 0;

    fb_clear(COLOR_BLACK);
    return EFI_SUCCESS;
}

void fb_pixel(UINT32 x, UINT32 y, UINT32 color) {
    if (x < g_boot.fb_width && y < g_boot.fb_height)
        g_boot.framebuffer[y * g_boot.fb_pitch + x] = color;
}

void fb_rect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color) {
    for (UINT32 row = y; row < y + h && row < g_boot.fb_height; row++) {
        UINT32 *line = &g_boot.framebuffer[row * g_boot.fb_pitch + x];
        for (UINT32 col = 0; col < w && (x + col) < g_boot.fb_width; col++)
            line[col] = color;
    }
}

void fb_clear(UINT32 color) {
    for (UINT32 y = 0; y < g_boot.fb_height; y++) {
        UINT32 *line = &g_boot.framebuffer[y * g_boot.fb_pitch];
        for (UINT32 x = 0; x < g_boot.fb_width; x++)
            line[x] = color;
    }
}

void fb_char(UINT32 cx, UINT32 cy, char c, UINT32 fg, UINT32 bg) {
    if (c < FONT_FIRST || c > FONT_LAST)
        c = '?';

    UINT32 scale = g_boot.scale;
    UINT32 cw = FONT_WIDTH * scale;
    UINT32 ch = FONT_HEIGHT * scale;
    UINT32 px = cx * cw;
    UINT32 py = cy * ch;

    if (px + cw > g_boot.fb_width || py + ch > g_boot.fb_height)
        return;

    const UINT8 *glyph = font_data[c - FONT_FIRST];
    UINT32 pitch = g_boot.fb_pitch;
    UINT32 *base = g_boot.framebuffer + py * pitch + px;

    if (scale == 1) {
        for (UINT32 row = 0; row < FONT_HEIGHT; row++) {
            UINT8 bits = glyph[row];
            UINT32 *dst = base;
            for (UINT32 col = 0; col < FONT_WIDTH; col++)
                *dst++ = (bits & (0x80 >> col)) ? fg : bg;
            base += pitch;
        }
    } else {
        /* 2x: write 2 pixels wide, 2 rows at once per font row */
        for (UINT32 row = 0; row < FONT_HEIGHT; row++) {
            UINT8 bits = glyph[row];
            UINT32 *dst = base;
            for (UINT32 col = 0; col < FONT_WIDTH; col++) {
                UINT32 color = (bits & (0x80 >> col)) ? fg : bg;
                dst[0] = color;
                dst[1] = color;
                dst[pitch] = color;
                dst[pitch + 1] = color;
                dst += 2;
            }
            base += pitch * 2;
        }
    }
}

void fb_string(UINT32 cx, UINT32 cy, const char *s, UINT32 fg, UINT32 bg) {
    while (*s) {
        if (cx >= g_boot.cols) {
            cx = 0;
            cy++;
        }
        if (cy >= g_boot.rows)
            return;
        fb_char(cx, cy, *s, fg, bg);
        cx++;
        s++;
    }
}

void fb_scroll(void) {
    UINT32 scroll_rows = FONT_HEIGHT * g_boot.scale;

    for (UINT32 y = 0; y < g_boot.fb_height - scroll_rows; y++) {
        mem_copy(&g_boot.framebuffer[y * g_boot.fb_pitch],
                 &g_boot.framebuffer[(y + scroll_rows) * g_boot.fb_pitch],
                 g_boot.fb_width * sizeof(UINT32));
    }

    for (UINT32 y = g_boot.fb_height - scroll_rows; y < g_boot.fb_height; y++) {
        UINT32 *line = &g_boot.framebuffer[y * g_boot.fb_pitch];
        for (UINT32 x = 0; x < g_boot.fb_width; x++)
            line[x] = COLOR_BLACK;
    }
}

void fb_print(const char *s, UINT32 fg) {
    while (*s) {
        if (*s == '\n') {
            g_boot.cursor_x = 0;
            g_boot.cursor_y++;
        } else {
            if (g_boot.cursor_x >= g_boot.cols) {
                g_boot.cursor_x = 0;
                g_boot.cursor_y++;
            }
            if (g_boot.cursor_y >= g_boot.rows) {
                fb_scroll();
                g_boot.cursor_y = g_boot.rows - 1;
            }
            fb_char(g_boot.cursor_x, g_boot.cursor_y, *s, fg, COLOR_BLACK);
            g_boot.cursor_x++;
        }

        if (g_boot.cursor_y >= g_boot.rows) {
            fb_scroll();
            g_boot.cursor_y = g_boot.rows - 1;
        }

        s++;
    }
}
