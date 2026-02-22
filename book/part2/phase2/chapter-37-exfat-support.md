---
layout: default
title: "Chapter 37: exFAT Support"
parent: "Phase 2: The Survival Toolkit"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 3
---

# Chapter 37: exFAT Support

## Why Two Filesystems

FAT32 has a 4 GB file size limit. That was fine when we controlled the card's contents — the kernel, initramfs, and bootloader are all under 4 GB. But when the file browser lets users read their own SD cards, we'll encounter cards formatted with exFAT. Any card 64 GB or larger ships from the factory formatted as exFAT. Any card with files larger than 4 GB — a single video, a disk image, a database backup — uses exFAT. Refusing to read these cards would make the file browser useless for half the SD cards people actually carry.

We already built a complete exFAT driver in Part 1 (Chapter 25) for the UEFI workstation. It reads directories, reads and writes files, manages the allocation bitmap, handles UTF-16 filenames, and follows the exFAT specification faithfully. It runs on the bare-metal x86_64 workstation we built in Part 1. Now we need it on the ESP32.

## The Architecture That Made Porting Easy

When we designed the Part 1 exFAT driver, we made one decision that pays off now: callback-based block I/O.

```c
typedef int (*exfat_block_read_fn)(void *ctx, uint64_t lba,
                                   uint32_t count, void *buf);
typedef int (*exfat_block_write_fn)(void *ctx, uint64_t lba,
                                    uint32_t count, const void *buf);

struct exfat_vol *exfat_mount(exfat_block_read_fn read_fn,
                               exfat_block_write_fn write_fn,
                               void *ctx, uint32_t block_size);
```

The driver never calls `disk_read_blocks()` directly. It never includes UEFI headers for I/O. All disk access goes through two function pointers provided at mount time. The caller decides how blocks get read and written. On x86_64 UEFI, those callbacks wrap `BlockIO->ReadBlocks()`. On ESP32, they wrap `sdcard_read()` and `sdcard_write()`.

This is the same pattern that makes the rest of our codebase portable. The display driver has platform-specific implementations behind a common `display.h`. The touch driver does the same. The exFAT driver takes it one step further — the I/O callbacks are runtime parameters, not compile-time selections.

## The Port: A Type Substitution Exercise

The Part 1 exFAT driver is 2,027 lines. The ESP32 port is the same 2,027 lines with mechanical substitutions. No algorithm changes. No restructuring. No new logic.

The substitutions:

| Part 1 (UEFI) | ESP32 | Notes |
|---|---|---|
| `UINT8` | `uint8_t` | `<stdint.h>` |
| `UINT16` | `uint16_t` | |
| `UINT32` | `uint32_t` | |
| `UINT64` | `uint64_t` | |
| `UINTN` | `size_t` | pointer-width integer |
| `mem_alloc(n)` | `malloc(n)` | `<stdlib.h>` |
| `mem_free(p)` | `free(p)` | |
| `mem_set(d,v,n)` | `memset(d,v,n)` | `<string.h>` |
| `mem_copy(d,s,n)` | `memcpy(d,s,n)` | |
| `str_len(s)` | `strlen(s)` | |
| `struct fs_entry` | `struct exfat_dir_info` | renamed for namespace |
| `FS_MAX_NAME` | `EXFAT_MAX_NAME` | still 128 |

Every substitution is a direct replacement. `mem_alloc` was always a thin wrapper around UEFI's `AllocatePool`; `malloc` is a thin wrapper around ESP-IDF's heap allocator. `mem_set` was `memset` under a different name. The Part 1 codebase used its own names because it ran without a C standard library — UEFI provides no libc. The ESP32 has a full libc via newlib, so we use the standard names.

One function needed a local replacement: `str_copy`. Part 1's version copies at most `max-1` characters and always null-terminates — exactly like `strlcpy`, but we don't assume `strlcpy` is available everywhere. A four-line static function handles it:

```c
static void str_copy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}
```

## The SD Card Wrapper

The generic `exfat_mount()` takes callback function pointers — useful for portability but verbose for the common case. Since the ESP32 has exactly one SD card, we add a convenience function:

```c
struct exfat_vol *exfat_mount_sdcard(uint32_t partition_start_lba);
```

Internally, it creates two callbacks that add the partition offset to every LBA:

```c
static struct {
    uint32_t partition_start_lba;
} s_exfat_ctx;

static int exfat_sd_read(void *ctx, uint64_t lba,
                         uint32_t count, void *buf)
{
    (void)ctx;
    return sdcard_read(
        (uint32_t)(s_exfat_ctx.partition_start_lba + lba),
        count, buf);
}
```

The partition start LBA comes from GPT parsing. The exFAT driver sees LBA 0 as the start of the volume; the callback translates that to the actual sector on disk. This is the same offset pattern our FAT32 driver uses — `s_fs.part_start` added to every `read_sector`/`write_sector` call — but expressed through callbacks instead of a global.

## How exFAT Differs from FAT32

The file browser in Chapter 38 will need to handle both filesystems. It helps to understand where they differ:

**Directory structure.** FAT32 directories are arrays of 32-byte entries — one per file, plus optional LFN entries prepended before each short entry. exFAT directories use "entry sets": a File entry (0x85) followed by a Stream Extension (0xC0) followed by one or more Name entries (0xC1). The File entry holds attributes and timestamps. The Stream Extension holds the file size, starting cluster, and a name hash. The Name entries hold 15 UTF-16LE characters each. Every file needs at least three directory entries.

**File sizes.** FAT32 stores file size as a 32-bit integer — maximum 4 GB. exFAT uses 64-bit, supporting files up to 16 exabytes in theory.

**Allocation tracking.** FAT32 tracks cluster allocation exclusively through the FAT table — each cluster's entry points to the next cluster or marks it free. exFAT has both a FAT table and a separate allocation bitmap. The bitmap is a simple bit-per-cluster structure loaded into memory at mount time. Directories can also mark files as "NoFatChain" (contiguous), meaning their clusters are sequential and the FAT doesn't need to be consulted.

**Case sensitivity.** FAT32 8.3 names are case-insensitive; the `nt_reserved` byte stores original case as flags. exFAT names are always case-insensitive, stored in UTF-16LE, compared via a specification-defined up-case table.

**Sector cache.** Our FAT32 driver uses three static 512-byte buffers. The exFAT driver uses an 8-entry LRU sector cache — each entry is a malloc'd 512-byte buffer. The cache reduces disk I/O when walking directory trees and FAT chains, since the same sectors get accessed repeatedly. The cost is 4 KB of heap (8 × 512 bytes).

## Memory Considerations

The exFAT driver allocates memory dynamically — something our FAT32 driver deliberately avoids. Three categories:

1. **Volume handle** (`struct exfat_vol`): ~300 bytes. Holds geometry, cache array, volume label. Allocated once at mount, freed at unmount.

2. **Sector cache**: 8 × 512 = 4,096 bytes. Allocated during `cache_init()`, freed during `cache_free()`. Each cache entry is a separate `malloc` so the buffers are properly aligned for DMA.

3. **Allocation bitmap**: variable size. This is the big one. The bitmap has one bit per cluster. A 32 GB card formatted with exFAT's default 32 KB clusters has about one million clusters, requiring a 128 KB bitmap. A 64 GB card might need 256 KB.

The ESP32 has roughly 300 KB of free heap after display buffers and other allocations. A 128 KB bitmap leaves 170 KB free — tight but workable. If the bitmap is too large for available memory, `malloc` returns NULL and `exfat_mount` fails gracefully. The file browser can display an error: "Card too large for exFAT — reformat as FAT32." In practice, cards up to 64 GB with default cluster sizes mount fine.

This is a different philosophy from our FAT32 driver, which uses zero heap allocation. The FAT32 driver was written for the ESP32 from scratch; the exFAT driver was ported from a system with 4 GB of RAM. The pragmatic choice is to keep the working design and accept the heap usage rather than rewrite 2,000 lines of tested code to use static buffers.

## What We Built

Two new files, two modified build files:

```
File                      Lines   Purpose
────────────────────────  ─────   ──────────────────────────────
esp32/main/exfat.h           91   Header: API + exfat_dir_info struct
esp32/main/exfat.c         2006   Full exFAT driver ported from Part 1
esp32/main/CMakeLists.txt     +1   Added exfat.c to build
cyd-emulator/CMakeLists.txt   +1   Added exfat.c to emulator build
```

The binary size doesn't change yet — the linker's garbage collection strips all exFAT functions because nothing calls them. When the file browser starts calling `exfat_mount_sdcard()` and `exfat_readdir()` in Chapter 38, those functions will link in and the binary will grow by roughly 8 KB. That's 8 KB of flash for full read/write support on the filesystem format used by every modern SD card over 32 GB.

The exFAT driver gives us the other half of SD card support. FAT32 handles cards we format ourselves (the flasher) and small cards. exFAT handles the cards people bring from the outside world — the 64 GB card from their camera, the 128 GB card with their document backups. The file browser won't care which filesystem it's reading. It'll try FAT32 first, then exFAT, and display whatever it finds.

---

**Next:** [Chapter 38: The File Browser](chapter-38-the-file-browser)
