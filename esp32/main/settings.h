/*
 * settings.h — Persistent settings for the Survival Workstation
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

typedef struct {
    bool show_aarch64;  /* show aarch64 option in Flash app */
} settings_t;

/* Global settings instance */
extern settings_t g_settings;

/* Initialize settings with defaults. */
void settings_init(void);

/* Save settings to NVS (ESP32 only, stub on emulator). */
void settings_save(void);

#endif /* SETTINGS_H */
