#include "kbd.h"

/* Map UEFI scan codes to our key codes */
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

int kbd_poll(struct key_event *ev) {
    EFI_INPUT_KEY key;
    EFI_STATUS status;

    status = g_boot.st->ConIn->ReadKeyStroke(g_boot.st->ConIn, &key);
    if (EFI_ERROR(status)) {
        ev->code = KEY_NONE;
        ev->scancode = 0;
        return 0;
    }

    ev->scancode = key.ScanCode;

    if (key.UnicodeChar != 0) {
        ev->code = (UINT16)key.UnicodeChar;
    } else {
        ev->code = scan_to_key(key.ScanCode);
    }

    return 1;
}

void kbd_wait(struct key_event *ev) {
    UINTN index;
    g_boot.bs->WaitForEvent(1, &g_boot.st->ConIn->WaitForKey, &index);
    kbd_poll(ev);
}
