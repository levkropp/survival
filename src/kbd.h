#ifndef KBD_H
#define KBD_H

#include "boot.h"

/* Key codes for special keys */
#define KEY_NONE    0
#define KEY_ESC     0x1B
#define KEY_ENTER   0x0D
#define KEY_BS      0x08
#define KEY_TAB     0x09
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_HOME    0x84
#define KEY_END     0x85
#define KEY_PGUP    0x86
#define KEY_PGDN    0x87
#define KEY_DEL     0x88
#define KEY_F1      0x90
#define KEY_F2      0x91
#define KEY_F3      0x92
#define KEY_F4      0x93
#define KEY_F5      0x94
#define KEY_F10     0x99

/* Key event */
struct key_event {
    UINT16 code;     /* ASCII char or KEY_* constant */
    UINT16 scancode; /* raw UEFI scan code */
};

/* Poll for key — returns 0 if no key, nonzero if key available */
int kbd_poll(struct key_event *ev);

/* Wait for a key press (blocking) */
void kbd_wait(struct key_event *ev);

#endif /* KBD_H */
