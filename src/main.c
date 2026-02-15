#include "boot.h"
#include "fb.h"
#include "kbd.h"
#include "mem.h"
#include "fs.h"
#include "browse.h"
#include "tcc.h"

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

/* Print ASCII string to UEFI console (converts to CHAR16) */
static void con_print_ascii(const char *s) {
    while (*s) {
        if (*s == '\n') {
            con_print(L"\r\n");
        } else {
            CHAR16 ch[2] = { (CHAR16)(unsigned char)*s, 0 };
            con_print(ch);
        }
        s++;
    }
}

/* Get total system memory in MB via UEFI memory map */
static UINT32 get_total_memory_mb(void) {
    UINTN map_size = 0, map_key, desc_size;
    UINT32 desc_ver;

    g_boot.bs->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
    map_size += 2 * desc_size;

    EFI_MEMORY_DESCRIPTOR *map = (EFI_MEMORY_DESCRIPTOR *)mem_alloc(map_size);
    if (!map) return 0;

    EFI_STATUS status = g_boot.bs->GetMemoryMap(
        &map_size, map, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        mem_free(map);
        return 0;
    }

    UINT64 total_pages = 0;
    UINT8 *ptr = (UINT8 *)map;
    UINT8 *end = ptr + map_size;
    while (ptr < end) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)ptr;
        UINT32 t = desc->Type;
        if ((t >= EfiLoaderCode && t <= EfiConventionalMemory) ||
            t == EfiACPIReclaimMemory)
            total_pages += desc->NumberOfPages;
        ptr += desc_size;
    }

    mem_free(map);
    return (UINT32)((total_pages * 4096) / (1024 * 1024));
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

#ifdef __aarch64__
    fb_print("  Platform: ARM64\n", COLOR_WHITE);
#elif defined(__x86_64__)
    fb_print("  Platform: x86_64\n", COLOR_WHITE);
#endif

    /* Display resolution */
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
    fb_print("  Display:  ", COLOR_GRAY);
    fb_print(res, COLOR_GRAY);
    fb_print("\n", COLOR_WHITE);

    /* Disk info */
    UINT64 total_bytes, free_bytes;
    if (fs_volume_info(&total_bytes, &free_bytes) == 0) {
        UINT32 total_mb = (UINT32)(total_bytes / (1024 * 1024));
        UINT32 free_mb = (UINT32)(free_bytes / (1024 * 1024));
        fb_print("  Disk:     ", COLOR_GRAY);
        uint_to_str(free_mb, num); fb_print(num, COLOR_GRAY);
        fb_print(" MB free / ", COLOR_GRAY);
        uint_to_str(total_mb, num); fb_print(num, COLOR_GRAY);
        fb_print(" MB total\n", COLOR_GRAY);
    }

    /* Memory info */
    UINT32 mem_mb = get_total_memory_mb();
    if (mem_mb > 0) {
        fb_print("  Memory:   ", COLOR_GRAY);
        uint_to_str(mem_mb, num); fb_print(num, COLOR_GRAY);
        fb_print(" MB\n", COLOR_GRAY);
    }

    fb_print("\n", COLOR_WHITE);
    fb_print("  Press any key to enter file browser.\n", COLOR_YELLOW);
    fb_print("  ----------------------------------------\n", COLOR_DGRAY);
}

static void print_banner_console(void) {
    char num[16];

    con_print(L"\r\n");
    con_print(L"  ========================================\r\n");
    con_print(L"       SURVIVAL WORKSTATION v0.1\r\n");
    con_print(L"  ========================================\r\n");
    con_print(L"\r\n");

#ifdef __aarch64__
    con_print(L"  Platform: ARM64\r\n");
#elif defined(__x86_64__)
    con_print(L"  Platform: x86_64\r\n");
#endif
    con_print(L"  Mode:     Console (no framebuffer)\r\n");

    /* Disk info */
    UINT64 total_bytes, free_bytes;
    if (fs_volume_info(&total_bytes, &free_bytes) == 0) {
        UINT32 total_mb = (UINT32)(total_bytes / (1024 * 1024));
        UINT32 free_mb = (UINT32)(free_bytes / (1024 * 1024));
        con_print_ascii("  Disk:     ");
        uint_to_str(free_mb, num); con_print_ascii(num);
        con_print_ascii(" MB free / ");
        uint_to_str(total_mb, num); con_print_ascii(num);
        con_print_ascii(" MB total\n");
    }

    /* Memory info */
    UINT32 mem_mb = get_total_memory_mb();
    if (mem_mb > 0) {
        con_print_ascii("  Memory:   ");
        uint_to_str(mem_mb, num); con_print_ascii(num);
        con_print_ascii(" MB\n");
    }

    con_print(L"\r\n");
    con_print(L"  Type anything. Press ESC to shutdown.\r\n");
    con_print(L"  ----------------------------------------\r\n");
    con_print(L"\r\n> ");
}

static void tcc_selftest(int use_fb); /* forward declaration */

/* Console-only main loop (no framebuffer) */
static void console_loop(void) {
    print_banner_console();
    tcc_selftest(0);

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

/* Compiler self-test: compile "return 42" and verify.
   use_fb=1 prints to framebuffer, use_fb=0 prints to console. */
static void tcc_selftest(int use_fb) {
    if (use_fb)
        fb_print("  TCC self-test: ", COLOR_GRAY);
    else
        con_print(L"  TCC self-test: ");

    struct tcc_result r = tcc_run_source(
        "int main(void) { return 42; }\n", "selftest.c");

    if (r.success && r.exit_code == 42) {
        if (use_fb)
            fb_print("PASS (return 42)\n", COLOR_GREEN);
        else
            con_print(L"PASS (return 42)\r\n");
    } else if (r.success) {
        char buf[64];
        uint_to_str((UINT32)(r.exit_code < 0 ? -r.exit_code : r.exit_code), buf);
        if (use_fb) {
            fb_print("FAIL (got ", COLOR_RED);
            if (r.exit_code < 0) fb_print("-", COLOR_RED);
            fb_print(buf, COLOR_RED);
            fb_print(")\n", COLOR_RED);
        } else {
            con_print(L"FAIL (got ");
            if (r.exit_code < 0) con_print_ascii("-");
            con_print_ascii(buf);
            con_print(L")\r\n");
        }
    } else {
        if (use_fb) {
            fb_print("FAIL: ", COLOR_RED);
            fb_print(r.error_msg, COLOR_RED);
            fb_print("\n", COLOR_RED);
        } else {
            con_print(L"FAIL: ");
            con_print_ascii(r.error_msg);
            con_print(L"\r\n");
        }
    }
}

/* Framebuffer main loop */
static void fb_loop(void) {
    print_banner_fb();

    /* Run compiler self-test */
    tcc_selftest(1);

    struct key_event ev;
    kbd_wait(&ev);

    browse_run();
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
    fs_init();

    status = fb_init();
    if (!EFI_ERROR(status)) {
        have_fb = 1;
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
