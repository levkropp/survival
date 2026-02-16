---
layout: default
title: "Chapter 31: Creating a GPT"
parent: "Phase 1: The Pocket Flasher"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 5
---

# Chapter 31: Creating a GPT

## Why GPT?

UEFI firmware boots from GPT-partitioned disks. The older MBR partitioning scheme works on some UEFI implementations in legacy/CSM mode, but GPT is the standard that UEFI was designed around. If we want the SD card to boot reliably on any UEFI machine — x86_64 laptops, ARM64 single-board computers, whatever the survivor finds — GPT is the right choice.

GPT stands for GUID Partition Table. It's a disk layout format defined in the UEFI specification that describes how a disk is divided into partitions. Unlike MBR (which uses 32-bit sector addresses and tops out at 2 TB), GPT uses 64-bit addresses and supports up to 128 partitions. We need exactly one: an EFI System Partition containing our FAT32 filesystem.

## The Layout

Here's what the GPT looks like on an SD card:

```
Sector  Contents
──────  ────────────────────────────────────────
  0     Protective MBR
  1     Primary GPT header
  2-33  Primary partition entries (128 entries × 128 bytes = 32 sectors)
  34    (padding — not used by GPT)
  ...
  2048  ┐
  2049  │ EFI System Partition (FAT32)
  ...   │ (fills remaining card space)
  N-33  ┘
  N-32  ┐ Backup partition entries (32 sectors)
  ...   │
  N-1   ┘ Backup GPT header (last sector on disk)
```

The EFI System Partition starts at sector 2048 (exactly 1 MB into the disk). This 1 MB alignment is a convention — it ensures the partition starts on a boundary that works well with the erase block sizes of flash-based storage. For an SD card, the erase block is typically 4 MB, and 1 MB alignment guarantees sector writes don't cross erase boundaries.

The backup GPT at the end of the disk is a redundancy feature. If the primary GPT header or entries are corrupted, firmware can fall back to the backup. We write it because the specification requires it and because UEFI firmware may refuse to boot from a disk with a primary GPT but no backup.

## Constants and GUIDs

```c
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
```

128 entries of 128 bytes each = 16,384 bytes = 32 sectors. This is fixed by the specification — even though we only use one partition, we must allocate space for all 128 entries. The unused entries are zero-filled.

GPT uses GUIDs (128-bit unique identifiers) everywhere. Three are needed:

```c
/* EFI System Partition GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const uint8_t esp_type_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
```

The EFI System Partition type GUID is defined by the UEFI specification. Every UEFI firmware recognizes it as "this partition contains a FAT filesystem with boot files." The byte order is mixed-endian — the first three fields are little-endian (GUIDs predate the debate being settled), while the last two are big-endian. This produces the seemingly scrambled byte sequence you see above from the human-readable `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`.

```c
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
```

Normally, disk and partition GUIDs are random — `sgdisk` and `gdisk` generate them from `/dev/urandom`. We use fixed values because randomness isn't available on a microcontroller with no entropy source, and because reproducibility is useful for testing. The ASCII strings embedded in the bytes ("SURVIVALDISKGUID" and "SURVPART0001EFI") are a debugging convenience — if you hexdump the card, the GUIDs are recognizable.

## The Structures

The GPT header and partition entry are packed C structures that map directly to the on-disk layout:

```c
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
```

`#pragma pack(1)` ensures no padding between fields. Without it, the compiler might insert padding bytes to align fields to their natural boundary (e.g., 4 bytes for `uint32_t`), which would make the structure larger than the on-disk format expects.

Key fields in the header:

- **`signature`** — Always `"EFI PART"`. Firmware checks this first to confirm it's reading a GPT header.
- **`my_lba`** / **`alternate_lba`** — The primary header is at LBA 1 and points to the backup at the last sector. The backup header is at the last sector and points back to LBA 1. This cross-referencing lets firmware find the backup even if the primary is corrupt.
- **`first_usable_lba`** / **`last_usable_lba`** — The range of sectors available for partitions. Everything before (GPT structures) and after (backup GPT) is reserved.
- **`partition_entry_crc32`** — CRC32 of all 128 partition entries. Firmware verifies this to detect corruption.
- **`header_crc32`** — CRC32 of the header itself (92 bytes, with this field set to zero during computation).

The partition entry has the type GUID, a unique GUID, start/end LBA, attributes (we use none), and a UTF-16LE name. The name is purely informational — "SURVIVAL" appears in `gdisk` and `lsblk` listings.

## CRC32

GPT uses CRC32 for integrity checking. The ESP32's ROM provides `esp_rom_crc32_le`, a hardware-accelerated CRC32 function:

```c
static uint32_t crc32(const void *data, size_t len)
{
    return esp_rom_crc32_le(0, (const uint8_t *)data, len);
}
```

One important detail: `esp_rom_crc32_le` includes the standard initial value of `0xFFFFFFFF` and the final XOR internally. You pass `0` as the initial value, not `0xFFFFFFFF`. If you pass `0xFFFFFFFF`, it gets XOR'd with the internal initial value, producing `0x00000000` as the effective starting point — and every CRC comes out wrong.

This is the kind of bug that wastes hours. The function signature suggests it takes an initial CRC value for incremental computation, and the standard CRC32 algorithm starts with `0xFFFFFFFF`, so passing `0xFFFFFFFF` seems logical. But `esp_rom_crc32_le` already handles the initial value internally. Pass `0` for the first call.

For incremental CRC computation (spreading the calculation across multiple calls), you *do* pass the previous result:

```c
uint32_t crc = esp_rom_crc32_le(0, first_chunk, chunk_size);
crc = esp_rom_crc32_le(crc, second_chunk, chunk_size);
crc = esp_rom_crc32_le(crc, third_chunk, chunk_size);
```

The first call passes 0. Subsequent calls pass the running CRC. We use this for the partition entry CRC, which spans 32 sectors (16 KB) — too large to buffer in memory at once.

## Writing the GPT

The `gpt_create` function writes the entire partition table in one pass. Let's walk through it:

```c
int gpt_create(uint64_t disk_size_bytes)
{
    uint32_t total_sectors = (uint32_t)(disk_size_bytes / SECTOR_SIZE);
    if (total_sectors < ESP_START_LBA + 1024) {
        ESP_LOGE(TAG, "Card too small: %lu sectors", (unsigned long)total_sectors);
        return -1;
    }

    uint32_t backup_header_lba = total_sectors - 1;
    uint32_t backup_entries_lba = backup_header_lba - ENTRY_SECTORS;

    uint32_t first_usable = 2 + ENTRY_SECTORS;  /* LBA 34 */
    last_usable_lba = backup_entries_lba - 1;
    esp_size = last_usable_lba - ESP_START_LBA + 1;
```

The minimum card size check: we need at least 2048 sectors for the gap before the partition, plus 1024 sectors for a usable partition, plus 33 sectors for the backup GPT. Any card smaller than about 1.5 MB is rejected — in practice, the smallest SD cards are 128 MB.

The backup GPT lives at the very end of the disk. The backup header occupies the last sector; the backup entries occupy the 32 sectors before it. `last_usable_lba` is one sector before the backup entries — this is where our EFI System Partition ends.

`esp_size` is computed dynamically from the card size. On a 2 GB card, the partition is about 2 GB minus 1 MB of overhead. On a 32 GB card, it's about 32 GB. The flasher adapts to whatever card is inserted.

### Building the Partition Entry

```c
    memset(s_buf, 0, SECTOR_SIZE);

    struct gpt_entry *entry = (struct gpt_entry *)s_buf;
    memcpy(entry->type_guid, esp_type_guid, 16);
    memcpy(entry->unique_guid, part_guid, 16);
    entry->starting_lba = ESP_START_LBA;
    entry->ending_lba = last_usable_lba;
    entry->attributes = 0;
    set_utf16_name(entry->name, "SURVIVAL", 36);
```

One partition entry fills 128 bytes of the 512-byte sector buffer. The remaining 384 bytes are three more empty entries (all zeros). This first sector plus 31 sectors of pure zeros constitute the 128 entries.

`set_utf16_name` converts an ASCII string to UTF-16LE — each ASCII byte becomes a 16-bit value with the high byte zero:

```c
static void set_utf16_name(uint16_t *dest, const char *src, int max)
{
    int i = 0;
    while (*src && i < max - 1)
        dest[i++] = (uint16_t)*src++;
    while (i < max)
        dest[i++] = 0;
}
```

### Computing the Entries CRC

```c
    uint32_t crc = esp_rom_crc32_le(0, s_buf, SECTOR_SIZE);
    {
        uint8_t zero_tmp[SECTOR_SIZE];
        memset(zero_tmp, 0, SECTOR_SIZE);
        for (int i = 1; i < ENTRY_SECTORS; i++)
            crc = esp_rom_crc32_le(crc, zero_tmp, SECTOR_SIZE);
    }
    uint32_t entries_crc = crc;
```

The CRC must cover all 128 entries (32 sectors), but we can't buffer 16 KB at once. Instead, we compute incrementally: CRC the first sector (which has our one entry plus three empty slots), then CRC 31 sectors of zeros. Each call passes the previous CRC result to continue the computation.

The `zero_tmp` buffer inside a block scope is a minor optimization — it exists only for the CRC loop and is freed when the block ends. We could use `s_buf` zeroed out, but that would overwrite the entry we just built.

### Writing Partition Entries

```c
    /* Write primary partition entries (LBA 2..33) */
    if (sdcard_write(2, 1, s_buf) != 0) return -1;
    /* Also write to backup location */
    if (sdcard_write(backup_entries_lba, 1, s_buf) != 0) return -1;

    /* Remaining 31 sectors are zeros */
    memset(s_buf, 0, SECTOR_SIZE);
    for (int i = 1; i < ENTRY_SECTORS; i++) {
        if (sdcard_write(2 + i, 1, s_buf) != 0) return -1;
        if (sdcard_write(backup_entries_lba + i, 1, s_buf) != 0) return -1;
    }
```

Both primary (LBA 2-33) and backup (near end of disk) entries are written. The first sector has the real entry; the other 31 are zeros. Writing one sector at a time is slow (31 × 2 = 62 SD card writes), but the total data is only 32 KB — well under a second even at SPI speeds.

### The Primary GPT Header

```c
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
```

The header CRC is computed with `header_crc32` set to zero. We set all other fields first, then compute the CRC of the 92-byte header, then write the result into the field. This is the standard GPT self-referential CRC pattern.

`revision = 0x00010000` is version 1.0 of the GPT specification. `header_size = 92` is the defined size — the remaining bytes up to 512 are reserved (zeros).

### The Backup GPT Header

```c
    hdr->my_lba = backup_header_lba;
    hdr->alternate_lba = 1;
    hdr->partition_entry_lba = backup_entries_lba;
    hdr->header_crc32 = 0;
    hdr->header_crc32 = crc32(hdr, 92);
    if (sdcard_write(backup_header_lba, 1, s_buf) != 0) return -1;
```

The backup header is the same as the primary, except:
- `my_lba` points to the backup location (last sector)
- `alternate_lba` points back to the primary (sector 1)
- `partition_entry_lba` points to the backup entries (instead of sector 2)

The CRC is recomputed because three fields changed.

### The Protective MBR

```c
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
```

The protective MBR exists for backwards compatibility. Old tools that don't understand GPT would see an unpartitioned disk and might try to create an MBR on it — overwriting the GPT. The protective MBR contains a single partition entry of type `0xEE` (GPT protective) that spans the entire disk. Old tools see this and say "there's already something here, I won't touch it."

The MBR partition entry occupies bytes 446-461 of sector 0:
- Byte 446: status (0x00 = not bootable)
- Bytes 447-449: CHS start address (0x000200 — sector 1)
- Byte 450: type (0xEE = GPT protective)
- Bytes 451-453: CHS end address (0xFFFFFF — maximum, meaning "see LBA fields")
- Bytes 454-457: LBA start (1, little-endian)
- Bytes 458-461: LBA size (total_sectors - 1, or 0xFFFFFFFF if larger)

The `0x55AA` signature at bytes 510-511 is the MBR boot signature. Every MBR must have it, or BIOS/UEFI firmware won't recognize the sector as a valid MBR.

## Query Functions

```c
uint32_t gpt_esp_start_lba(void)
{
    return ESP_START_LBA;
}

uint32_t gpt_esp_size_sectors(void)
{
    return esp_size;
}
```

The FAT32 formatter (next chapter) needs to know where the partition starts and how large it is. These functions provide that information after `gpt_create` has computed the layout.

## Verification

After flashing a card with the GPT, you can verify it on a Linux machine:

```bash
$ sudo fdisk -l /dev/mmcblk0
Disklabel type: gpt
Disk identifier: 53555256-4956-414C-4449-534B47554944

Device         Start      End  Sectors  Size Type
/dev/mmcblk0p1  2048 30535646 30533599 14.6G EFI System
```

The disk identifier matches our ASCII-art GUID ("SURVIVALDISKGUID"). The single partition starts at sector 2048, fills the card, and has type "EFI System." The GPT is correct.

## What We Have

```
gpt.c   217 lines   GPT creation: protective MBR, headers, entries, CRC32
gpt.h    22 lines   Interface: create, query start LBA, query size
```

217 lines for a complete GPT implementation. The structures are straightforward — the complexity is in the details: mixed-endian GUIDs, self-referential CRC32 computation, the incremental CRC across 32 sectors of entries, the protective MBR for backwards compatibility, and the backup copy at the end of the disk.

The SD card now has a valid partition table. Next, we fill that partition with a FAT32 filesystem.
