/*
 * ui.h — Touch UI state machine (menu, progress, done/error)
 */
#ifndef UI_H
#define UI_H

/* Menu choices */
#define UI_CHOICE_FLASH_AARCH64  1
#define UI_CHOICE_FLASH_X86_64   2

/* Show splash screen: "SURVIVAL WORKSTATION" + version info. */
void ui_show_splash(void);

/* Show the main menu with architecture selection buttons.
 * If has_usb is true, shows a grayed-out "USB Boot (ESP32-S3)" hint.
 * Blocks until a button is tapped. Returns a UI_CHOICE_* value. */
int ui_show_menu(int has_usb);

/* Update the progress display during flashing. */
void ui_update_progress(const char *status, int current, int total);

/* Show "Done!" screen. */
void ui_show_done(const char *arch);

/* Show an error message. */
void ui_show_error(const char *message);

/* Block until the user taps anywhere. */
void ui_wait_for_tap(void);

#endif /* UI_H */
