/*
 * app.h — App framework for the Survival Workstation
 *
 * Each app has a name, icon, color, and run() function.
 * run() takes over the screen and returns when the user exits.
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>

#define APP_COUNT 6

/* App descriptor */
typedef struct {
    const char     *name;       /* label shown below icon */
    const uint8_t  *icon;       /* 32x32 monochrome bitmap (128 bytes) */
    uint16_t        icon_color; /* foreground color for the icon */
    void          (*run)(void); /* takes over screen, returns to go home */
} app_t;

/* Global app registry (defined in apps.c) */
extern app_t g_apps[APP_COUNT];

/* Show the home screen grid. Blocks until an icon is tapped.
 * Returns the index (0..APP_COUNT-1) of the chosen app. */
int home_screen_show(void);

#endif /* APP_H */
