/*
 * main.c — Entry point for the Survival Workstation
 *
 * Runs on the ESP32-2432S028R ("Cheap Yellow Display"):
 *   - ILI9341 320x240 display on SPI2
 *   - XPT2046 resistive touch (bit-banged SPI)
 *   - SD card slot on SPI3
 *   - 4MB flash with compressed workstation images
 *
 * Boot sequence: init display → init touch → init settings →
 * read payload manifest → detect chip → show splash → home screen loop.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"

#include "display.h"
#include "touch.h"
#include "payload.h"
#include "settings.h"
#include "app.h"
#include "ui.h"

static const char *TAG = "main";

static void chip_detect(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    const char *name = "ESP32";
    if (info.model == CHIP_ESP32S3)      name = "ESP32-S3";
    else if (info.model == CHIP_ESP32S2) name = "ESP32-S2";
    else if (info.model == CHIP_ESP32C3) name = "ESP32-C3";

    ESP_LOGI(TAG, "Chip: %s, cores: %d, flash: %s",
             name, info.cores,
             (info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Survival Workstation v2.0 ===");

    /* Initialize peripherals */
    display_init();
    touch_init();
    settings_init();

    /* Read payload manifest from flash partition (non-fatal) */
    int payload_ok = payload_init();
    if (payload_ok != 0) {
        ESP_LOGW(TAG, "No payload found — Flash SD will not work");
    }

    /* Detect chip variant */
    chip_detect();

    /* Show splash screen */
    ui_show_splash();
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Main app loop: home screen → run app → repeat */
    while (1) {
        int choice = home_screen_show();
        g_apps[choice].run();
    }
}
