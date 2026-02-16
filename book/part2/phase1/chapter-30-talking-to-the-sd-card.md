---
layout: default
title: "Chapter 30: Talking to the SD Card"
parent: "Phase 1: The Pocket Flasher"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 4
---

# Chapter 30: Talking to the SD Card

## The Third SPI Device

We have a display on SPI2 and a touchscreen on bit-banged GPIO. The SD card slot gets the ESP32's last hardware SPI controller: SPI3.

SD cards are versatile. They support two interfaces: a fast 4-bit SDIO mode and a slower SPI mode. SDIO mode uses four data lines and can push 25+ MB/s. SPI mode uses a single data line and tops out around 2 MB/s. We use SPI mode because the CYD board wires the SD card slot to SPI pins, not SDIO pins — and because 2 MB/s is more than enough to write a 2 MB workstation image.

The pin assignments, like all CYD pins, are fixed by the PCB:

```c
#define PIN_MOSI  23
#define PIN_MISO  19
#define PIN_CLK   18
#define PIN_CS    5
```

## Initialization

ESP-IDF provides the `sdspi_host` component — a driver that handles the SD card SPI protocol, card detection, capacity probing, and sector-level I/O. We don't need to implement the SD card command protocol ourselves.

```c
#include "sdcard.h"

#include <string.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

static const char *TAG = "sdcard";

static sdmmc_card_t *card = NULL;
static bool bus_initialized = false;
```

`sdmmc_card_t` is ESP-IDF's card state structure — it holds the card's capacity, sector size, manufacturer ID, and other metadata populated during initialization. We keep a static pointer because only one card can be open at a time.

`bus_initialized` tracks whether the SPI3 bus has been set up. We separate bus initialization from card initialization because the bus setup is expensive (it configures DMA channels) and should only happen once, while card initialization may happen multiple times (each flash cycle deinits and reinits the card).

```c
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
```

The SPI bus configuration mirrors the display setup on SPI2: pin assignments, no quad-SPI, DMA auto-selection. The `max_transfer_sz` of 65,536 bytes (64 KB) matches our largest batch write — the FAT table zeroing loop (Chapter 32) writes 128 sectors at a time, which is 65,536 bytes.

```c
    /* SD card SPI device configuration */
    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.host_id = SPI3_HOST;
    dev_cfg.gpio_cs = PIN_CS;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
```

Two configuration structures: `sdspi_device_config_t` for the physical SPI connection (which host, which CS pin), and `sdmmc_host_t` for the SD protocol layer (command sequences, timing). The `_DEFAULT()` macros fill in sensible defaults; we only override the host ID and CS pin.

```c
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
```

`sdspi_host_init_device` registers the SD card as an SPI device on the bus. `sdmmc_card_init` sends the SD card initialization sequence — CMD0 (reset), CMD8 (voltage check), ACMD41 (initialize), CMD2 (get CID), CMD9 (get CSD) — and populates the `card` structure with the card's identity and capacity.

The log message shows the card's name (from the CID register — manufacturers like SanDisk, Samsung, Kingston) and capacity. A "Card: SD 16 GB" log line confirms everything is working.

Error handling follows a consistent pattern: log the error, free the card structure, set the pointer to NULL, return -1. The caller (`main.c`) shows an error on the display and returns to the menu.

## Deinitialization

```c
void sdcard_deinit(void)
{
    if (card) {
        free(card);
        card = NULL;
    }
    /* Leave SPI bus initialized — reinit is expensive */
}
```

We free the card structure but leave the SPI bus initialized. Bus initialization allocates DMA descriptors and configures hardware registers — tearing it down and setting it up again between flash cycles wastes time and risks resource leaks. Since the bus configuration never changes, we initialize it once and leave it alone.

## The Sector Abstraction

SD cards deal in **sectors** — fixed-size blocks of 512 bytes. You can't read or write individual bytes. Every operation addresses a sector number (LBA — Logical Block Address) and a count of consecutive sectors.

```c
uint64_t sdcard_size(void)
{
    if (!card) return 0;
    return (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
}

uint32_t sdcard_sector_size(void)
{
    return 512;
}
```

`sdcard_size()` returns the total capacity in bytes. The CSD (Card-Specific Data) register provides both the number of sectors and the sector size. We multiply them to get bytes.

`sdcard_sector_size()` always returns 512. The SD specification allows for larger sectors, but in practice all SD cards use 512-byte sectors in SPI mode. Hardcoding this simplifies the rest of the code — every buffer allocation can assume 512 bytes per sector.

## Reading and Writing Sectors

```c
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
```

Two thin wrappers around ESP-IDF's `sdmmc_read_sectors` and `sdmmc_write_sectors`. The parameters are:

- `lba` — starting sector number (0 = first sector on the card)
- `count` — number of consecutive 512-byte sectors
- `data` — pointer to a buffer holding `count * 512` bytes

The rest of the flasher — GPT, FAT32, file writing — uses only these two functions for all SD card I/O. Everything above this layer thinks in terms of sector numbers and sector buffers.

## DMA Alignment

There's a hidden requirement in the data buffers. The ESP32's SPI DMA engine requires that data buffers are 4-byte aligned. If you pass a misaligned pointer to `sdmmc_write_sectors`, the DMA transfer silently reads from the wrong address, corrupting data on the card.

Throughout the flasher code, every buffer that touches the SD card is declared with an alignment attribute:

```c
static uint8_t __attribute__((aligned(4))) s_buf[512];
```

`__attribute__((aligned(4)))` tells the compiler to place `s_buf` at an address divisible by 4. Static buffers allocated at file scope are usually aligned anyway (the compiler tends to align global variables to their natural boundary), but the explicit attribute makes the requirement visible and guarantees it regardless of compiler behavior.

This alignment requirement doesn't appear in Part 1's code. UEFI's `AllocatePool` always returns 8-byte-aligned memory, and x86/ARM CPUs can handle unaligned DMA (or the UEFI firmware's block I/O driver handles alignment internally). On the ESP32, the DMA hardware enforces it.

## The Complete Header

```c
#ifndef SDCARD_H
#define SDCARD_H

#include <stdint.h>
#include <stddef.h>

int sdcard_init(void);
void sdcard_deinit(void);
uint64_t sdcard_size(void);
uint32_t sdcard_sector_size(void);
int sdcard_write(uint32_t lba, uint32_t count, const void *data);
int sdcard_read(uint32_t lba, uint32_t count, void *data);

#endif /* SDCARD_H */
```

Six functions. Initialize, deinitialize, query size, query sector size, write sectors, read sectors. This is the entire interface between the flasher and the SD card hardware.

## Testing

With the SD card driver working, we can verify by reading sector 0:

```c
static uint8_t __attribute__((aligned(4))) test_buf[512];
sdcard_read(0, 1, test_buf);
ESP_LOGI(TAG, "Sector 0: %02x %02x %02x %02x ...",
         test_buf[0], test_buf[1], test_buf[2], test_buf[3]);
```

On a blank card, sector 0 is all zeros. On a card with an MBR, the first bytes are the x86 boot code (typically `0xEB 0x58 0x90`). On a card with a GPT, sector 0 has a protective MBR. Whatever the output, if you get bytes back without an error, the SD card driver is working.

## What We Have

```
sdcard.c   131 lines   SD card driver: init, deinit, size, read, write
sdcard.h    33 lines   Interface
```

131 lines, most of which is initialization boilerplate. The actual I/O is two functions of 10 lines each. ESP-IDF's SD card stack handles the hard parts — the SPI protocol, the initialization command sequence, the CRC checks, the DMA transfers. We provide a clean sector-level abstraction on top.

The layer below is complete. We can read and write arbitrary sectors on the SD card. Now we need to write something meaningful: a GPT partition table.
