#include "boot.h"
#include "fb.h"
#include "kbd.h"
#include "mem.h"

/* Global boot state */
struct boot_state g_boot;

/* Simple integer to decimal string */
static void uint_to_str(UINT32 n, char *buf) {
    char tmp[16];
    int t = 0;
    if (n == 0) { tmp[t++] = '0'; }
    else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; } }
    int i = 0;
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = '\0';
}

/* Print to UEFI text console (works over serial too) */
static void con_print(const CHAR16 *s) {
    g_boot.st->ConOut->OutputString(g_boot.st->ConOut, (CHAR16 *)s);
}

static void print_banner_fb(void) {
    char res[64];
    char num[16];
    int i = 0;

    fb_print("\n", COLOR_GREEN);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("       SURVIVAL WORKSTATION v0.1\n", COLOR_GREEN);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("\n", COLOR_GREEN);
    fb_print("  Bare-metal ARM64 development terminal\n", COLOR_WHITE);
    fb_print("  Target: Libre Computer Sweet Potato V2\n", COLOR_WHITE);
    fb_print("  SoC:    Amlogic S905X (Cortex-A53)\n", COLOR_WHITE);
    fb_print("\n", COLOR_WHITE);

    /* Build resolution string */
    uint_to_str(g_boot.fb_width, num);
    for (int j = 0; num[j]; j++) res[i++] = num[j];
    res[i++] = 'x';
    uint_to_str(g_boot.fb_height, num);
    for (int j = 0; num[j]; j++) res[i++] = num[j];
    res[i++] = ' ';
    res[i++] = '(';
    uint_to_str(g_boot.cols, num);
    for (int j = 0; num[j]; j++) res[i++] = num[j];
    res[i++] = 'x';
    uint_to_str(g_boot.rows, num);
    for (int j = 0; num[j]; j++) res[i++] = num[j];
    res[i++] = ' ';
    res[i++] = 'c'; res[i++] = 'h'; res[i++] = 'a';
    res[i++] = 'r'; res[i++] = 's'; res[i++] = ')';
    res[i] = '\0';

    fb_print("  Display: ", COLOR_GRAY);
    fb_print(res, COLOR_GRAY);
    fb_print("\n\n", COLOR_WHITE);
    fb_print("  Type anything. Press ESC to shutdown.\n", COLOR_YELLOW);
    fb_print("  ----------------------------------------\n", COLOR_DGRAY);
    fb_print("\n", COLOR_WHITE);
    fb_print("> ", COLOR_GREEN);
}

static void print_banner_console(void) {
    con_print(L"\r\n");
    con_print(L"  ========================================\r\n");
    con_print(L"       SURVIVAL WORKSTATION v0.1\r\n");
    con_print(L"  ========================================\r\n");
    con_print(L"\r\n");
    con_print(L"  Bare-metal ARM64 development terminal\r\n");
    con_print(L"  Target: Libre Computer Sweet Potato V2\r\n");
    con_print(L"  SoC:    Amlogic S905X (Cortex-A53)\r\n");
    con_print(L"  Mode:   Console (no framebuffer)\r\n");
    con_print(L"\r\n");
    con_print(L"  Type anything. Press ESC to shutdown.\r\n");
    con_print(L"  ----------------------------------------\r\n");
    con_print(L"\r\n> ");
}

/* Console-only main loop (no framebuffer) */
static void console_loop(void) {
    print_banner_console();

    EFI_INPUT_KEY key;
    UINTN index;

    for (;;) {
        g_boot.bs->WaitForEvent(1, &g_boot.st->ConIn->WaitForKey, &index);
        EFI_STATUS status = g_boot.st->ConIn->ReadKeyStroke(g_boot.st->ConIn, &key);
        if (EFI_ERROR(status))
            continue;

        /* ESC to exit */
        if (key.ScanCode == 0x17 || key.UnicodeChar == 0x1B)
            break;

        if (key.UnicodeChar == '\r') {
            con_print(L"\r\n> ");
        } else if (key.UnicodeChar >= 0x20 && key.UnicodeChar <= 0x7E) {
            CHAR16 ch[2] = { key.UnicodeChar, 0 };
            con_print(ch);
        }
    }
}

/* Framebuffer main loop */
static void fb_loop(void) {
    print_banner_fb();

    struct key_event ev;
    for (;;) {
        kbd_wait(&ev);

        if (ev.code == KEY_ESC)
            break;

        if (ev.code == KEY_ENTER || ev.code == '\r') {
            fb_print("\n> ", COLOR_GREEN);
        } else if (ev.code == KEY_BS) {
            if (g_boot.cursor_x > 2) {
                g_boot.cursor_x--;
                fb_char(g_boot.cursor_x, g_boot.cursor_y, ' ', COLOR_BLACK, COLOR_BLACK);
            }
        } else if (ev.code >= 0x20 && ev.code <= 0x7E) {
            char s[2] = {(char)ev.code, '\0'};
            fb_print(s, COLOR_WHITE);
        }
    }
}

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status;
    int have_fb = 0;

    /* Initialize global state */
    g_boot.image_handle = image_handle;
    g_boot.st = st;
    g_boot.bs = st->BootServices;
    g_boot.rs = st->RuntimeServices;

    /* Disable watchdog timer */
    g_boot.bs->SetWatchdogTimer(0, 0, 0, NULL);

    con_print(L"SURVIVAL WORKSTATION: Booting...\r\n");

    /* Initialize subsystems */
    mem_init();

    status = fb_init();
    if (!EFI_ERROR(status)) {
        have_fb = 1;
        con_print(L"Framebuffer initialized.\r\n");
    } else {
        con_print(L"No framebuffer, falling back to console.\r\n");
    }

    /* Reset keyboard input */
    st->ConIn->Reset(st->ConIn, FALSE);

    /* Run appropriate mode */
    if (have_fb)
        fb_loop();
    else
        console_loop();

    /* Shutdown */
    con_print(L"\r\nShutting down...\r\n");
    g_boot.bs->Stall(1000000);
    g_boot.rs->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);

    return EFI_SUCCESS;
}
