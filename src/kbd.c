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

/* Map UEFI scan codes to our key codes */
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
    case 0x15: return KEY_F11;
    case 0x16: return KEY_F12;
    case 0x17: return KEY_ESC;
    default:   return KEY_NONE;
    }
}

/* Map UEFI shift state to KMOD_* flags */
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
            /* Normalize Ctrl+letter to control character.
               Some firmware returns 'c' + CTRL flag instead of 0x03. */
            if ((ev->modifiers & KMOD_CTRL) &&
                ev->code >= 'a' && ev->code <= 'z')
                ev->code = ev->code - 'a' + 1;
            else if ((ev->modifiers & KMOD_CTRL) &&
                     ev->code >= 'A' && ev->code <= 'Z')
                ev->code = ev->code - 'A' + 1;
        } else {
            ev->code = scan_to_key(kd.Key.ScanCode);
        }
        return 1;
    }

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
