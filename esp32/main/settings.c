/*
 * settings.c — Settings storage (NVS on ESP32, in-memory on emulator)
 */

#include "settings.h"

settings_t g_settings;

void settings_init(void)
{
    g_settings.show_aarch64 = false;
}

void settings_save(void)
{
    /* NVS persistence added in Chapter 42 */
}
