# Chapter 8: The Main Loop

## Wiring It Together

We have four subsystems built across four chapters:

- **Memory** (Chapter 5) — allocates and frees memory through UEFI
- **Framebuffer** (Chapter 6) — draws pixels, characters, and strings
- **Font** (Chapter 6) — 8x16 bitmap font data for text rendering
- **Keyboard** (Chapter 7) — reads and normalizes keystrokes

Each module works in isolation, but `main.c` still runs the Chapter 4 console loop. In this chapter, we connect everything into a working application — one that boots, draws a colored banner on the framebuffer, echoes keystrokes, and shuts down cleanly.

## The Problem of No Framebuffer

Not every system provides a usable framebuffer. UEFI's GOP protocol can exist even when there's no linear framebuffer — `LocateProtocol` succeeds, but `FrameBufferBase` is NULL. Our `fb_init()` handles this by returning an error.

We need two main loops:

```
fb_init() succeeds?
    ├── Yes → fb_loop()      Framebuffer mode: draw pixels
    └── No  → console_loop() Console mode: UEFI text output
```

The console loop is our Chapter 4 code, largely unchanged. It always works because UEFI's text console goes to both the display and the serial port. The framebuffer loop is the new, richer experience.

## Updating main.c

Let's rebuild `main.c` from the top. First, the includes and global state:

```c
#include "boot.h"
#include "fb.h"
#include "kbd.h"
#include "mem.h"

struct boot_state g_boot;
```

We now include all four module headers. Every module depends on `boot.h` (through their own headers), but `main.c` needs them all directly because it calls functions from each.

## A Number-to-String Helper

We want to display the screen resolution in our banner, but we don't have `sprintf`. We need to convert integers to strings manually:

```c
static void uint_to_str(UINT32 n, char *buf) {
    char tmp[16];
    int t = 0;
    if (n == 0) { tmp[t++] = '0'; }
    else { while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; } }
    int i = 0;
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = '\0';
}
```

The algorithm extracts digits in reverse (`n % 10` gives the last digit, `n / 10` removes it), stores them in a temporary buffer, then copies them out in the right order. For `1024`, the first phase produces `4, 2, 0, 1`; the second phase reverses to `1, 0, 2, 4`.

## A Console Print Helper for ASCII

Our `con_print` function takes CHAR16 wide strings, which is what UEFI's `OutputString` expects. But when we build strings dynamically — like resolution numbers — we work in plain ASCII `char` buffers. Converting each one to a `L"..."` wide literal is not possible at runtime.

We add a small helper that converts ASCII to CHAR16 one character at a time:

```c
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
```

This lets the console banner use the same `uint_to_str` buffers as the framebuffer banner, without duplicating number formatting logic into wide-string equivalents. The `\n` to `\r\n` translation handles the fact that UEFI's console requires carriage returns.

## Querying System Memory

We want the banner to show how much memory the system has. UEFI provides this through its memory map — the same map an OS would use to understand available RAM. We query it, sum the usable regions, and convert to megabytes:

```c
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
```

The two-call pattern is standard UEFI: first call with a NULL buffer to learn the required size, then allocate and call again. We add `2 * desc_size` as padding because the allocation itself can change the memory map.

The walk through the descriptor array uses `desc_size` rather than `sizeof(EFI_MEMORY_DESCRIPTOR)` — UEFI firmware may return descriptors larger than the struct definition, so we must step by the reported size.

We count pages from memory types that represent usable RAM: `EfiLoaderCode` through `EfiConventionalMemory` (the loader and boot services regions), plus `EfiACPIReclaimMemory` (reclaimable after ACPI tables are read). Each page is 4096 bytes.

## The Framebuffer Banner

When the framebuffer is available, we show a splash screen:

```c
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
```

Multiple colors on screen for the first time. The title is green, platform info is white. Each `fb_print` call advances the cursor, so the lines stack naturally.

The original banner hard-coded "Target: Libre Computer Sweet Potato V2" and "SoC: Amlogic S905X" — details specific to one ARM64 board. Since our workstation builds for both ARM64 and x86_64, we use preprocessor conditionals to show the correct platform. The compiler defines `__aarch64__` or `__x86_64__` automatically based on the target, so this costs nothing at runtime.

```c
    /* Build resolution string: "800x600 (100x37 chars)" */
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
```

The resolution string is built character by character — awkward without `sprintf`, but correct.

```c
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
```

After the display resolution, we show total system memory. Later, when we add filesystem support (Chapter 9), the banner will also show disk space — "Disk: X MB free / Y MB total" — using the `fs_volume_info()` function. And in Phase 3, a TCC compiler self-test line will appear here too. For now, platform, display, and memory give us a useful system overview.

The yellow "Press any key" line and gray separator complete the banner.

## The Framebuffer Loop

```c
static void fb_loop(void) {
    print_banner_fb();

    struct key_event ev;
    kbd_wait(&ev);
}
```

The framebuffer loop shows the banner and waits for one keypress. In Phase 2 (Chapter 10), this will launch the file browser. But for Phase 1, we're establishing the pattern: show a banner, then hand control to an interactive component.

Notice there is no second "Press any key" message here. An earlier version printed the prompt both inside `print_banner_fb()` and again in `fb_loop()`, producing a duplicate line on screen. The banner owns the prompt — the loop just waits.

## The Console Banner and Fallback

The console banner mirrors the framebuffer banner but uses `con_print` and `con_print_ascii` for UEFI text output:

```c
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
```

The same `#ifdef` platform detection appears here. Where the framebuffer banner shows a resolution string, the console banner shows "Mode: Console (no framebuffer)" — the console has no resolution to report.

```c
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
```

The memory line uses `con_print_ascii` to print the dynamically formatted number. As with the framebuffer banner, disk info will be added here once the filesystem module exists in Chapter 9.

The console loop itself stays close to Chapter 4, using UEFI's text console directly:

```c
static void console_loop(void) {
    print_banner_console();

    EFI_INPUT_KEY key;
    UINTN index;

    for (;;) {
        g_boot.bs->WaitForEvent(1, &g_boot.st->ConIn->WaitForKey, &index);
        EFI_STATUS status = g_boot.st->ConIn->ReadKeyStroke(g_boot.st->ConIn, &key);
        if (EFI_ERROR(status))
            continue;

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
```

Notice it uses UEFI's keyboard directly instead of our `kbd` module. This is intentional — the console loop is a minimal fallback. It doesn't need key code normalization because it checks `UnicodeChar` and `ScanCode` directly.

## The Entry Point

Let's look at the complete `efi_main`, which orchestrates everything:

```c
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
```

The initialization order matters:

1. **Global state first.** Everything else needs `g_boot`.
2. **Watchdog off.** Before anything that might take time.
3. **Console message.** So we see boot progress via serial even if framebuffer fails.
4. **Memory.** Other subsystems might need to allocate.
5. **Framebuffer.** Try to set up graphics; note whether it worked.
6. **Keyboard reset.** Clear any buffered keystrokes from the boot process.

One subtle thing to notice: when `fb_init()` succeeds, we do *not* print "Framebuffer initialized" to the console. An earlier version did this, and the text leaked onto the graphical display. UEFI's text console and the framebuffer share the same physical screen — writing to ConOut after initializing GOP smears text console output over whatever the framebuffer is drawing. The fix is simple: once we have a framebuffer, only talk to it through `fb_print`. The console is for the fallback path and for serial output during early boot, before `fb_init` is called.

After the main loop exits, we print a shutdown message via the console (which always works), wait one second, and power off.

## The Build System

The Makefile orchestrates a three-step build:

```
Source files  ──compile──→  Object files  ──link──→  ELF binary  ──convert──→  PE binary
(src/*.c)                   (build/*.o)              (survival.so)             (survival.efi)
```

Key elements:

```makefile
SOURCES  := $(SRCDIR)/main.c $(SRCDIR)/fb.c $(SRCDIR)/kbd.c \
            $(SRCDIR)/mem.c $(SRCDIR)/font.c
```

Each source file is compiled separately. Make only recompiles files that changed — edit `kbd.c` and only `kbd.o` is rebuilt.

```makefile
$(SO): $(OBJECTS)
	$(LD) $(LDFLAGS) -L$(EFI_LIB) $(EFI_CRT) $(OBJECTS) -o $@ \
	    -lefi -lgnuefi $(LIBGCC)
```

Linking order matters. The CRT startup code (`crt0-efi-aarch64.o`) comes first. Then our objects. Then the libraries (`-lefi -lgnuefi`) and `libgcc.a`. If you reorder these, you get cryptic linker errors.

```makefile
$(TARGET): $(SO)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	    -j .dynsym -j .rel -j .rela -j .reloc \
	    --target=efi-app-aarch64 $< $@
```

The final conversion from ELF to PE/COFF. The `-j` flags specify which sections to include. Missing `-j .rodata` would silently produce a binary that crashes — all string literals and the font data live in `.rodata`.

```makefile
esp: $(TARGET)
	@mkdir -p $(ESP_DIR)
	cp $(TARGET) $(ESP_DIR)/BOOTAA64.EFI
```

Copy the binary to the UEFI boot path. `BOOTAA64.EFI` is the standard name UEFI looks for on ARM64.

## Testing with QEMU

The `scripts/run-qemu.sh` script creates a complete virtual ARM64 machine. Here's what it does:

1. **Finds UEFI firmware** — tries common paths for `QEMU_EFI.fd` across distros
2. **Prepares firmware images** — pads to 64 MB for QEMU's pflash
3. **Creates a FAT32 disk image** — `mkfs.vfat`, then copies our binary with `mtools`
4. **Launches QEMU** with the right flags:

```bash
qemu-system-aarch64 \
    -M virt -cpu cortex-a53 -m 256M \
    -drive if=pflash,format=raw,file="$FW_COPY",readonly=on \
    -drive if=pflash,format=raw,file="$VARS" \
    -hda "$IMG" \
    -device ramfb \
    -device qemu-xhci -device usb-kbd -device usb-mouse \
    -serial stdio
```

The `-device ramfb` flag is critical — it provides a real, memory-mapped linear framebuffer that our `fb_init()` can use. Without it (or with `virtio-gpu-pci`), `FrameBufferBase` is NULL and we fall back to console mode.

The script supports three modes:
- **`graphical`** — GTK window showing the framebuffer
- **`console`** — no display, serial output only
- **`vnc`** — framebuffer via VNC on port 5900

Run it:

```bash
make && ./scripts/run-qemu.sh graphical
```

You should see the green banner, platform and resolution info, memory size, and a prompt waiting for input.

## What We Built

Phase 1 is complete. Our binary:

- Boots on an ARM64 machine via UEFI
- Initializes a framebuffer and renders text with an 8x16 bitmap font
- Shows platform, display, and memory information at startup
- Falls back to console mode when no framebuffer is available
- Accepts keyboard input
- Shuts down cleanly
- Is 64 KB total — smaller than many JPEG images

The codebase:

```
src/boot.h    — 42 lines   Global state, color constants
src/main.c    — 161 lines  Entry point, two main loops, banners
src/fb.c      — 127 lines  Framebuffer driver
src/fb.h      — 25 lines   Framebuffer API
src/font.c    — 280 lines  Bitmap font data (mostly data, not logic)
src/font.h    — 16 lines   Font constants
src/kbd.c     — 53 lines   Keyboard input
src/kbd.h     — 41 lines   Key codes
src/mem.c     — 35 lines   Memory allocator
src/mem.h     — 19 lines   Memory API
```

About 800 lines total, of which 280 are font bitmap data. The actual logic is roughly 500 lines of C.

## What Comes Next

Phase 1 proves that our approach works. We have a foundation — screen, keyboard, memory — that everything else builds on.

Phase 2 will add FAT32 filesystem access using UEFI's Simple File System Protocol. This lets us read files from the SD card — opening the door to loading survival documentation, source code, and eventually, compiling and running programs right on the device. Once the filesystem is in place, the banner will grow a "Disk" line showing free and total space.
