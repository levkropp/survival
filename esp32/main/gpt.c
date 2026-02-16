/*
 * gpt.c — GPT partition table creation
 *
 * Creates a GUID Partition Table on the SD card with one EFI System Partition.
 * Mirrors what scripts/make-usb-image.sh does with sgdisk:
 *   sgdisk -n 1:2048:0 -t 1:EF00 -c 1:SURVIVAL
 *
 * Layout:
 *   LBA 0:        Protective MBR
 *   LBA 1:        Primary GPT header
 *   LBA 2..33:    Primary partition entries (128 entries × 128 bytes = 32 sectors)
 *   LBA 2048..N:  EFI System Partition (FAT32)
 *   LBA N+1..end: Backup GPT entries + header
 *
 * CRC32 computed via esp_rom_crc32_le() (available in ESP32 ROM).
 */

#include "gpt.h"
#include "sdcard.h"

#include <string.h>
#include "esp_rom_crc.h"
#include "esp_log.h"

static const char *TAG = "gpt";

#define SECTOR_SIZE     512
#define GPT_ENTRY_SIZE  128
#define GPT_ENTRIES     128
#define ENTRY_SECTORS   ((GPT_ENTRIES * GPT_ENTRY_SIZE) / SECTOR_SIZE)  /* 32 */
#define ESP_START_LBA   2048

/* EFI System Partition GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const uint8_t esp_type_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* Disk GUID — fixed value (not random, for reproducibility) */
static const uint8_t disk_guid[16] = {
    0x53, 0x55, 0x52, 0x56, 0x49, 0x56, 0x41, 0x4C,  /* "SURVIVAL" */
    0x44, 0x49, 0x53, 0x4B, 0x47, 0x55, 0x49, 0x44   /* "DISKGUID" */
};

/* Partition unique GUID */
static const uint8_t part_guid[16] = {
    0x53, 0x55, 0x52, 0x56, 0x50, 0x41, 0x52, 0x54,  /* "SURVPART" */
    0x30, 0x30, 0x30, 0x31, 0x45, 0x46, 0x49, 0x00   /* "0001EFI\0" */
};

static uint32_t last_usable_lba;
static uint32_t esp_size;

/* GPT header structure (92 bytes, padded to 512) */
#pragma pack(1)
struct gpt_header {
    uint8_t  signature[8];       /* "EFI PART" */
    uint32_t revision;           /* 0x00010000 */
    uint32_t header_size;        /* 92 */
    uint32_t header_crc32;       /* CRC32 of header (with this field zero) */
    uint32_t reserved;           /* 0 */
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_entry_crc32;
};

struct gpt_entry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name[36];           /* UTF-16LE partition name */
};
#pragma pack()

/* Shared sector buffer — static for DMA compatibility with SD SPI */
static uint8_t __attribute__((aligned(4))) s_buf[SECTOR_SIZE];

static uint32_t crc32(const void *data, size_t len)
{
    /* esp_rom_crc32_le includes initial 0xFFFFFFFF and final XOR internally,
     * so pass 0 to start a fresh standard CRC32 computation. */
    return esp_rom_crc32_le(0, (const uint8_t *)data, len);
}

static void set_utf16_name(uint16_t *dest, const char *src, int max)
{
    int i = 0;
    while (*src && i < max - 1)
        dest[i++] = (uint16_t)*src++;
    while (i < max)
        dest[i++] = 0;
}

int gpt_create(uint64_t disk_size_bytes)
{
    uint32_t total_sectors = (uint32_t)(disk_size_bytes / SECTOR_SIZE);
    if (total_sectors < ESP_START_LBA + 1024) {
        ESP_LOGE(TAG, "Card too small: %lu sectors", (unsigned long)total_sectors);
        return -1;
    }

    /* Backup GPT: entries at (last - 32), header at last sector */
    uint32_t backup_header_lba = total_sectors - 1;
    uint32_t backup_entries_lba = backup_header_lba - ENTRY_SECTORS;

    /* Usable range: after primary entries, before backup entries */
    uint32_t first_usable = 2 + ENTRY_SECTORS;  /* LBA 34 */
    last_usable_lba = backup_entries_lba - 1;
    esp_size = last_usable_lba - ESP_START_LBA + 1;

    ESP_LOGI(TAG, "Creating GPT: %lu sectors, ESP at LBA %u (%lu sectors)",
             (unsigned long)total_sectors, ESP_START_LBA, (unsigned long)esp_size);

    /* ---- Build the one partition entry (128 bytes) ----
     * Only entry 0 is populated; the remaining 127 are zeros.
     * We use the static s_buf throughout to stay DMA-safe. */
    memset(s_buf, 0, SECTOR_SIZE);

    struct gpt_entry *entry = (struct gpt_entry *)s_buf;
    memcpy(entry->type_guid, esp_type_guid, 16);
    memcpy(entry->unique_guid, part_guid, 16);
    entry->starting_lba = ESP_START_LBA;
    entry->ending_lba = last_usable_lba;
    entry->attributes = 0;
    set_utf16_name(entry->name, "SURVIVAL", 36);

    /* CRC32 over all 128 entries (16KB). First sector has our entry +
     * 3 empty entries; remaining 31 sectors are all zeros.
     * Compute incrementally — pass previous result to continue. */
    uint32_t crc = esp_rom_crc32_le(0, s_buf, SECTOR_SIZE);
    {
        uint8_t zero_tmp[SECTOR_SIZE];
        memset(zero_tmp, 0, SECTOR_SIZE);
        for (int i = 1; i < ENTRY_SECTORS; i++)
            crc = esp_rom_crc32_le(crc, zero_tmp, SECTOR_SIZE);
    }
    uint32_t entries_crc = crc;

    /* Write primary partition entries (LBA 2..33) — first sector has the entry */
    if (sdcard_write(2, 1, s_buf) != 0) return -1;
    /* Also write to backup location */
    if (sdcard_write(backup_entries_lba, 1, s_buf) != 0) return -1;

    /* Remaining 31 sectors are zeros */
    memset(s_buf, 0, SECTOR_SIZE);
    for (int i = 1; i < ENTRY_SECTORS; i++) {
        if (sdcard_write(2 + i, 1, s_buf) != 0) return -1;
        if (sdcard_write(backup_entries_lba + i, 1, s_buf) != 0) return -1;
    }

    /* ---- Primary GPT header (LBA 1) ---- */
    memset(s_buf, 0, SECTOR_SIZE);
    struct gpt_header *hdr = (struct gpt_header *)s_buf;
    memcpy(hdr->signature, "EFI PART", 8);
    hdr->revision = 0x00010000;
    hdr->header_size = 92;
    hdr->my_lba = 1;
    hdr->alternate_lba = backup_header_lba;
    hdr->first_usable_lba = first_usable;
    hdr->last_usable_lba = last_usable_lba;
    memcpy(hdr->disk_guid, disk_guid, 16);
    hdr->partition_entry_lba = 2;
    hdr->num_partition_entries = GPT_ENTRIES;
    hdr->partition_entry_size = GPT_ENTRY_SIZE;
    hdr->partition_entry_crc32 = entries_crc;
    hdr->header_crc32 = 0;
    hdr->header_crc32 = crc32(hdr, 92);
    if (sdcard_write(1, 1, s_buf) != 0) return -1;

    /* ---- Backup GPT header (last sector) ---- */
    hdr->my_lba = backup_header_lba;
    hdr->alternate_lba = 1;
    hdr->partition_entry_lba = backup_entries_lba;
    hdr->header_crc32 = 0;
    hdr->header_crc32 = crc32(hdr, 92);
    if (sdcard_write(backup_header_lba, 1, s_buf) != 0) return -1;

    /* ---- Protective MBR (LBA 0) ---- */
    memset(s_buf, 0, SECTOR_SIZE);
    s_buf[446] = 0x00;        /* status */
    s_buf[447] = 0x00;        /* CHS start */
    s_buf[448] = 0x02;
    s_buf[449] = 0x00;
    s_buf[450] = 0xEE;        /* type: GPT protective */
    s_buf[451] = 0xFF;        /* CHS end */
    s_buf[452] = 0xFF;
    s_buf[453] = 0xFF;
    s_buf[454] = 0x01; s_buf[455] = 0x00; s_buf[456] = 0x00; s_buf[457] = 0x00;
    uint32_t mbr_size = (total_sectors - 1 > 0xFFFFFFFF)
                       ? 0xFFFFFFFF : total_sectors - 1;
    memcpy(&s_buf[458], &mbr_size, 4);
    s_buf[510] = 0x55;
    s_buf[511] = 0xAA;
    if (sdcard_write(0, 1, s_buf) != 0) return -1;

    ESP_LOGI(TAG, "GPT written successfully");
    return 0;
}

uint32_t gpt_esp_start_lba(void)
{
    return ESP_START_LBA;
}

uint32_t gpt_esp_size_sectors(void)
{
    return esp_size;
}
