/*
 * main.c — Entry point for the Survival Workstation SD Card Flasher
 *
 * Runs on the ESP32-2432S028R ("Cheap Yellow Display"):
 *   - ILI9341 320x240 display on SPI2
 *   - XPT2046 resistive touch (bit-banged SPI)
 *   - SD card slot on SPI3
 *   - 4MB flash with compressed workstation images
 *
 * Boot sequence: init display → init touch → read payload manifest →
 * detect chip → show menu → wait for touch → flash SD card.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"

#include "display.h"
#include "touch.h"
#include "sdcard.h"
#include "payload.h"
#include "flasher.h"
#include "ui.h"

static const char *TAG = "main";

/* Chip detection — true if ESP32-S3 (has USB OTG) */
static bool has_usb_otg = false;

static void chip_detect(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    const char *name = "ESP32";
    if (info.model == CHIP_ESP32S3) {
        name = "ESP32-S3";
        has_usb_otg = true;
    } else if (info.model == CHIP_ESP32S2) {
        name = "ESP32-S2";
    } else if (info.model == CHIP_ESP32C3) {
        name = "ESP32-C3";
    }

    ESP_LOGI(TAG, "Chip: %s, cores: %d, flash: %s",
             name, info.cores,
             (info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Survival Workstation Flasher ===");

    /* Initialize peripherals */
    display_init();
    touch_init();

    /* Read payload manifest from flash partition */
    int payload_ok = payload_init();

    /* Detect chip variant */
    chip_detect();

    /* Show splash screen */
    ui_show_splash();
    vTaskDelay(pdMS_TO_TICKS(1500));

    if (payload_ok != 0) {
        ui_show_error("No payload found in flash.\n"
                      "Run pack_payload.py and\n"
                      "flash the payload partition.");
        /* Spin forever — nothing else to do */
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Main UI loop */
    while (1) {
        int choice = ui_show_menu(has_usb_otg);

        if (choice == UI_CHOICE_FLASH_AARCH64 || choice == UI_CHOICE_FLASH_X86_64) {
            const char *arch = (choice == UI_CHOICE_FLASH_AARCH64)
                             ? "aarch64" : "x86_64";

            /* Initialize SD card */
            if (sdcard_init() != 0) {
                ui_show_error("No SD card detected.\n"
                              "Insert a card and try again.");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            /* Run the flash sequence */
            int result = flasher_run(arch);
            sdcard_deinit();

            if (result == 0) {
                ui_show_done(arch);
            } else {
                ui_show_error("Flash failed.\n"
                              "Check serial log for details.");
            }

            /* Wait for tap before returning to menu */
            ui_wait_for_tap();
        }
    }
}
