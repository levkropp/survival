/*
 * ui.h — Shared UI helpers (splash, progress, done/error, back button)
 */
#ifndef UI_H
#define UI_H

/* Show splash screen: "SURVIVAL WORKSTATION" + version info. */
void ui_show_splash(void);

/* Update the progress display during flashing. */
void ui_update_progress(const char *status, int current, int total);

/* Show "Done!" screen. */
void ui_show_done(const char *arch);

/* Show an error message. */
void ui_show_error(const char *message);

/* Block until the user taps anywhere. */
void ui_wait_for_tap(void);

/* Draw a "< Back" button in the top-left corner. */
void ui_draw_back_button(void);

/* Check if a tap at (tx, ty) hits the back button. */
int ui_check_back_button(int tx, int ty);

#endif /* UI_H */
