/*
 * gpt.h — GPT partition table creation for SD cards
 */
#ifndef GPT_H
#define GPT_H

#include <stdint.h>

/* Write a GPT partition table to the SD card with a single EFI System Partition.
 * The ESP starts at LBA 2048 (1MB aligned) and fills the card.
 * Writes: protective MBR, primary GPT header + entries, backup GPT.
 * Returns 0 on success. */
int gpt_create(uint64_t disk_size_bytes);

/* Returns the starting LBA of the EFI System Partition (2048). */
uint32_t gpt_esp_start_lba(void);

/* Returns the size of the EFI System Partition in sectors. */
uint32_t gpt_esp_size_sectors(void);

#endif /* GPT_H */
