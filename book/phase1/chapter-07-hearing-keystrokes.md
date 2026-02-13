# Chapter 7: Hearing Keystrokes

## The Problem of Input

We can paint pixels on the screen. Now we need to hear back from the user. Without keyboard input, our workstation is just a very expensive picture frame.

On a normal system, keyboard input goes through many layers:

```
Physical key press
       ↓
USB HID controller generates interrupt
       ↓
OS kernel USB driver reads HID report
       ↓
Kernel input subsystem translates to key event
       ↓
Window manager routes to focused application
       ↓
Application's event loop processes the key
```

We skip most of this. UEFI handles the USB stack and the HID translation for us. We just need to ask "was a key pressed?" and UEFI gives us the answer.

## How UEFI Keyboard Input Works

UEFI provides keyboard input through the **Simple Text Input Protocol**, which lives at `SystemTable->ConIn`. This protocol has two important members:

```c
typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_INPUT_RESET    Reset;         // Reset the input device
    EFI_INPUT_READ_KEY ReadKeyStroke; // Read a keystroke
    EFI_EVENT          WaitForKey;    // Event that fires when a key is available
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
```

### ReadKeyStroke

```c
EFI_STATUS ReadKeyStroke(
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    EFI_INPUT_KEY *Key
);
```

This function checks if a key has been pressed. If yes, it fills in the `Key` structure and returns `EFI_SUCCESS`. If no key is waiting, it returns `EFI_NOT_READY`. It never blocks — it always returns immediately.

The `EFI_INPUT_KEY` structure has two fields:

```c
typedef struct {
    UINT16 ScanCode;     // For special keys (arrows, function keys, etc.)
    CHAR16 UnicodeChar;  // For normal keys (letters, numbers, symbols)
} EFI_INPUT_KEY;
```

These two fields work together in a specific way:

- **Normal key** (like 'A'): `ScanCode` is 0, `UnicodeChar` is the character ('A', or 'a', depending on shift state)
- **Special key** (like Up arrow): `UnicodeChar` is 0, `ScanCode` identifies which special key

They're mutually exclusive. For any given keystroke, exactly one field is meaningful and the other is zero.

### WaitForKey

`WaitForKey` is a UEFI event. You don't call it directly — you pass it to `WaitForEvent`:

```c
EFI_STATUS WaitForEvent(
    UINTN NumberOfEvents,     // How many events to wait for
    EFI_EVENT *Event,         // Array of events
    UINTN *Index              // Which event fired (output)
);
```

`WaitForEvent` blocks — the CPU goes idle until one of the events fires. This is much better than busy-polling in a loop, because the CPU can enter a low-power state while waiting. When a key is pressed, the USB controller generates an interrupt, the firmware processes it, and `WaitForEvent` returns.

The `Index` output tells you which event in the array fired. Since we only pass one event, it's always 0. But the function is designed for waiting on multiple events simultaneously — useful when you want to wait for either a key press OR a timer tick.

### UEFI Scan Codes

The UEFI specification defines these scan codes for special keys:

```
0x01 = Up Arrow        0x09 = Page Up
0x02 = Down Arrow      0x0A = Page Down
0x03 = Right Arrow     0x0B = F1
0x04 = Left Arrow      0x0C = F2
0x05 = Home            0x0D = F3
0x06 = End             0x0E = F4
0x07 = Insert          0x0F = F5
0x08 = Delete          ...
0x17 = Escape          0x14 = F10
```

Notice that Escape has a scan code (`0x17`) but also an ASCII value (`0x1B`). Different firmware implementations handle this differently — some set the scan code, some set the Unicode character, some set both. We check for both to be safe.

## Our Keyboard Abstraction

We wrap UEFI's keyboard interface in a thin abstraction layer. The goal is twofold: simplify the calling code, and define our own key constants that won't change even if we replace the UEFI keyboard driver with a bare-metal USB driver later.

### The Header: kbd.h

```c
#ifndef KBD_H
#define KBD_H

#include "boot.h"
```

Standard include guard and our UEFI type definitions.

```c
/* Key codes for special keys */
#define KEY_NONE    0
#define KEY_ESC     0x1B
#define KEY_ENTER   0x0D
#define KEY_BS      0x08
#define KEY_TAB     0x09
```

These first five key codes match their ASCII values. `KEY_ESC` is `0x1B` (ASCII Escape), `KEY_ENTER` is `0x0D` (ASCII Carriage Return), `KEY_BS` is `0x08` (ASCII Backspace), and `KEY_TAB` is `0x09` (ASCII Horizontal Tab).

Why do these keys have ASCII values at all? Because they're control characters — the original ASCII standard reserved codes 0x00 through 0x1F for control purposes. When you press Enter on a terminal, it sends the byte `0x0D`. When you press Backspace, it sends `0x08`. These conventions date back to teletypes in the 1960s and are still with us today.

```c
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_HOME    0x84
#define KEY_END     0x85
#define KEY_PGUP    0x86
#define KEY_PGDN    0x87
#define KEY_DEL     0x88
```

For keys that don't have ASCII values, we define our own codes starting at `0x80`. Why `0x80`? Because ASCII only uses values 0x00 through 0x7F (it's a 7-bit encoding). Anything at `0x80` or above won't collide with any ASCII character, so we can safely use this range for our special keys.

```c
#define KEY_F1      0x90
#define KEY_F2      0x91
#define KEY_F3      0x92
#define KEY_F4      0x93
#define KEY_F5      0x94
#define KEY_F10     0x99
```

Function keys start at `0x90`, leaving room for more navigation keys in the `0x80` range if we need them later. We skip F6-F9 for now — we don't use them yet, and adding them later is trivial.

```c
struct key_event {
    UINT16 code;     /* ASCII char or KEY_* constant */
    UINT16 scancode; /* raw UEFI scan code */
};
```

Our key event structure. `code` is the normalized key code — either an ASCII character or one of our `KEY_*` constants. The caller only needs to check this one field.

`scancode` preserves the raw UEFI scan code, in case the caller needs it for something we haven't anticipated. This is a common pattern in input handling: normalize the common case but keep the raw data available.

Both fields are `UINT16` because UEFI's key values are 16-bit. The structure totals 4 bytes — small enough to pass by value, but we pass it by pointer for consistency with C conventions.

```c
int kbd_poll(struct key_event *ev);
void kbd_wait(struct key_event *ev);
```

Two functions, two modes of operation:

- `kbd_poll` checks if a key is available without blocking. Returns nonzero if a key was read, zero if not. This is for situations where you want to check for input while doing other work (like animation or periodic updates).

- `kbd_wait` blocks until a key is pressed. The CPU goes idle while waiting. This is for situations where there's nothing to do until the user types something — which is most of the time in our Phase 1 application.

```c
#endif /* KBD_H */
```

### The Implementation: kbd.c

```c
#include "kbd.h"
```

One include — `kbd.h` already pulls in `boot.h` for the UEFI types and `g_boot` global.

```c
static UINT16 scan_to_key(UINT16 scan) {
    switch (scan) {
    case 0x01: return KEY_UP;
    case 0x02: return KEY_DOWN;
    case 0x03: return KEY_RIGHT;
    case 0x04: return KEY_LEFT;
    case 0x05: return KEY_HOME;
    case 0x06: return KEY_END;
    case 0x08: return KEY_DEL;
    case 0x09: return KEY_PGUP;
    case 0x0A: return KEY_PGDN;
    case 0x0B: return KEY_F1;
    case 0x0C: return KEY_F2;
    case 0x0D: return KEY_F3;
    case 0x0E: return KEY_F4;
    case 0x0F: return KEY_F5;
    case 0x14: return KEY_F10;
    case 0x17: return KEY_ESC;
    default:   return KEY_NONE;
    }
}
```

This function translates UEFI scan codes into our `KEY_*` constants. It's a simple lookup — nothing clever.

The `static` keyword means this function is only visible within `kbd.c`. Callers use `kbd_poll` and `kbd_wait` instead. The translation is an internal detail they shouldn't depend on.

Notice that we don't have entries for every UEFI scan code. `0x07` (Insert) is missing because we don't need it yet. Unknown scan codes fall through to the `default` case and return `KEY_NONE`, which the caller ignores. This means if a user presses a key we don't handle, it's silently dropped — which is exactly what we want. No crash, no garbage on screen, just nothing happens.

```c
int kbd_poll(struct key_event *ev) {
    EFI_INPUT_KEY key;
    EFI_STATUS status;

    status = g_boot.st->ConIn->ReadKeyStroke(g_boot.st->ConIn, &key);
    if (EFI_ERROR(status)) {
        ev->code = KEY_NONE;
        ev->scancode = 0;
        return 0;
    }
```

`kbd_poll` starts by calling `ReadKeyStroke`. Remember, this function is non-blocking — it returns immediately. If no key is available, `status` will be `EFI_NOT_READY`.

On failure (no key), we zero out the event structure and return 0. Zeroing the structure is defensive: even though the caller should check the return value, a zeroed event is harmless if they accidentally read it.

```c
    ev->scancode = key.ScanCode;

    if (key.UnicodeChar != 0) {
        ev->code = (UINT16)key.UnicodeChar;
    } else {
        ev->code = scan_to_key(key.ScanCode);
    }

    return 1;
}
```

When we do get a key, we first save the raw scan code. Then we determine the normalized key code:

- If `UnicodeChar` is nonzero, the user pressed a normal key (letter, number, symbol, or a control character like Enter or Backspace). We use the Unicode character directly as the key code. Since our `KEY_ESC`, `KEY_ENTER`, `KEY_BS`, and `KEY_TAB` constants match their ASCII values, this works seamlessly — pressing Enter gives us `UnicodeChar = 0x0D`, which equals `KEY_ENTER`.

- If `UnicodeChar` is zero, the user pressed a special key (arrow, function key, etc.). We translate the scan code through `scan_to_key`.

The cast `(UINT16)` on `key.UnicodeChar` is technically unnecessary since `CHAR16` and `UINT16` are both 16-bit types, but it makes the intent explicit: we're treating this as a numeric code, not a character.

We return 1 to indicate "yes, a key was read."

```c
void kbd_wait(struct key_event *ev) {
    UINTN index;
    g_boot.bs->WaitForEvent(1, &g_boot.st->ConIn->WaitForKey, &index);
    kbd_poll(ev);
}
```

`kbd_wait` is elegantly simple. It does two things:

1. **Block until a key is available.** `WaitForEvent` suspends execution until the `WaitForKey` event fires. The `1` means we're waiting on exactly one event. `&index` receives which event fired (always 0 since we only have one).

2. **Read the key.** Once `WaitForEvent` returns, we know a key is available, so we call `kbd_poll` to read it. Since we just got notified that a key is ready, `kbd_poll` will always succeed here.

Why not call `ReadKeyStroke` directly instead of going through `kbd_poll`? Because `kbd_poll` handles the normalization (Unicode character vs. scan code). If we called `ReadKeyStroke` directly, we'd duplicate that logic. By calling `kbd_poll`, we keep the translation in one place.

## Polling vs. Blocking: When to Use Which

Our keyboard module offers two styles of input:

**Blocking (`kbd_wait`)** — The CPU sleeps until a key is pressed. Advantages:
- Zero CPU usage while waiting
- Simplest code — call `kbd_wait`, process key, repeat
- Perfect when there's nothing else to do

**Polling (`kbd_poll`)** — Check once and return immediately. Advantages:
- Can do other work between checks (animation, timers, background tasks)
- Essential for responsive UI that needs to update even without input
- Can check multiple input sources

In Phase 1, we use blocking mode exclusively. Our application has nothing to do between keystrokes — no cursor blinking, no clock display, no background processing. Blocking mode keeps the code simple and the CPU cool (literally — an idle CPU consumes much less power).

In later phases, when we have a blinking cursor, a status bar with a clock, or multiple windows that might need updating, we'll switch to polling mode with a timer-based event loop.

## Why an Abstraction Layer?

You might wonder: `kbd.c` is only 52 lines. The translation function and the poll/wait functions are straightforward wrappers. Why bother with the abstraction?

Three reasons:

**1. Consistent interface.** Without the abstraction, every piece of code that reads keyboard input would need to deal with the UEFI dual-field (ScanCode / UnicodeChar) system. With it, callers just check `ev.code` against our `KEY_*` constants. One field instead of two.

**2. Decoupling from UEFI.** If we later replace the UEFI keyboard driver with a bare-metal USB HID driver (Phase 9 in our roadmap), we change `kbd.c` and nothing else. Every caller keeps using `kbd_poll` and `kbd_wait` with the same `key_event` structure.

**3. Defensive defaults.** Our abstraction guarantees that unrecognized keys produce `KEY_NONE` instead of raw scan codes that calling code might misinterpret. It's a single place to filter and sanitize input.

## Key Takeaways

- UEFI provides keyboard input through the Simple Text Input Protocol at `SystemTable->ConIn`
- Keys arrive as either a Unicode character (for normal keys) or a scan code (for special keys)
- `WaitForEvent` blocks efficiently — the CPU goes idle instead of spinning in a busy loop
- We normalize UEFI's two-field key representation into a single code field
- Our `KEY_*` constants use 0x80+ to avoid collisions with ASCII
- The abstraction layer is thin but provides consistency, decoupling, and safety

Next: wiring everything together into a working application.
