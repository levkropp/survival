---
layout: default
title: "Chapter 4: Hello, UEFI"
parent: "Phase 1: Boot & Input"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 4
---

# Chapter 4: Hello, UEFI

## The Bare Minimum

Let's write our first line of code. Not a plan, not a diagram — actual C that compiles and runs on a bare machine. We'll start with the absolute simplest program that does something visible, and build up from there.

A UEFI application needs an entry point. The firmware calls this function when it loads our binary:

```c
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    return EFI_SUCCESS;
}
```

That's a valid UEFI application. It does nothing and immediately returns. The firmware loaded us, we said "thanks, I'm done," and it continues the boot process.

But where do `EFI_STATUS`, `EFI_HANDLE`, and `EFI_SYSTEM_TABLE` come from? These are UEFI types defined in the gnu-efi headers. We need to include them. Create a file called `src/main.c`:

```c
#include <efi.h>
#include <efilib.h>

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    return EFI_SUCCESS;
}
```

`efi.h` defines the core UEFI types:
- `EFI_STATUS` — a return code (success or error)
- `EFI_HANDLE` — an opaque pointer representing a UEFI object
- `EFI_SYSTEM_TABLE` — the master structure from Chapter 2
- `UINT32`, `UINT64`, `UINTN` — fixed-size integer types
- `CHAR16` — a 16-bit Unicode character (UEFI uses UTF-16)

`efilib.h` provides helper macros like `EFI_ERROR()`.

The two parameters UEFI gives us are important. `image_handle` is an ID representing our loaded application — some UEFI functions need it to know who's calling. `st` is a pointer to the System Table, which is our gateway to every service the firmware provides.

This compiles, but it does nothing visible. Let's fix that.

## Printing to the Console

The System Table has a field called `ConOut` — the console output protocol. Through it, we can print text:

```c
#include <efi.h>
#include <efilib.h>

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    st->ConOut->OutputString(st->ConOut, L"Hello from UEFI!\r\n");
    return EFI_SUCCESS;
}
```

Let's unpack this call:

- `st->ConOut` — a pointer to the console output protocol (type `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *`)
- `->OutputString(...)` — a function pointer in that protocol that prints text

The first argument to `OutputString` is the protocol itself (`st->ConOut`). This looks redundant, but it's how UEFI does object-oriented programming in C. The "object" is passed as the first parameter to its own methods — like `self` in Python or `this` in C++.

`L"Hello from UEFI!\r\n"` is a wide string literal. The `L` prefix makes each character 16 bits wide. With our `-fshort-wchar` compiler flag from Chapter 3, C's `wchar_t` is 16 bits, matching UEFI's `CHAR16` type.

The `\r\n` is carriage-return plus line-feed. UEFI follows the Windows convention of requiring both characters for a newline. If you only use `\n`, the cursor moves down but stays in the same column, producing staircase-shaped text:

```
Hello
      from
            UEFI!
```

If you build and run this in QEMU, you'll see "Hello from UEFI!" on the serial console. Our code just ran on a bare ARM64 machine. No operating system. No C library. Just us and the firmware.

But this program prints one line and exits. We want a workstation that runs until we tell it to stop.

## Staying Alive

Two problems: our program exits immediately, and UEFI has a watchdog timer that will reboot the system if we run for more than 5 minutes. Let's fix both.

```c
#include <efi.h>
#include <efilib.h>

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    st->ConOut->OutputString(st->ConOut, L"SURVIVAL WORKSTATION: Booting...\r\n");

    /* Wait forever */
    for (;;) {
    }

    return EFI_SUCCESS;
}
```

`SetWatchdogTimer` is a Boot Service. The signature is:

```c
SetWatchdogTimer(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData)
```

Setting `Timeout` to 0 disables the timer entirely. The other parameters don't matter when disabling.

`st->BootServices` gives us the Boot Services table — a collection of functions for memory allocation, protocol lookup, event handling, timers, and more. We'll use it constantly.

The `for (;;)` is an infinite loop. Our program now runs forever (or until we cut power). But it's useless — it prints a message and then burns CPU doing nothing. We need keyboard input.

## Reading a Keystroke

UEFI provides keyboard input through `st->ConIn`, the console input protocol. It has a function called `ReadKeyStroke` and an event called `WaitForKey`:

```c
    st->ConOut->OutputString(st->ConOut, L"SURVIVAL WORKSTATION: Booting...\r\n");
    st->ConOut->OutputString(st->ConOut, L"Press any key (ESC to exit)...\r\n");

    EFI_INPUT_KEY key;
    UINTN index;

    for (;;) {
        st->BootServices->WaitForEvent(1, &st->ConIn->WaitForKey, &index);

        EFI_STATUS status = st->ConIn->ReadKeyStroke(st->ConIn, &key);
        if (EFI_ERROR(status))
            continue;

        if (key.ScanCode == 0x17 || key.UnicodeChar == 0x1B)
            break;
    }
```

`WaitForEvent` is a Boot Service that blocks — it pauses execution until one of the events you pass fires. We pass one event: `ConIn->WaitForKey`, which fires when a key is pressed. The CPU goes idle while waiting, saving power. This is much better than spinning in a tight loop checking "any key yet? any key yet?"

The `1` is the number of events. `&index` receives which event fired (always 0 since we only have one).

After `WaitForEvent` returns, we know a key is available. `ReadKeyStroke` retrieves it into an `EFI_INPUT_KEY` structure:

```c
typedef struct {
    UINT16 ScanCode;     // Special key (arrows, function keys, etc.)
    CHAR16 UnicodeChar;  // ASCII/Unicode character (for normal keys)
} EFI_INPUT_KEY;
```

For a normal key like 'A', `ScanCode` is 0 and `UnicodeChar` is `'A'`. For a special key like the up arrow, `UnicodeChar` is 0 and `ScanCode` identifies the key. They're mutually exclusive — for any keystroke, exactly one is meaningful.

We check for ESC two ways: scan code `0x17` is UEFI's code for the Escape key, and `0x1B` is the ASCII escape character. Different firmware implementations may set one or the other, so we check both.

But we're not doing anything with non-ESC keys yet. Let's echo them.

## Echoing Characters

```c
        if (key.ScanCode == 0x17 || key.UnicodeChar == 0x1B)
            break;

        if (key.UnicodeChar == '\r') {
            st->ConOut->OutputString(st->ConOut, L"\r\n> ");
        } else if (key.UnicodeChar >= 0x20 && key.UnicodeChar <= 0x7E) {
            CHAR16 ch[2] = { key.UnicodeChar, 0 };
            st->ConOut->OutputString(st->ConOut, ch);
        }
```

For Enter (`\r`), we print a newline and a prompt. For printable ASCII characters (space `0x20` through tilde `0x7E`), we echo the character.

There's a subtlety: `OutputString` expects a null-terminated `CHAR16` string, not a single character. We can't just pass the character — we create a 2-element array: the character followed by a null terminator `0`. This is a small annoyance of the UEFI API.

Characters outside the printable range (control characters, extended Unicode) are silently ignored. No crash, no garbage — just nothing happens.

## Shutting Down Cleanly

When the user presses ESC, we should shut down the machine instead of just returning to the firmware:

```c
    st->ConOut->OutputString(st->ConOut, L"\r\nShutting down...\r\n");
    st->BootServices->Stall(1000000);
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);

    return EFI_SUCCESS;
```

`Stall` is a Boot Service that pauses for a given number of microseconds. 1,000,000 microseconds = 1 second. This gives the user a moment to see the goodbye message.

`ResetSystem` is a Runtime Service. `EfiResetShutdown` means "power off" (as opposed to `EfiResetCold` for reboot or `EfiResetWarm` for a warm reset). This function never returns — the hardware powers down.

The `return EFI_SUCCESS` is unreachable but satisfies the compiler's requirement that a non-void function returns a value.

Notice we access `RuntimeServices` separately from `BootServices`. Boot Services handles the runtime operation of our application (memory, events, protocols). Runtime Services handles system-level operations that persist even after the OS takes over (clock, reboot, shutdown, persistent variables).

## The Full Program So Far

Here's everything we have in `src/main.c`:

```c
#include <efi.h>
#include <efilib.h>

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    st->ConOut->OutputString(st->ConOut, L"SURVIVAL WORKSTATION: Booting...\r\n");
    st->ConOut->OutputString(st->ConOut, L"Press any key (ESC to shutdown)...\r\n> ");

    st->ConIn->Reset(st->ConIn, FALSE);

    EFI_INPUT_KEY key;
    UINTN index;

    for (;;) {
        st->BootServices->WaitForEvent(1, &st->ConIn->WaitForKey, &index);

        EFI_STATUS status = st->ConIn->ReadKeyStroke(st->ConIn, &key);
        if (EFI_ERROR(status))
            continue;

        if (key.ScanCode == 0x17 || key.UnicodeChar == 0x1B)
            break;

        if (key.UnicodeChar == '\r') {
            st->ConOut->OutputString(st->ConOut, L"\r\n> ");
        } else if (key.UnicodeChar >= 0x20 && key.UnicodeChar <= 0x7E) {
            CHAR16 ch[2] = { key.UnicodeChar, 0 };
            st->ConOut->OutputString(st->ConOut, ch);
        }
    }

    st->ConOut->OutputString(st->ConOut, L"\r\nShutting down...\r\n");
    st->BootServices->Stall(1000000);
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);

    return EFI_SUCCESS;
}
```

We also added `st->ConIn->Reset(st->ConIn, FALSE)` before the loop. This clears any keystrokes that were buffered during the boot process. The `FALSE` parameter means "don't run extended diagnostics."

This is a complete, working UEFI application. It boots, prints a banner, echoes keystrokes, and shuts down on ESC. But look at how many times we type `st->BootServices->` and `st->ConOut->`. Every single UEFI call goes through the System Table. When we start adding more modules — framebuffer, memory, filesystem — every one of them will need `st`. Passing it around as a parameter to every function is going to get tedious.

## The Global State

Let's introduce a global structure to hold the UEFI pointers. Every module in our application will need access to these, so making them global is the pragmatic choice.

Create a new file, `src/boot.h`:

```c
#ifndef BOOT_H
#define BOOT_H

#include <efi.h>
#include <efilib.h>

struct boot_state {
    EFI_HANDLE image_handle;
    EFI_SYSTEM_TABLE *st;
    EFI_BOOT_SERVICES *bs;
    EFI_RUNTIME_SERVICES *rs;
};

extern struct boot_state g_boot;

#endif /* BOOT_H */
```

Let's go through this line by line.

```c
#ifndef BOOT_H
#define BOOT_H
```

**Include guards.** They prevent the file from being included twice in the same compilation unit. If `main.c` includes `boot.h` and also includes some other header that also includes `boot.h`, without guards the compiler would see all the definitions twice and complain about redefinitions. The `#ifndef` / `#define` / `#endif` pattern ensures the contents are processed only once.

```c
#include <efi.h>
#include <efilib.h>
```

We pull in the UEFI type definitions here, so any file that includes `boot.h` automatically gets access to them.

```c
struct boot_state {
    EFI_HANDLE image_handle;
    EFI_SYSTEM_TABLE *st;
    EFI_BOOT_SERVICES *bs;
    EFI_RUNTIME_SERVICES *rs;
};
```

Our global state structure. Right now it has exactly four fields — the four things we extracted from `efi_main`'s parameters:

- **`image_handle`** — The handle for our loaded image. Some UEFI functions need this to know who's calling.
- **`st`** — Pointer to the System Table. The root of everything.
- **`bs`** — Pointer to Boot Services. We extract this from `st->BootServices` for convenience. Instead of `st->BootServices->SetWatchdogTimer(...)`, we write `g_boot.bs->SetWatchdogTimer(...)`.
- **`rs`** — Pointer to Runtime Services. Extracted from `st->RuntimeServices`. Provides shutdown, reboot, and the system clock.

That's it. Four fields. We'll add more fields in later chapters as we need them — framebuffer pointers when we start drawing pixels, cursor position when we start rendering text. But right now, this is all we need.

```c
extern struct boot_state g_boot;
```

The `extern` keyword says: "this variable exists somewhere, but it's defined in another file." Every file that includes `boot.h` can *use* `g_boot`, but none of them *create* it. The actual storage will be in `main.c`.

Without `extern`, every file that includes `boot.h` would try to create its own copy of `g_boot`, and the linker would complain about duplicate symbols.

```c
#endif /* BOOT_H */
```

Close the include guard.

## Refactoring main.c

Now let's update `src/main.c` to use our global state:

```c
#include "boot.h"

struct boot_state g_boot;
```

We replace the two `#include <efi.h>` and `#include <efilib.h>` with a single `#include "boot.h"` — which pulls those in for us. And we define `g_boot` here — this is the actual storage that the `extern` declaration in `boot.h` referred to.

Note the different include syntax: `<efi.h>` uses angle brackets (search the system include paths), while `"boot.h"` uses quotes (search the current directory first). Our own headers use quotes.

Now let's add a helper function to make console printing less verbose:

```c
static void con_print(const CHAR16 *s) {
    g_boot.st->ConOut->OutputString(g_boot.st->ConOut, (CHAR16 *)s);
}
```

The `static` keyword means this function is only visible within `main.c`. Other files can't call it. This is good practice for helper functions that don't need external visibility.

The cast `(CHAR16 *)` discards the `const` qualifier because UEFI's `OutputString` signature doesn't use `const` — a minor oversight in the specification. Our function takes `const CHAR16 *` because it's the right thing to do: `con_print` doesn't modify the string.

Now the full refactored `src/main.c`:

```c
#include "boot.h"

struct boot_state g_boot;

static void con_print(const CHAR16 *s) {
    g_boot.st->ConOut->OutputString(g_boot.st->ConOut, (CHAR16 *)s);
}

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_boot.image_handle = image_handle;
    g_boot.st = st;
    g_boot.bs = st->BootServices;
    g_boot.rs = st->RuntimeServices;

    g_boot.bs->SetWatchdogTimer(0, 0, 0, NULL);

    con_print(L"SURVIVAL WORKSTATION: Booting...\r\n");
    con_print(L"Press any key (ESC to shutdown)...\r\n> ");

    st->ConIn->Reset(st->ConIn, FALSE);

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

    con_print(L"\r\nShutting down...\r\n");
    g_boot.bs->Stall(1000000);
    g_boot.rs->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);

    return EFI_SUCCESS;
}
```

The first thing `efi_main` does is stash the UEFI parameters into our global state. From that point on, any code anywhere can access Boot Services through `g_boot.bs`, Runtime Services through `g_boot.rs`, and the System Table through `g_boot.st`.

Compare the old `st->BootServices->SetWatchdogTimer(...)` with the new `g_boot.bs->SetWatchdogTimer(...)`. Slightly shorter, but more importantly, it works from any file — not just `efi_main`.

## What We Have

Two files. About 40 lines total:

```
src/boot.h   — Global state structure and UEFI includes
src/main.c   — Entry point, console I/O loop, shutdown
```

The application boots, prints a banner, echoes keystrokes to the UEFI console, and shuts down on ESC. It works in QEMU and on real hardware.

But we're drawing text using UEFI's built-in console, which is limited — we can't control colors, position, or font. To build a real workstation UI, we need to draw our own pixels. That requires two things: memory management (to allocate buffers) and a framebuffer driver (to paint pixels on screen).

We'll tackle memory first. It's the simpler problem, and the framebuffer code will need it.

---

**Next:** [Chapter 5: Memory Without an OS](chapter-05-memory-without-an-os)
