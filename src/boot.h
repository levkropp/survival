#ifndef BOOT_H
#define BOOT_H

#include <efi.h>
#include <efilib.h>

/* Global system state — initialized in main.c */
struct boot_state {
    EFI_HANDLE image_handle;
    EFI_SYSTEM_TABLE *st;
    EFI_BOOT_SERVICES *bs;
    EFI_RUNTIME_SERVICES *rs;

    /* Graphics */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    UINT32 *framebuffer;
    UINT32 fb_width;
    UINT32 fb_height;
    UINT32 fb_pitch; /* pixels per scan line */
    UINTN fb_size;

    /* Text cursor state */
    UINT32 cursor_x; /* column in characters */
    UINT32 cursor_y; /* row in characters */
    UINT32 cols;     /* max columns */
    UINT32 rows;     /* max rows */
    UINT32 scale;    /* font scale factor (1 or 2) */
};

extern struct boot_state g_boot;

/* Color constants (BGRX 32-bit) */
#define COLOR_BLACK   0x00000000
#define COLOR_WHITE   0x00FFFFFF
#define COLOR_GREEN   0x0000FF00
#define COLOR_RED     0x00FF0000
#define COLOR_BLUE    0x000000FF
#define COLOR_YELLOW  0x0000FFFF
#define COLOR_CYAN    0x00FFFF00
#define COLOR_GRAY    0x00808080
#define COLOR_DGRAY   0x00404040
#define COLOR_ORANGE  0x00FFA500
#define COLOR_MAGENTA 0x00FF00FF

#endif /* BOOT_H */
