# Chapter 4: Hello, UEFI

## The Global State

Before we write our entry point, we need to decide how our program will access UEFI services. Every subsystem — framebuffer, keyboard, memory — needs access to the UEFI System Table. We could pass it as a parameter to every function, but that gets tedious. Instead, we'll store it in a global structure that any part of our code can access.

This is `src/boot.h`:

```c
#ifndef BOOT_H
#define BOOT_H
```

These are **include guards**. They prevent the file from being included twice in the same compilation unit. If `main.c` includes `boot.h` and also includes `fb.h` which itself includes `boot.h`, without guards the compiler would see all the definitions twice and report errors. The `#ifndef` / `#define` / `#endif` pattern ensures the contents are only processed once.

```c
#include <efi.h>
#include <efilib.h>
```

These are the gnu-efi headers. `efi.h` defines the core UEFI types:
- `EFI_STATUS` — a return code (success or error)
- `EFI_HANDLE` — an opaque pointer representing a UEFI object
- `EFI_SYSTEM_TABLE` — the master structure we discussed in Chapter 2
- `UINT32`, `UINT64`, `UINTN` — fixed-size integer types
- `CHAR16` — a 16-bit Unicode character (UEFI uses UTF-16)

`efilib.h` provides helper functions and macros for working with UEFI.

```c
struct boot_state {
    EFI_HANDLE image_handle;
    EFI_SYSTEM_TABLE *st;
    EFI_BOOT_SERVICES *bs;
    EFI_RUNTIME_SERVICES *rs;
```

This is our global state structure. We store:

- **`image_handle`** — The handle for our loaded image. Some UEFI functions need this to know who's calling.
- **`st`** — Pointer to the System Table. The root of everything.
- **`bs`** — Pointer to Boot Services. We extract this from the System Table for convenience. Boot Services provides memory allocation, protocol lookup, event handling, and more. These services are only available before `ExitBootServices()` is called.
- **`rs`** — Pointer to Runtime Services. Provides the system clock, shutdown/reboot, and persistent variables. These remain available even after `ExitBootServices()`.

```c
    /* Graphics */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    UINT32 *framebuffer;
    UINT32 fb_width;
    UINT32 fb_height;
    UINT32 fb_pitch;
    UINTN fb_size;
```

Graphics state. We'll fill these in when we initialize the framebuffer in Chapter 6. Key concepts:

- **`gop`** — The Graphics Output Protocol interface.
- **`framebuffer`** — A pointer to the pixel data in memory. Each pixel is a 32-bit value (8 bits each for blue, green, red, and a reserved byte). Writing to this memory changes what appears on screen.
- **`fb_width` / `fb_height`** — Screen resolution in pixels.
- **`fb_pitch`** — The number of pixels per horizontal scan line. This is often equal to `fb_width`, but can be larger if the hardware requires padding at the end of each row. Always use `pitch` (not `width`) when calculating pixel offsets.
- **`fb_size`** — Total size of the framebuffer in bytes.

```c
    /* Text cursor state */
    UINT32 cursor_x;
    UINT32 cursor_y;
    UINT32 cols;
    UINT32 rows;
};
```

We'll render text using a bitmap font (Chapter 6). These track the cursor position in **character coordinates** — column and row, not pixels. `cols` and `rows` are the maximum: the screen width divided by font width, and height divided by font height.

```c
extern struct boot_state g_boot;
```

The `extern` keyword says: "this variable exists somewhere, but it's defined in another file." The actual definition is in `main.c`:

```c
struct boot_state g_boot;
```

Without `extern`, every file that includes `boot.h` would try to create its own copy of `g_boot`, and the linker would complain about duplicate symbols.

```c
#define COLOR_BLACK   0x00000000
#define COLOR_WHITE   0x00FFFFFF
#define COLOR_GREEN   0x0000FF00
#define COLOR_RED     0x00FF0000
#define COLOR_BLUE    0x000000FF
#define COLOR_YELLOW  0x0000FFFF
#define COLOR_CYAN    0x00FFFF00
#define COLOR_GRAY    0x00808080
#define COLOR_DGRAY   0x00404040
```

Color constants. Each is a 32-bit value in **BGRX** format:
- Bits 0-7: Blue
- Bits 8-15: Green
- Bits 16-23: Red
- Bits 24-31: Reserved (usually zero)

This is the most common pixel format for UEFI GOP framebuffers. Note that it's BGR, not RGB — blue comes first in memory. So "pure red" is `0x00FF0000` (red in the third byte), and "pure blue" is `0x000000FF` (blue in the first byte).

Why BGR? It's a convention inherited from early VGA hardware. The UEFI specification calls this format `PixelBlueGreenRedReserved8BitPerColor`.

## The Entry Point

Now let's look at `src/main.c`. This is the heart of our application.

```c
#include "boot.h"
#include "fb.h"
#include "kbd.h"
#include "mem.h"

struct boot_state g_boot;
```

We include our headers and define the global state variable. Remember, `boot.h` declared it with `extern`; here we provide the actual storage.

### A Helper Function

```c
static void con_print(const CHAR16 *s) {
    g_boot.st->ConOut->OutputString(g_boot.st->ConOut, (CHAR16 *)s);
}
```

This is a convenience wrapper for printing to the UEFI text console. Let's unpack the UEFI call:

- `g_boot.st` — Our System Table pointer
- `->ConOut` — The console output protocol (type `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *`)
- `->OutputString(...)` — A function pointer in that protocol

The first argument to `OutputString` is the protocol itself (`g_boot.st->ConOut`). This is how UEFI does object-oriented programming in C: the "object" is passed as the first parameter to its own methods. It's like `self` in Python or `this` in C++.

`CHAR16 *` is a pointer to UTF-16 encoded text. The `L"..."` syntax in C creates a wide string literal. With our `-fshort-wchar` compiler flag, each character is 16 bits — matching UEFI's expectations.

The cast `(CHAR16 *)` discards the `const` qualifier because UEFI's `OutputString` function signature doesn't use `const` (a minor oversight in the specification).

The `static` keyword means this function is only visible within `main.c`. Other files can't call it. This is good practice for helper functions that don't need external visibility.

### The Main Function

```c
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status;
    int have_fb = 0;
```

This is where UEFI calls us. The return type `EFI_STATUS` is a UEFI status code. `EFI_SUCCESS` (0) means everything went well; any other value is an error code.

`have_fb` tracks whether we successfully initialized a framebuffer. If not, we'll fall back to console-only mode.

```c
    g_boot.image_handle = image_handle;
    g_boot.st = st;
    g_boot.bs = st->BootServices;
    g_boot.rs = st->RuntimeServices;
```

We stash the UEFI parameters into our global state. From here on, any code in any file can access these through `g_boot`.

Extracting `BootServices` and `RuntimeServices` into separate pointers saves us from writing `g_boot.st->BootServices->AllocatePool(...)` every time. Instead, we can write `g_boot.bs->AllocatePool(...)`.

```c
    g_boot.bs->SetWatchdogTimer(0, 0, 0, NULL);
```

UEFI sets a **watchdog timer** that reboots the system if the boot application doesn't finish within 5 minutes. This is a safety feature — if a boot loader hangs, the system reboots. But our application is meant to run indefinitely (it's a workstation, not a boot loader), so we disable it by setting the timeout to 0.

The function signature is:
```c
SetWatchdogTimer(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData)
```
All zeros means "disable."

```c
    con_print(L"SURVIVAL WORKSTATION: Booting...\r\n");
```

Our first output! This prints to the UEFI text console, which typically appears both on the HDMI display (if available) and on the serial port. It's useful for debugging because it works even if our framebuffer initialization fails.

The `\r\n` is a carriage-return + line-feed. UEFI follows the Windows convention of using both characters for a newline. If you only use `\n`, the cursor moves down but stays in the same column, producing staircase-shaped text.

```c
    mem_init();
```

Initialize our memory subsystem (Chapter 5). In our current implementation this is a no-op, but having the call here lets us add more complex memory management later without changing `main.c`.

```c
    status = fb_init();
    if (!EFI_ERROR(status)) {
        have_fb = 1;
        con_print(L"Framebuffer initialized.\r\n");
    } else {
        con_print(L"No framebuffer, falling back to console.\r\n");
    }
```

Try to initialize the framebuffer (Chapter 6). `EFI_ERROR()` is a macro that checks if a status code represents an error. If `fb_init()` succeeds, we set `have_fb = 1` and will use framebuffer rendering. Otherwise, we fall back to the simpler UEFI console output.

This fallback is important for testing. In QEMU with `-display none`, there's no real framebuffer — the GOP protocol exists but returns a null framebuffer address. Our console mode lets us test the application logic even without a display.

```c
    st->ConIn->Reset(st->ConIn, FALSE);
```

Reset the keyboard input buffer. This clears any keystrokes that might have been buffered during boot. The `FALSE` parameter means "don't run extended diagnostics."

```c
    if (have_fb)
        fb_loop();
    else
        console_loop();
```

We enter one of two main loops depending on whether we have a framebuffer. Both loops do the same thing — display a banner, accept keystrokes, echo them to screen, and exit on ESC. They just use different output methods.

```c
    con_print(L"\r\nShutting down...\r\n");
    g_boot.bs->Stall(1000000);
    g_boot.rs->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);

    return EFI_SUCCESS;
}
```

When the user presses ESC, we:

1. Print a goodbye message
2. Wait 1 second (`Stall` takes microseconds, so 1,000,000 = 1 second)
3. Ask the firmware to shut down the system

`ResetSystem` is a Runtime Service. `EfiResetShutdown` means "power off" (as opposed to `EfiResetCold` for reboot or `EfiResetWarm` for a warm reset).

The `return EFI_SUCCESS` at the end is technically unreachable — `ResetSystem` doesn't return. But the compiler doesn't know that, and omitting it would generate a warning.

## The Console Loop

Let's look at the console fallback mode:

```c
static void console_loop(void) {
    print_banner_console();

    EFI_INPUT_KEY key;
    UINTN index;

    for (;;) {
        g_boot.bs->WaitForEvent(1, &g_boot.st->ConIn->WaitForKey, &index);
```

`WaitForEvent` is a Boot Service that blocks (pauses execution) until an event fires. We pass it one event: `ConIn->WaitForKey`, which fires when a key is pressed.

The `1` is the number of events to wait for. `&index` receives the index of the event that fired (always 0 in our case since we only pass one event). This is a true block — the CPU goes idle until a key is pressed, saving power.

```c
        EFI_STATUS status = g_boot.st->ConIn->ReadKeyStroke(g_boot.st->ConIn, &key);
        if (EFI_ERROR(status))
            continue;
```

After `WaitForEvent` returns, we read the actual keystroke. `ReadKeyStroke` fills in an `EFI_INPUT_KEY` structure:

```c
typedef struct {
    UINT16 ScanCode;     // Special key (arrows, function keys, etc.)
    CHAR16 UnicodeChar;  // ASCII/Unicode character (for normal keys)
} EFI_INPUT_KEY;
```

For a normal key like 'A', `ScanCode` is 0 and `UnicodeChar` is `'A'`. For a special key like the up arrow, `UnicodeChar` is 0 and `ScanCode` identifies the key.

```c
        if (key.ScanCode == 0x17 || key.UnicodeChar == 0x1B)
            break;
```

Check for ESC. Scan code `0x17` is UEFI's code for the Escape key. We also check for the ASCII escape character `0x1B` just in case. Either one exits the loop.

```c
        if (key.UnicodeChar == '\r') {
            con_print(L"\r\n> ");
        } else if (key.UnicodeChar >= 0x20 && key.UnicodeChar <= 0x7E) {
            CHAR16 ch[2] = { key.UnicodeChar, 0 };
            con_print(ch);
        }
    }
}
```

For Enter (`\r`), we print a newline and a new prompt. For printable ASCII characters (0x20 space through 0x7E tilde), we echo the character. We create a 2-element `CHAR16` array — the character followed by a null terminator — because `OutputString` expects a null-terminated string, not a single character.

Characters outside this range (control characters, extended Unicode) are silently ignored.

## What We Achieved

With just this code, we have a working UEFI application that:

1. Receives control from the firmware
2. Disables the watchdog
3. Initializes subsystems
4. Displays a banner
5. Accepts keyboard input in an interactive loop
6. Shuts down cleanly on ESC

The binary is 64 KB. It boots on real ARM64 hardware with UEFI firmware and on QEMU. Not bad for our first step.

In the next chapter, we'll look at memory management — how to allocate and free memory when you don't have `malloc`.
