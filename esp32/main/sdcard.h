/*
 * sdcard.h — SD card driver via sdspi_host on SPI3
 */
#ifndef SDCARD_H
#define SDCARD_H

#include <stdint.h>
#include <stddef.h>

/* Initialize the SD card on SPI3. Returns 0 on success. */
int sdcard_init(void);

/* Release the SD card and SPI bus. */
void sdcard_deinit(void);

/* Get total card size in bytes. */
uint64_t sdcard_size(void);

/* Get sector size (always 512). */
uint32_t sdcard_sector_size(void);

/* Write sectors to the SD card.
 * lba: starting sector number
 * count: number of 512-byte sectors
 * data: buffer with count*512 bytes
 * Returns 0 on success. */
int sdcard_write(uint32_t lba, uint32_t count, const void *data);

/* Read sectors from the SD card. Returns 0 on success. */
int sdcard_read(uint32_t lba, uint32_t count, void *data);

#endif /* SDCARD_H */
