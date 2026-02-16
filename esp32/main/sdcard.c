/*
 * sdcard.c — SD card access via sdspi_host on SPI3
 *
 * CYD SD card slot pins: MOSI=23, MISO=19, CLK=18, CS=5
 */

#include "sdcard.h"

#include <string.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

static const char *TAG = "sdcard";

/* CYD SD card pin assignments */
#define PIN_MOSI  23
#define PIN_MISO  19
#define PIN_CLK   18
#define PIN_CS    5

static sdmmc_card_t *card = NULL;
static bool bus_initialized = false;

int sdcard_init(void)
{
    if (card) return 0;  /* already initialized */

    ESP_LOGI(TAG, "Initializing SD card on SPI3");

    /* Initialize the SPI bus */
    if (!bus_initialized) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = PIN_MOSI,
            .miso_io_num = PIN_MISO,
            .sclk_io_num = PIN_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 65536,
        };
        esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
            return -1;
        }
        bus_initialized = true;
    }

    /* SD card SPI device configuration */
    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.host_id = SPI3_HOST;
    dev_cfg.gpio_cs = PIN_CS;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    /* Allocate and probe the card */
    card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    if (!card) {
        ESP_LOGE(TAG, "Out of memory");
        return -1;
    }

    esp_err_t ret = sdspi_host_init_device(&dev_cfg, &host.slot);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device init failed: %s", esp_err_to_name(ret));
        free(card);
        card = NULL;
        return -1;
    }

    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Card init failed: %s", esp_err_to_name(ret));
        free(card);
        card = NULL;
        return -1;
    }

    ESP_LOGI(TAG, "Card: %s, %llu MB",
             card->cid.name,
             (unsigned long long)(card->csd.capacity) *
             (unsigned long long)(card->csd.sector_size) / (1024 * 1024));
    return 0;
}

void sdcard_deinit(void)
{
    if (card) {
        free(card);
        card = NULL;
    }
    /* Leave SPI bus initialized — reinit is expensive */
}

uint64_t sdcard_size(void)
{
    if (!card) return 0;
    return (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
}

uint32_t sdcard_sector_size(void)
{
    return 512;
}

int sdcard_write(uint32_t lba, uint32_t count, const void *data)
{
    if (!card) return -1;
    esp_err_t ret = sdmmc_write_sectors(card, data, lba, count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write failed at LBA %lu: %s",
                 (unsigned long)lba, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

int sdcard_read(uint32_t lba, uint32_t count, void *data)
{
    if (!card) return -1;
    esp_err_t ret = sdmmc_read_sectors(card, data, lba, count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read failed at LBA %lu: %s",
                 (unsigned long)lba, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}
