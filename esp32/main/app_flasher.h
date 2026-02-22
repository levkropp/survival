/*
 * app_flasher.h — Flash SD card app
 */
#ifndef APP_FLASHER_H
#define APP_FLASHER_H

/* Run the flasher app. Shows confirmation screen (x86_64 default),
 * or arch selection if show_aarch64 is enabled in settings. */
void app_flasher_run(void);

#endif /* APP_FLASHER_H */
