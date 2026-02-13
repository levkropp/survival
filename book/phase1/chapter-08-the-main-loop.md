# Chapter 8: The Main Loop

## Wiring It Together

We've built four subsystems:

- **Memory** (Chapter 5) — allocates and frees memory through UEFI
- **Framebuffer** (Chapter 6) — draws pixels and text on screen
- **Keyboard** (Chapter 7) — reads keystrokes from the user
- **Boot state** (Chapter 4) — holds the global UEFI handles everything shares

Now we connect them in `main.c` — the file that contains our entry point, initialization sequence, and main loop. We've already seen the entry point and console loop in Chapter 4. In this chapter, we'll focus on the framebuffer loop and the design decisions that shaped the whole program.

## Two Loops, One Program

Our application has two main loops:

```
fb_init() succeeds?
    ├── Yes → fb_loop()      Framebuffer mode: draw pixels
    └── No  → console_loop() Console mode: UEFI text output
```

This dual-mode design exists for practical reasons. Not every environment provides a linear framebuffer:

- **QEMU with `ramfb`** — real framebuffer, `fb_init()` succeeds
- **QEMU with `virtio-gpu-pci`** — GOP exists but framebuffer address is NULL, `fb_init()` fails
- **QEMU with `-display none`** — no display device at all
- **Real hardware** — depends on U-Boot configuration and display connection

Console mode gives us a fallback that always works (UEFI text output goes to both the display and the serial port). We can test application logic even when there's no screen attached.

## The Framebuffer Loop

Let's walk through `fb_loop()` line by line:

```c
static void fb_loop(void) {
    print_banner_fb();
```

We display the splash screen. `print_banner_fb()` uses `fb_print()` to draw colored text — the project name in green, hardware info in white, instructions in yellow, and a separator in dark gray. This gives immediate visual feedback that the system booted successfully.

The function also constructs a resolution string like "1024x768 (128x48 chars)" by converting integers to ASCII manually with `uint_to_str()`. We can't use `sprintf` — we don't have a C library.

```c
    struct key_event ev;
    for (;;) {
        kbd_wait(&ev);
```

The infinite loop. We block on `kbd_wait()`, which suspends the CPU until a key is pressed. Between keystrokes, our application uses zero CPU — the processor sits in a low-power state.

```c
        if (ev.code == KEY_ESC)
            break;
```

Escape exits the loop. When we break out, execution falls through to the shutdown sequence in `efi_main()`.

```c
        if (ev.code == KEY_ENTER || ev.code == '\r') {
            fb_print("\n> ", COLOR_GREEN);
```

Enter starts a new line and prints a green prompt. We check for both `KEY_ENTER` (our constant, `0x0D`) and `'\r'` (the ASCII carriage return character, also `0x0D`). They're actually the same value — the double check is redundant but reads clearly. It says "we expect Enter here" rather than just checking a magic number.

The `fb_print()` function handles the `\n` by advancing `cursor_y` and resetting `cursor_x` to 0. The `>` and space are then drawn as characters at the new cursor position.

```c
        } else if (ev.code == KEY_BS) {
            if (g_boot.cursor_x > 2) {
                g_boot.cursor_x--;
                fb_char(g_boot.cursor_x, g_boot.cursor_y, ' ', COLOR_BLACK, COLOR_BLACK);
            }
```

Backspace handling. We check `cursor_x > 2` to prevent the user from backspacing over the `> ` prompt (which occupies columns 0 and 1). If there's room, we move the cursor back one column and draw a space with a black foreground and background — effectively erasing the character.

This is a simple but imperfect implementation. It doesn't handle backspacing across line boundaries (if the user typed enough characters to wrap to the next line, backspace won't unwrap). For Phase 1, this is fine — we're building a keystroke echo demo, not a text editor.

```c
        } else if (ev.code >= 0x20 && ev.code <= 0x7E) {
            char s[2] = {(char)ev.code, '\0'};
            fb_print(s, COLOR_WHITE);
        }
    }
}
```

For printable ASCII characters (space `0x20` through tilde `0x7E`), we echo the character in white. We create a 2-byte string — the character plus a null terminator — because `fb_print` expects a null-terminated string, not a single character.

Characters outside the printable range (control characters, extended Unicode) are silently ignored. No crash, no garbage — just nothing happens.

## The Console Loop

The console loop (covered in Chapter 4) follows the same pattern but uses UEFI's text console instead of our framebuffer:

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
```

Notice that the console loop uses UEFI's keyboard functions directly instead of our `kbd` module. This is intentional — the console loop is a minimal fallback. It doesn't need the abstraction because it doesn't need the key code normalization. It checks `UnicodeChar` directly for printable characters and `ScanCode` for Escape.

Also notice the `continue` on error. After `WaitForEvent` returns, `ReadKeyStroke` should always succeed because an event just told us a key is available. But if something goes wrong (a race condition, a firmware quirk), we don't crash — we just go back to waiting.

## The Integer-to-String Helper

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

This function converts an unsigned integer to its decimal string representation. Without `printf` or `sprintf`, we need to do this ourselves.

The algorithm works in two phases:

**Phase 1: Extract digits in reverse.** `n % 10` gives the last digit, `n / 10` removes it. For example, 1024 produces: 4, 2, 0, 1. We store these in `tmp`.

**Phase 2: Reverse into the output.** We copy from `tmp` to `buf` in reverse order, so the digits come out in the right order: 1, 0, 2, 4.

The zero case is handled specially — without it, the loop body never executes (because `0 > 0` is false) and we'd produce an empty string.

## The Entry Point Revisited

Let's look at `efi_main()` one more time, now that we understand all the subsystems it coordinates:

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
```

The initialization order matters:

1. **Global state first.** Everything else needs `g_boot`.
2. **Watchdog off.** Before we do anything that might take time.
3. **Console message.** So we can see boot progress via serial even if framebuffer fails.
4. **Memory.** Other subsystems might need to allocate.
5. **Framebuffer.** Try to set up graphics, note whether it worked.
6. **Keyboard reset.** Clear any buffered keystrokes from the boot process.

After the main loop exits, we:
1. Print a shutdown message (via the console, which always works)
2. Wait one second so the user can read the message
3. Power off the machine

`Stall(1000000)` is UEFI's microsecond delay — 1,000,000 microseconds = 1 second. `ResetSystem(EfiResetShutdown, ...)` asks the firmware to power off. This function never returns — after it's called, the hardware powers down. The `return EFI_SUCCESS` is unreachable but satisfies the compiler's requirement that the function returns a value.

## The Build System

The Makefile orchestrates the three-step build process we detailed in Chapter 3:

```
Source files  ──compile──→  Object files  ──link──→  ELF binary  ──convert──→  PE binary
(src/*.c)                   (build/*.o)              (survival.so)             (survival.efi)
```

Let's highlight a few make targets:

```makefile
all: $(TARGET) esp
```

The default target builds the EFI binary AND copies it to the ESP directory. So a bare `make` produces everything needed for testing.

```makefile
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<
```

The pattern rule: any `.o` depends on the corresponding `.c`. Make only recompiles files that changed — if you edit `kbd.c`, only `kbd.o` is rebuilt, then the link and convert steps run again.

```makefile
esp: $(TARGET)
	@mkdir -p $(ESP_DIR)
	cp $(TARGET) $(ESP_DIR)/BOOTAA64.EFI
```

This copies the final binary to the UEFI boot path: `build/esp/EFI/BOOT/BOOTAA64.EFI`. The QEMU test script reads from this location.

## Testing with QEMU

Our `scripts/run-qemu.sh` creates a complete virtual ARM64 machine. Let's trace what happens when you run `./scripts/run-qemu.sh graphical`:

### 1. Find the Firmware

```bash
for fw in \
    /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
    /usr/share/edk2/aarch64/QEMU_EFI.fd \
    /usr/share/AAVMF/AAVMF_CODE.fd \
    ...
```

Different Linux distributions install UEFI firmware at different paths. The script tries them in order. On Ubuntu/Debian, it's usually at the first path.

### 2. Prepare Firmware Images

```bash
cp "$FIRMWARE" "$FW_COPY"
truncate -s 64M "$FW_COPY"
dd if=/dev/zero of="$VARS" bs=1M count=64 2>/dev/null
```

QEMU's pflash (persistent flash) requires the firmware images to be exactly 64 MB. The original `QEMU_EFI.fd` is smaller, so we pad it with `truncate`. The vars file stores UEFI variables (like boot order) — we create it empty.

### 3. Create the Disk Image

```bash
dd if=/dev/zero of="$IMG" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
mmd -i "$IMG" "::EFI"
mmd -i "$IMG" "::EFI/BOOT"
mcopy -i "$IMG" "$ESP_DIR/EFI/BOOT/BOOTAA64.EFI" "::EFI/BOOT/BOOTAA64.EFI"
```

This creates a 64 MB FAT32 disk image and copies our binary into it. The `mtools` commands (`mmd`, `mcopy`) manipulate FAT32 images without mounting them — no root permissions needed. The `::` prefix means "the root of the disk image."

### 4. Launch the VM

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

Breaking this down:

| Flag | Purpose |
|------|---------|
| `-M virt` | Generic ARM virtual machine (not emulating specific hardware) |
| `-cpu cortex-a53` | Same CPU as the Sweet Potato's S905X |
| `-m 256M` | 256 MB RAM (plenty for testing) |
| `-drive if=pflash...` (first) | UEFI firmware code (read-only) |
| `-drive if=pflash...` (second) | UEFI variable storage (read-write) |
| `-hda "$IMG"` | Our FAT32 disk with BOOTAA64.EFI |
| `-device ramfb` | Simple linear framebuffer device |
| `-device qemu-xhci` | USB 3.0 controller |
| `-device usb-kbd` | Virtual USB keyboard |
| `-device usb-mouse` | Virtual USB mouse |
| `-serial stdio` | Connect serial port to terminal |

The `ramfb` device is key. It provides a real, memory-mapped linear framebuffer — exactly like the hardware on the Sweet Potato. The alternative, `virtio-gpu-pci`, provides a GPU with command-based rendering but no linear framebuffer, which is why our framebuffer code can't use it.

### Three Test Modes

The script supports three modes:

**`graphical`** — Opens a GTK window showing the actual framebuffer output. You see exactly what would appear on a monitor connected to the Sweet Potato. Serial output also appears in your terminal.

**`console`** — No display window. Uses `virtio-gpu-pci` with `-display none`, which means `fb_init()` fails and we fall back to console mode. All text output goes to your terminal via serial. Good for automated testing or SSH sessions.

**`vnc`** — Like graphical, but displays via VNC on port 5900. Useful for headless development servers. Connect with any VNC client to `localhost:5900`.

## Lessons Learned Building Phase 1

Building this 64 KB binary taught us several hard-won lessons. Let's document them for future reference.

### The .rodata Section Must Be Included

When converting from ELF to PE with `objcopy`, we list which sections to include with `-j` flags. Missing `-j .rodata` produces a binary that compiles and links without errors but crashes immediately at boot with "Synchronous Exception."

This is because `.rodata` contains all string literals (`L"Hello"`, `"Booting..."`) and constant data (our entire font bitmap). Without it, any access to a string or constant reads from unmapped memory and triggers a page fault.

This is a particularly nasty bug because there are no compiler or linker warnings. Everything looks fine until you try to run it.

### Build Flags Must Match gnu-efi

Our initial build used minimal flags and produced binaries that crashed. The fix was to match the flags that gnu-efi uses when building its own examples:

- `-fPIC -fPIE` — Position-independent code (required for UEFI relocation)
- `-fno-merge-all-constants` — Prevents constant merging that breaks relocations
- `-pie` and `--no-dynamic-linker` — Linker produces a proper PIE without expecting a dynamic linker
- `-z common-page-size=4096 -z max-page-size=4096` — Match UEFI's 4 KB page size
- `-z norelro -z nocombreloc` — Disable security features that UEFI doesn't support

When something doesn't work, look at how the library's own examples build.

### Not All Display Devices Provide a Framebuffer

UEFI's GOP protocol can exist even when there's no usable framebuffer. `LocateProtocol` succeeds, `QueryMode` succeeds, but `FrameBufferBase` is NULL or zero. Our `fb_init()` checks for this and fails gracefully, falling back to console mode.

Always validate what you get from hardware, even when the API says it succeeded.

### Link Order Matters

The CRT startup code must come first. Libraries must come after the code that references them. Getting this wrong produces cryptic linker errors or binaries that crash at startup because relocations aren't processed correctly.

### Kernel-Style Freestanding C

With `-ffreestanding`, GCC promises not to call standard library functions. But it doesn't always keep that promise — struct copies and array initializations can still generate calls to `memcpy` and `memset`. Linking with `libgcc.a` usually provides these, but be aware of the trap.

## What We Built

Let's take stock. Our Phase 1 binary:

- Boots on an ARM64 machine via UEFI
- Initializes a framebuffer and renders text with an 8x16 bitmap font
- Accepts keyboard input and echoes characters to screen
- Falls back to console mode when no framebuffer is available
- Shuts down cleanly on ESC
- Is 64 KB total — smaller than many JPEG images

The codebase:

```
src/boot.h    — 40 lines   Global state, color constants
src/main.c    — 175 lines  Entry point, two main loops
src/fb.c      — 120 lines  Framebuffer driver
src/fb.h      — 20 lines   Framebuffer API
src/font.c    — 280 lines  Bitmap font data (mostly data, not logic)
src/font.h    — 12 lines   Font constants
src/kbd.c     — 52 lines   Keyboard input
src/kbd.h     — 40 lines   Key codes
src/mem.c     — 35 lines   Memory allocator
src/mem.h     — 18 lines   Memory API
```

About 800 lines total, of which 280 are font bitmap data. The actual logic is roughly 500 lines of C.

## What Comes Next

Phase 1 proves that our approach works. We can build a UEFI application, test it in QEMU, and run it on real ARM64 hardware. We have a foundation — screen, keyboard, memory — that everything else builds on.

Phase 2 will add FAT32 filesystem access using UEFI's Simple File System Protocol. This lets us read files from the SD card — opening the door to loading survival documentation, source code, and eventually, compiling and running programs right on the device.

The journey from "Hello, UEFI" to a fully self-hosting survival workstation is long, but every step builds directly on what came before. We have our foundation. Now we build on it.
