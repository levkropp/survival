---
layout: default
title: "Chapter 7: Hearing Keystrokes"
parent: "Phase 1: Boot & Input"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 7
---

# Chapter 7: Hearing Keystrokes

## The Other Half

We can paint pixels. Now we need to hear back from the user. Without keyboard input through our framebuffer system, the workstation is just an expensive picture frame.

In Chapter 4, we used UEFI's console input directly — `WaitForEvent` plus `ReadKeyStroke`, checking `ScanCode` and `UnicodeChar` fields inline. That worked for the console loop, but as we add a framebuffer UI, file browser, and text viewer, every piece of code that reads a key will need the same translation logic. Let's extract it into a module.

## How UEFI Keyboard Input Works

A quick recap. UEFI provides keyboard input through `SystemTable->ConIn`, the Simple Text Input Protocol. It has two members we care about:

- **`ReadKeyStroke`** — checks if a key is available. Returns immediately: `EFI_SUCCESS` with the key, or `EFI_NOT_READY` if nothing is buffered.
- **`WaitForKey`** — an event that fires when a key arrives. Pass it to `WaitForEvent` to block until input.

`ReadKeyStroke` fills an `EFI_INPUT_KEY`:

```c
typedef struct {
    UINT16 ScanCode;     // For special keys (arrows, function keys)
    CHAR16 UnicodeChar;  // For normal keys (letters, numbers, symbols)
} EFI_INPUT_KEY;
```

For normal keys like 'A', `ScanCode` is 0 and `UnicodeChar` is the character. For special keys like Up Arrow, `UnicodeChar` is 0 and `ScanCode` identifies which key. They're mutually exclusive.

The problem: every caller has to check both fields and translate. And UEFI's scan codes are small numbers (`0x01` for Up, `0x02` for Down) that could collide with ASCII characters. We need a unified key code space.

There's another problem we'll hit later: Simple Text Input tells us *what* key was pressed, but not *how*. Was Ctrl held? Shift? Alt? For a text editor with Ctrl+C copy and Ctrl+V paste, we need modifier state. UEFI has an answer for that too, but it's a separate protocol. We'll get to it.

## Defining Our Key Codes

Create `src/kbd.h`:

```c
#ifndef KBD_H
#define KBD_H

#include "boot.h"
```

First, the keys that have ASCII values — these match their standard ASCII codes:

```c
#define KEY_NONE    0
#define KEY_ESC     0x1B
#define KEY_ENTER   0x0D
#define KEY_BS      0x08
#define KEY_TAB     0x09
```

These work because pressing Enter sends `UnicodeChar = 0x0D`, which equals `KEY_ENTER`. Pressing Backspace sends `0x08`, which equals `KEY_BS`. We don't need any translation for these — the UEFI values and our constants are the same.

Now the keys that have no ASCII representation:

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
#define KEY_INS     0x89
```

We start at `0x80`. Why? ASCII uses only values `0x00` through `0x7F` (it's a 7-bit encoding). Anything at `0x80` or above can't collide with any ASCII character or control code. Callers can safely use a single `switch` statement covering both ASCII characters and our special keys. Insert gets `0x89`, right after Delete — we'll need it once the text editor supports insert/overwrite modes.

```c
#define KEY_F1      0x90
#define KEY_F2      0x91
#define KEY_F3      0x92
#define KEY_F4      0x93
#define KEY_F5      0x94
#define KEY_F6      0x95
#define KEY_F7      0x96
#define KEY_F8      0x97
#define KEY_F9      0x98
#define KEY_F10     0x99
```

Function keys start at `0x90`, leaving room in the `0x80` range for more navigation keys later. We define F1 through F10 — enough for the file browser and editor hotkeys we'll build in later chapters.

## Modifier Flags

Knowing *which* key was pressed is only half the story. A text editor needs to distinguish 'c' (type the letter) from Ctrl+C (copy). That requires modifier state: is Ctrl held? Alt? Shift?

```c
#define KMOD_CTRL   0x01
#define KMOD_ALT    0x02
#define KMOD_SHIFT  0x04
```

These are bit flags, so they compose naturally. Ctrl+Shift would be `KMOD_CTRL | KMOD_SHIFT`. A caller checking for Ctrl+C writes:

```c
if (ev.code == 0x03 && (ev.modifiers & KMOD_CTRL))
```

Where `0x03` is the control character for C (ASCII ETX). We'll see shortly how the keyboard module ensures this works correctly.

## The Key Event

```c
struct key_event {
    UINT16 code;     /* ASCII char or KEY_* constant */
    UINT16 scancode; /* raw UEFI scan code */
    UINT32 modifiers; /* KMOD_* flags (0 if InputEx unavailable) */
};
```

The caller only needs to check `code` for basic input. It's either a printable ASCII character ('A', '5', ' ') or one of our `KEY_*` constants. One field instead of two.

We also preserve the raw `scancode` in case a caller needs it for something we haven't anticipated. This is a common pattern: normalize the common case but keep the raw data available.

The `modifiers` field carries `KMOD_*` flags when modifier detection is available. When it isn't (older firmware, or firmware that only supports basic Simple Text Input), modifiers is simply 0. Callers that care about modifiers check the flags; callers that don't can ignore the field entirely. No conditional compilation, no `#ifdef` — just a field that might be zero.

## Two Ways to Read

```c
int kbd_poll(struct key_event *ev);
void kbd_wait(struct key_event *ev);

#endif /* KBD_H */
```

- **`kbd_poll`** — non-blocking. Returns 1 if a key was read, 0 if not. For situations where you're doing other work between input checks.
- **`kbd_wait`** — blocking. Suspends the CPU until a key arrives. For situations where there's nothing to do until the user types.

## SimpleTextInputEx: Getting Modifier State

Before we get to the translation table, we need to solve the modifier problem. UEFI actually has two keyboard protocols:

1. **Simple Text Input** (`EFI_SIMPLE_TEXT_INPUT_PROTOCOL`) — what we've been using. Gives us `ScanCode` and `UnicodeChar`. No modifier state.
2. **Simple Text Input Ex** (`EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`) — the extended version. Same key data, plus a `KeyState` structure that includes `KeyShiftState` — a bitmask telling us exactly which modifier keys are held.

The "Ex" protocol isn't guaranteed to exist. Some firmware implements it, some doesn't. Real hardware almost always has it; some minimal UEFI implementations might not. So we try once, cache the result, and fall back gracefully.

Create `src/kbd.c`:

```c
#include "kbd.h"

/* SimpleTextInputEx protocol — try once, cache result */
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *s_inputex;
static int s_inputex_tried;

static EFI_GUID s_inputex_guid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;

static void try_inputex(void) {
    if (s_inputex_tried)
        return;
    s_inputex_tried = 1;
    EFI_STATUS status = g_boot.bs->LocateProtocol(
        &s_inputex_guid, NULL, (void **)&s_inputex);
    if (EFI_ERROR(status))
        s_inputex = NULL;
}
```

`try_inputex` uses `LocateProtocol` to find the InputEx protocol. We call it at most once — `s_inputex_tried` ensures we don't repeat the lookup on every keystroke. If the protocol isn't available, `s_inputex` stays NULL and we use the basic protocol from `ConIn` instead.

This is a pattern worth noting: try the better interface, cache whether it exists, fall back to the simpler one. No error messages, no degraded mode warnings. The user doesn't need to know which protocol their firmware supports — the keyboard just works either way.

## The Translation Table

```c
static UINT16 scan_to_key(UINT16 scan) {
    switch (scan) {
    case 0x01: return KEY_UP;
    case 0x02: return KEY_DOWN;
    case 0x03: return KEY_RIGHT;
    case 0x04: return KEY_LEFT;
    case 0x05: return KEY_HOME;
    case 0x06: return KEY_END;
    case 0x07: return KEY_INS;
    case 0x08: return KEY_DEL;
    case 0x09: return KEY_PGUP;
    case 0x0A: return KEY_PGDN;
    case 0x0B: return KEY_F1;
    case 0x0C: return KEY_F2;
    case 0x0D: return KEY_F3;
    case 0x0E: return KEY_F4;
    case 0x0F: return KEY_F5;
    case 0x10: return KEY_F6;
    case 0x11: return KEY_F7;
    case 0x12: return KEY_F8;
    case 0x13: return KEY_F9;
    case 0x14: return KEY_F10;
    case 0x17: return KEY_ESC;
    default:   return KEY_NONE;
    }
}
```

A simple lookup from UEFI scan codes to our key codes. The `static` keyword means this function is only visible within `kbd.c` — callers use `kbd_poll` and `kbd_wait` instead.

We map all the keys we defined: navigation keys, Insert, Delete, F1 through F10, and the alternate ESC scan code. Unrecognized scan codes fall through to `default` and return `KEY_NONE`. The caller ignores `KEY_NONE`. This means unknown keys are silently dropped — no crash, no garbage on screen.

## Translating Modifier State

The InputEx protocol gives us `KeyState.KeyShiftState`, a UEFI-defined bitmask with flags like `EFI_LEFT_CONTROL_PRESSED`, `EFI_RIGHT_CONTROL_PRESSED`, and so on. We collapse left/right variants into our simpler `KMOD_*` flags:

```c
static UINT32 shift_to_modifiers(UINT32 shift_state) {
    UINT32 mods = 0;
    if (!(shift_state & EFI_SHIFT_STATE_VALID))
        return 0;
    if (shift_state & (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED))
        mods |= KMOD_CTRL;
    if (shift_state & (EFI_LEFT_ALT_PRESSED | EFI_RIGHT_ALT_PRESSED))
        mods |= KMOD_ALT;
    if (shift_state & (EFI_LEFT_SHIFT_PRESSED | EFI_RIGHT_SHIFT_PRESSED))
        mods |= KMOD_SHIFT;
    return mods;
}
```

Note the `EFI_SHIFT_STATE_VALID` check. UEFI sets this bit to indicate that the shift state data is actually meaningful. If it's not set, we return 0 — no modifiers — rather than interpreting garbage data.

We don't distinguish left Ctrl from right Ctrl. For a text editor, it doesn't matter. One modifier flag is simpler for callers and covers every use case we'll encounter.

## Polling

```c
int kbd_poll(struct key_event *ev) {
    try_inputex();

    if (s_inputex) {
        EFI_KEY_DATA kd;
        EFI_STATUS status = s_inputex->ReadKeyStrokeEx(s_inputex, &kd);
        if (EFI_ERROR(status)) {
            ev->code = KEY_NONE;
            ev->scancode = 0;
            ev->modifiers = 0;
            return 0;
        }
        ev->scancode = kd.Key.ScanCode;
        ev->modifiers = shift_to_modifiers(kd.KeyState.KeyShiftState);
        if (kd.Key.UnicodeChar != 0) {
            ev->code = (UINT16)kd.Key.UnicodeChar;
```

When InputEx is available, we use `ReadKeyStrokeEx` instead of `ReadKeyStroke`. It fills an `EFI_KEY_DATA` structure that contains both the key itself (`kd.Key`, same `ScanCode`/`UnicodeChar` pair) and the key state (`kd.KeyState`, with shift state flags). We translate the key the same way as before, but now we also populate `ev->modifiers`.

Here's where things get interesting:

```c
            /* Normalize Ctrl+letter to control character.
               Some firmware returns 'c' + CTRL flag instead of 0x03. */
            if ((ev->modifiers & KMOD_CTRL) &&
                ev->code >= 'a' && ev->code <= 'z')
                ev->code = ev->code - 'a' + 1;
            else if ((ev->modifiers & KMOD_CTRL) &&
                     ev->code >= 'A' && ev->code <= 'Z')
                ev->code = ev->code - 'A' + 1;
```

This is a critical normalization step. With basic SimpleTextInput, pressing Ctrl+C produces `UnicodeChar = 0x03` — the ASCII control character ETX. The firmware does the translation for us. But with SimpleTextInputEx, some firmware takes a different approach: it returns `UnicodeChar = 'c'` (the literal letter) plus the `KMOD_CTRL` modifier flag. It's telling you "the user pressed C while holding Ctrl" and leaving interpretation to you.

Both representations are valid, but our callers shouldn't have to handle both. So we normalize: when Ctrl is held and the character is a letter, we convert it to the corresponding control character. `'a'` becomes 1 (Ctrl+A, SOH), `'c'` becomes 3 (Ctrl+C, ETX), `'z'` becomes 26 (Ctrl+Z, SUB). The formula `char - 'a' + 1` maps the alphabet onto control codes 1-26, which is how ASCII was designed — this isn't an arbitrary encoding, it's the original intent of the control character range.

We handle both cases (lowercase 'a'-'z' and uppercase 'A'-'Z') because the firmware might send either depending on whether it already applied the Shift modifier.

This normalization is what makes Ctrl+C, Ctrl+V, Ctrl+X, and Ctrl+K work correctly in the text editor. Without it, the editor would see a literal 'c' and type the letter instead of copying.

```c
        } else {
            ev->code = scan_to_key(kd.Key.ScanCode);
        }
        return 1;
    }
```

If `UnicodeChar` is zero, it's a special key (arrow, function key, etc.) — we translate through `scan_to_key` as before.

When InputEx is unavailable, we fall back to the basic protocol:

```c
    /* Fallback: basic SimpleTextInput */
    EFI_INPUT_KEY key;
    EFI_STATUS status = g_boot.st->ConIn->ReadKeyStroke(g_boot.st->ConIn, &key);
    if (EFI_ERROR(status)) {
        ev->code = KEY_NONE;
        ev->scancode = 0;
        ev->modifiers = 0;
        return 0;
    }

    ev->scancode = key.ScanCode;
    ev->modifiers = 0;
    if (key.UnicodeChar != 0)
        ev->code = (UINT16)key.UnicodeChar;
    else
        ev->code = scan_to_key(key.ScanCode);
    return 1;
}
```

Same logic as before, but `modifiers` is always 0. With basic SimpleTextInput, Ctrl+C still works — the firmware returns `UnicodeChar = 0x03` directly, and our callers can check `ev.code == 0x03`. They just can't distinguish "user typed Ctrl+C" from "user somehow typed the ETX character directly." In practice this distinction never matters.

## Blocking Wait

```c
void kbd_wait(struct key_event *ev) {
    try_inputex();

    EFI_EVENT wait_event;
    if (s_inputex)
        wait_event = s_inputex->WaitForKeyEx;
    else
        wait_event = g_boot.st->ConIn->WaitForKey;

    UINTN index;
    g_boot.bs->WaitForEvent(1, &wait_event, &index);
    kbd_poll(ev);
}
```

Similar to before, but now we pick the right event to wait on. If InputEx is available, we wait on `WaitForKeyEx`; otherwise, `WaitForKey`. Then we call `kbd_poll` to read and normalize the key. We reuse `kbd_poll` instead of duplicating the translation logic — including the Ctrl+letter normalization.

The `1` is the event count. `&index` receives which event fired (always 0 since we only pass one). While waiting, the CPU goes idle — much better than spinning in a loop checking "any key yet?"

## The Complete Module

That's all of `kbd.c` — about 120 lines, up from the minimal version because of InputEx support and Ctrl normalization. The complete `kbd.h` is 51 lines. Still small, but the abstraction now handles quite a lot:

1. **Callers check one field** (`ev.code`) instead of two (`ScanCode` and `UnicodeChar`).
2. **Modifier state is available** when the firmware supports it, zero when it doesn't. No conditional compilation needed.
3. **Ctrl+letter is normalized.** Callers always see control characters (0x01-0x1A), never raw letters with a modifier flag. This makes keyboard shortcuts work consistently regardless of which UEFI protocol provided the data.
4. **Key codes are stable.** If we later replace UEFI input with a bare-metal USB HID driver, we change `kbd.c` and nothing else.
5. **Unknown keys are safe.** Unrecognized input produces `KEY_NONE`, which callers ignore.

## What We Have

```
src/boot.h   — Global state with framebuffer fields
src/mem.h/c  — Memory allocation and utilities
src/font.h/c — 8x16 bitmap font
src/fb.h/c   — Framebuffer driver
src/kbd.h/c  — Keyboard input with modifier detection
src/main.c   — Entry point (still using console loop)
```

We now have all the pieces: screen output (framebuffer), text rendering (font), and input (keyboard with modifier awareness). The next chapter wires them together into a working application with a framebuffer-based main loop.
