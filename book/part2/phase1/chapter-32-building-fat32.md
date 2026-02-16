---
layout: default
title: "Chapter 32: Building FAT32"
parent: "Phase 1: The Pocket Flasher"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 6
---

# Chapter 32: Building FAT32

## Why FAT32 Again?

The UEFI specification mandates FAT32 for the EFI System Partition. There is no alternative. If you want UEFI firmware to find and load `EFI/BOOT/BOOTAA64.EFI` or `EFI/BOOT/BOOTX64.EFI`, the partition must contain a valid FAT32 filesystem.

We built FAT32 support in Part 1 — [Chapter 24](../../part1/phase5/chapter-24-the-format-tool) created a formatter for USB drives, and the file browser reads and writes FAT32 throughout. The ESP32 version is a port of that code, adapted to work with standard C types (replacing UEFI's `UINT32` with `uint32_t`) and to write through the SD card SPI driver instead of UEFI's block I/O protocol.

But there are new challenges. The ESP32 has 180 KB of available DRAM — not enough to buffer an entire FAT table in memory. Files arrive as compressed streams that must be decompressed and written in chunks. And the FAT32 code needs to create directories, handle long filenames, and manage cluster chains, all within the memory budget. This chapter walks through the complete implementation: 893 lines of C, the largest single file in the flasher.

## The BPB (BIOS Parameter Block)

Every FAT32 volume begins with a boot sector at the first sector of the partition. This sector contains the BPB — a collection of fields that describe the filesystem geometry:

```c
#pragma pack(1)
struct fat32_bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
    uint8_t  boot_code[420];
    uint16_t signature;
};
#pragma pack()
```

Every field is specified by the FAT specification (Microsoft's "FAT: General Overview of On-Disk Format"). Let's explain the ones that matter for our formatter:

- **`jmp[3]`** — `{0xEB, 0x58, 0x90}` — a short JMP instruction that skips the BPB. On x86, this would jump to the boot code area. We don't use boot code, but the jump instruction is expected.
- **`oem[8]`** — `"SURVIVAL"` — an 8-byte identifier. Can be anything; we use our project name.
- **`bytes_per_sector`** — Always 512.
- **`sectors_per_cluster`** — How many sectors make up one cluster (the allocation unit). More on this shortly.
- **`reserved_sectors`** — 32. The area between the BPB and the first FAT copy. Contains the FSInfo sector and backup boot sector.
- **`num_fats`** — 2. Two copies of the FAT table for redundancy.
- **`total_sectors_32`** — Total sectors in the partition (from GPT).
- **`fat_size_32`** — Sectors per FAT copy.
- **`root_cluster`** — 2. The root directory starts at cluster 2 (the first data cluster).
- **`fs_info_sector`** — 1. The FSInfo structure is at sector 1 of the partition.
- **`backup_boot_sector`** — 6. A backup copy of the boot sector at sector 6.
- **`signature`** — `0xAA55` (little-endian) — the boot signature. Without it, nothing recognizes this as a valid boot sector.

## Choosing Sectors Per Cluster

Clusters are the filesystem's allocation unit. Files are stored in chains of clusters. Each cluster is `sectors_per_cluster` × 512 bytes. Larger clusters mean fewer clusters on the disk, which means a smaller FAT table — but also more wasted space when small files don't fill their last cluster.

The FAT32 specification requires at least 65,525 data clusters. Below that threshold, the filesystem is technically FAT16 or FAT12, and UEFI firmware won't mount it as FAT32.

```c
#define MIN_FAT32_CLUSTERS 65525

/* Choose sectors_per_cluster */
uint32_t spc = 8;
while (spc > 1) {
    uint32_t est = partition_sectors - RESERVED_SECTORS;
    uint32_t ncl = est / spc;
    uint32_t fat_sec = (ncl * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t data_sec = partition_sectors - RESERVED_SECTORS - fat_sec * NUM_FATS;
    ncl = data_sec / spc;
    if (ncl >= MIN_FAT32_CLUSTERS) break;
    spc /= 2;
}
s_fs.spc = spc;
```

We start with 8 sectors per cluster (4 KB clusters) and check if the resulting cluster count meets the FAT32 minimum. If not, we halve the cluster size and try again: 4, 2, 1 sectors per cluster. On a typical SD card (2 GB or larger), 8 sectors per cluster gives hundreds of thousands of clusters — well above the minimum. On a very small card (128 MB), we might need 2 sectors per cluster (1 KB) to stay above 65,525 clusters.

The estimation loop computes the FAT table size iteratively. Each FAT entry is 4 bytes, so `ncl * 4` bytes of FAT, rounded up to whole sectors. Two FAT copies are subtracted from the data area. The remaining data sectors divided by `spc` gives the final cluster count.

## Internal State

The filesystem state is tracked in a static structure:

```c
static struct {
    uint32_t part_start;
    uint32_t fat_start;       /* absolute LBA */
    uint32_t fat_sectors;
    uint32_t data_start;      /* absolute LBA */
    uint32_t total_clusters;
    uint32_t next_free_cluster;
    uint32_t spc;
} s_fs;
```

All LBAs are absolute — `part_start` is added once during initialization, and all subsequent operations use absolute sector numbers. This avoids the error-prone pattern of adding the partition offset in every I/O call.

The layout inside the partition:

```
Sector (relative)    Contents
──────────────────   ─────────────────────
0                    BPB (boot sector)
1                    FSInfo
2-5                  (reserved)
6                    Backup BPB
7                    Backup FSInfo
8-31                 (reserved)
32                   FAT copy 1 (start)
32+fat_sectors       FAT copy 2 (start)
32+fat_sectors*2     Data region (cluster 2)
```

## Shared Buffers

Memory is tight. Every buffer that touches the SD card must be DMA-aligned and statically allocated (to avoid heap fragmentation):

```c
static uint8_t __attribute__((aligned(4))) s_buf[SECTOR_SIZE];
static uint8_t __attribute__((aligned(4))) s_dir[SECTOR_SIZE];

#define ZERO_BATCH 128  /* sectors per bulk zero write (64KB) */
static uint8_t __attribute__((aligned(4))) s_zero[SECTOR_SIZE * ZERO_BATCH];
```

Three buffers serving different roles:

- **`s_buf`** — Used by FAT table operations (`fat_set`, `fat_get`) and formatting. 512 bytes.
- **`s_dir`** — Used by directory operations (`find_in_dir`, `add_dir_entry`, `ensure_dir`). 512 bytes.
- **`s_zero`** — 64 KB of zeros for bulk zeroing the FAT tables. Declared as BSS, so it doesn't consume flash — the startup code zero-fills it.

Why two single-sector buffers instead of one? Directory operations (`find_in_dir`) need to read directory sectors, but they also call `fat_get` to follow cluster chains — which reads FAT sectors into its own buffer. If both used the same buffer, the directory read would be overwritten by the FAT read. Two buffers prevent this conflict.

## FAT Table Operations

The FAT (File Allocation Table) is an array of 32-bit entries, one per cluster. Each entry either:
- Contains `0x00000000` (free cluster)
- Points to the next cluster in a chain (cluster number)
- Contains `0x0FFFFFF8` or higher (end-of-chain marker)

```c
static int fat_set(uint32_t cluster, uint32_t value)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_offset / SECTOR_SIZE;
    uint32_t fat_entry_offset = fat_offset % SECTOR_SIZE;

    if (read_sector(s_fs.fat_start + fat_sector, s_buf) < 0)
        return -1;

    uint32_t *entry = (uint32_t *)(s_buf + fat_entry_offset);
    *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);

    /* Write to both FAT copies */
    if (write_sector(s_fs.fat_start + fat_sector, s_buf) < 0)
        return -1;
    if (write_sector(s_fs.fat_start + s_fs.fat_sectors + fat_sector, s_buf) < 0)
        return -1;
    return 0;
}
```

The masking with `0x0FFFFFFF` is important. FAT32 entries are technically 28 bits — the top 4 bits are reserved and must be preserved when writing. The read-modify-write cycle keeps those bits intact.

Both FAT copies are updated on every write. This is the redundancy that lets `fsck.fat` repair one copy from the other if corruption occurs.

```c
static uint32_t alloc_cluster(uint32_t prev)
{
    uint32_t cl = s_fs.next_free_cluster;
    if (cl >= s_fs.total_clusters + 2)
        return 0;

    s_fs.next_free_cluster = cl + 1;
    fat_set(cl, FAT32_EOC);
    if (prev >= 2)
        fat_set(prev, cl);
    return cl;
}
```

Cluster allocation is sequential: we maintain a `next_free_cluster` counter and always allocate the next one in line. No free-list search, no bitmap — just an incrementing counter. This works because we're writing to a freshly formatted filesystem where every cluster after the root directory is free. The sequential allocation also produces contiguous file layouts, which is optimal for read performance.

If `prev >= 2`, the new cluster is appended to an existing chain: the previous cluster's FAT entry is updated to point to the new one. The new cluster is marked as end-of-chain.

## Formatting

The `fat32_format` function creates a fresh filesystem:

```c
int fat32_format(uint32_t partition_start_lba, uint32_t partition_sectors,
                 fat32_progress_cb progress)
{
    s_fs.part_start = partition_start_lba;

    /* ... (cluster size selection shown above) ... */

    s_fs.fat_start = s_fs.part_start + RESERVED_SECTORS;
    s_fs.data_start = s_fs.fat_start + s_fs.fat_sectors * NUM_FATS;
    s_fs.total_clusters = (partition_sectors - RESERVED_SECTORS - s_fs.fat_sectors * NUM_FATS)
                          / spc;
    s_fs.next_free_cluster = 3;
```

Cluster numbering starts at 2 (0 and 1 are reserved). The root directory is cluster 2. So the first allocatable cluster for files is 3.

After computing the layout, the formatter writes the metadata structures:

### The BPB

```c
    memset(s_buf, 0, SECTOR_SIZE);
    struct fat32_bpb *bpb = (struct fat32_bpb *)s_buf;
    bpb->jmp[0] = 0xEB; bpb->jmp[1] = 0x58; bpb->jmp[2] = 0x90;
    memcpy(bpb->oem, "SURVIVAL", 8);
    bpb->bytes_per_sector = SECTOR_SIZE;
    bpb->sectors_per_cluster = (uint8_t)spc;
    bpb->reserved_sectors = RESERVED_SECTORS;
    bpb->num_fats = NUM_FATS;
    bpb->media_type = 0xF8;
    bpb->total_sectors_32 = partition_sectors;
    bpb->fat_size_32 = s_fs.fat_sectors;
    bpb->root_cluster = 2;
    bpb->fs_info_sector = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->boot_sig = 0x29;
    bpb->volume_id = 0x12345678;
    memcpy(bpb->volume_label, "SURVIVAL   ", 11);
    memcpy(bpb->fs_type, "FAT32   ", 8);
    bpb->signature = 0xAA55;

    if (write_sector(s_fs.part_start, s_buf) < 0) return -1;
    if (write_sector(s_fs.part_start + 6, s_buf) < 0) return -1;
```

Written twice: once at sector 0 of the partition (the primary) and once at sector 6 (the backup). The `media_type = 0xF8` means "fixed disk" — the standard value for non-removable media (and conventionally used for SD cards too). `volume_label` is padded to exactly 11 bytes with spaces.

### The FSInfo Sector

```c
    memset(s_buf, 0, SECTOR_SIZE);
    struct fat32_fsinfo *fsi = (struct fat32_fsinfo *)s_buf;
    fsi->lead_sig = 0x41615252;
    fsi->struct_sig = 0x61417272;
    fsi->free_count = s_fs.total_clusters - 1;
    fsi->next_free = 3;
    fsi->trail_sig = 0xAA550000;
    if (write_sector(s_fs.part_start + 1, s_buf) < 0) return -1;
    if (write_sector(s_fs.part_start + 7, s_buf) < 0) return -1;
```

FSInfo provides hints to the OS about free space: how many clusters are free and where to start looking. The three signature values (`0x41615252`, `0x61417272`, `0xAA550000`) are magic constants defined by the specification — they are how the OS confirms it's reading a valid FSInfo sector and not garbage.

### Zeroing the FAT Tables

This is the most time-consuming part of formatting:

```c
    int fat_total = (int)(s_fs.fat_sectors * NUM_FATS);
    int fat_done = 0;
    if (zero_sectors_ex(s_fs.fat_start, s_fs.fat_sectors,
                        progress, &fat_done, fat_total) < 0)
        return -1;
    if (zero_sectors_ex(s_fs.fat_start + s_fs.fat_sectors, s_fs.fat_sectors,
                        progress, &fat_done, fat_total) < 0)
        return -1;
```

Both FAT copies must be completely zeroed. Garbage from previous card contents — remnants of an old filesystem, fragments of photos, whatever was on the card before — would appear as corrupted cluster entries. Linux's `fsck.fat` rejects filesystems with non-zero entries in the FAT that don't form valid chains.

The zeroing is done in batches using `s_zero`, the 64 KB buffer:

```c
static int zero_sectors_ex(uint32_t lba, uint32_t count,
                           fat32_progress_cb progress, int *done, int total)
{
    while (count > 0) {
        uint32_t batch = (count > ZERO_BATCH) ? ZERO_BATCH : count;
        if (sdcard_write(lba, batch, s_zero) != 0)
            return -1;
        lba += batch;
        count -= batch;
        if (progress && done) {
            *done += (int)batch;
            progress(*done, total);
        }
    }
    return 0;
}
```

Writing 128 sectors (64 KB) per SD card command is much faster than writing one sector at a time. SD cards optimize for large sequential writes — the flash controller can erase and program an entire page in one operation instead of 128 separate erase-program cycles.

The progress callback updates the display during zeroing. On a 16 GB card with 8 sectors per cluster, the FAT has about 31,000 sectors per copy, or 62,000 total. At 128 sectors per batch, that's about 485 SD card writes. At ~2 ms per write, zeroing takes about a second — long enough that the user needs visual feedback.

After zeroing, three initial FAT entries are set:

```c
    fat_set(0, 0x0FFFFFF8);  /* media type marker */
    fat_set(1, 0x0FFFFFFF);  /* end-of-chain marker (clean shutdown flag) */
    fat_set(2, FAT32_EOC);   /* root directory cluster (end of chain) */
```

Cluster 0's entry is a copy of the media type byte. Cluster 1's entry is an end-of-chain marker that also serves as a "clean shutdown" flag. Cluster 2 is the root directory — a single-cluster chain.

### The Root Directory

```c
    uint64_t root_lba = cluster_to_lba(2);
    if (zero_sectors((uint32_t)root_lba, spc) < 0) return -1;

    memset(s_buf, 0, SECTOR_SIZE);
    struct fat32_dir_entry *de = (struct fat32_dir_entry *)s_buf;
    memcpy(de->name, "SURVIVAL   ", 11);
    de->attr = ATTR_VOLUME_ID;
    de->modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;
    if (write_sector((uint32_t)root_lba, s_buf) < 0) return -1;
```

The root directory's first entry is the volume label — a special directory entry with the `ATTR_VOLUME_ID` attribute. The name "SURVIVAL" appears in file managers and `lsblk` output. The date is packed as a FAT date field: year offset from 1980 in bits 15-9, month in bits 8-5, day in bits 4-0.

## 8.3 Names and NT Case Flags

FAT32 natively uses 8.3 filenames: 8 characters for the name, 3 for the extension, all uppercase. The classic DOS limitation.

```c
static uint8_t name_to_83(const char *name, uint8_t *out)
{
    memset(out, ' ', 11);
    const char *dot = NULL;
    for (const char *p = name; *p; p++)
        if (*p == '.') dot = p;

    int base_lower = 0, base_upper = 0;
    int ext_lower = 0, ext_upper = 0;
    int i = 0;
    const char *p = name;
    if (dot) {
        while (p < dot && i < 8) {
            if (*p >= 'a' && *p <= 'z') base_lower++;
            else if (*p >= 'A' && *p <= 'Z') base_upper++;
            out[i++] = (uint8_t)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
            p++;
        }
        p = dot + 1;
        i = 8;
        while (*p && i < 11) {
            if (*p >= 'a' && *p <= 'z') ext_lower++;
            else if (*p >= 'A' && *p <= 'Z') ext_upper++;
            out[i++] = (uint8_t)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
            p++;
        }
    } else {
        while (*p && i < 8) {
            if (*p >= 'a' && *p <= 'z') base_lower++;
            else if (*p >= 'A' && *p <= 'Z') base_upper++;
            out[i++] = (uint8_t)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
            p++;
        }
    }

    uint8_t nt_flags = 0;
    if (base_lower > 0 && base_upper == 0) nt_flags |= 0x08;
    if (ext_lower > 0 && ext_upper == 0) nt_flags |= 0x10;
    return nt_flags;
}
```

The function converts to uppercase while counting case: how many letters in the base name are lowercase vs. uppercase, and the same for the extension.

The return value — the NT case flags — is a Windows NT extension to the FAT directory entry format. It's stored in the `nt_reserved` byte (offset 12 of the directory entry):
- Bit 3 (0x08): the base name was all lowercase
- Bit 4 (0x10): the extension was all lowercase

This lets `boot.efi` display as `boot.efi` instead of `BOOT.EFI` without needing a full VFAT long filename entry. It's a space-efficient trick: one byte preserves the case for names that are otherwise valid 8.3. Windows, Linux, and UEFI firmware all honor these flags.

## VFAT Long Filenames

Some of our files don't fit in 8.3. The `tcc-headers` directory, for instance — `tcc-headers` is 11 characters with a hyphen, which can't be expressed as a valid 8.3 name. VFAT (Virtual FAT) extends FAT32 with long filename entries:

```c
static int needs_lfn(const char *name)
{
    const char *dot = NULL;
    int baselen = 0, extlen = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    if (dot) {
        baselen = (int)(dot - name);
        for (const char *p = dot + 1; *p; p++) extlen++;
    } else {
        for (const char *p = name; *p; p++) baselen++;
    }
    return baselen > 8 || extlen > 3;
}
```

If the base name exceeds 8 characters or the extension exceeds 3, we need LFN entries.

### Short Name Generation

Every LFN-bearing file still needs a valid 8.3 short name as a fallback:

```c
static void make_short_name(const char *name, uint8_t *out)
{
    memset(out, ' ', 11);
    const char *dot = NULL;
    for (const char *p = name; *p; p++)
        if (*p == '.') dot = p;

    const char *end = dot ? dot : name + strlen(name);
    int i = 0;
    for (const char *p = name; p < end && i < 6; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i++] = (uint8_t)c;
    }
    out[i++] = '~';
    out[i] = '1';

    if (dot) {
        i = 8;
        for (const char *p = dot + 1; *p && i < 11; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c -= 32;
            out[i++] = (uint8_t)c;
        }
    }
}
```

`tcc-headers` becomes `TCC-HE~1` — the first 6 characters (uppercase), a tilde, and a numeric suffix. This is the same algorithm Windows uses. The `~1` suffix guarantees uniqueness within a directory (we'd increment to `~2`, `~3`, etc. if needed, but in practice we never have collisions).

### LFN Entry Structure

Each LFN entry stores 13 UTF-16 characters of the long filename and occupies one 32-byte directory entry slot:

```c
static int build_lfn_entries(const char *name, const uint8_t *short_name,
                             struct fat32_dir_entry *lfn_out)
{
    int namelen = (int)strlen(name);
    int n = (namelen + LFN_CHARS_PER_ENTRY - 1) / LFN_CHARS_PER_ENTRY;
    uint8_t cksum = lfn_checksum(short_name);

    /* Pad name with 0xFFFF after the null terminator */
    uint16_t uname[260];
    int i;
    for (i = 0; i < namelen; i++) uname[i] = (uint16_t)(uint8_t)name[i];
    uname[i++] = 0x0000; /* null terminator */
    while (i < n * LFN_CHARS_PER_ENTRY) uname[i++] = 0xFFFF;
```

The name is first converted to UTF-16, null-terminated, then padded with `0xFFFF` to fill the last entry. This padding is required by the specification — it marks unused character positions.

```c
    /* LFN entries are stored in reverse order */
    for (int seq = n; seq >= 1; seq--) {
        uint8_t *e = (uint8_t *)&lfn_out[n - seq];
        memset(e, 0, 32);
        e[0] = (uint8_t)seq | ((seq == n) ? 0x40 : 0);
        e[11] = ATTR_LONG_NAME; /* 0x0F */
        e[13] = cksum;

        int base = (seq - 1) * LFN_CHARS_PER_ENTRY;
        /* chars 1-5 at offset 1 */
        for (int j = 0; j < 5; j++) {
            e[1 + j * 2]     = (uint8_t)(uname[base + j] & 0xFF);
            e[1 + j * 2 + 1] = (uint8_t)(uname[base + j] >> 8);
        }
        /* chars 6-11 at offset 14 */
        for (int j = 0; j < 6; j++) {
            e[14 + j * 2]     = (uint8_t)(uname[base + 5 + j] & 0xFF);
            e[14 + j * 2 + 1] = (uint8_t)(uname[base + 5 + j] >> 8);
        }
        /* chars 12-13 at offset 28 */
        for (int j = 0; j < 2; j++) {
            e[28 + j * 2]     = (uint8_t)(uname[base + 11 + j] & 0xFF);
            e[28 + j * 2 + 1] = (uint8_t)(uname[base + 11 + j] >> 8);
        }
    }
    return n;
}
```

LFN entries are stored in reverse order: entry N (marked with bit 6 of the sequence byte = 0x40) comes first in the directory, followed by N-1, N-2, ..., 1, then the short entry. This reverse ordering lets the filesystem driver know it has found the last LFN entry when it sees the 0x40 flag, and can stop scanning backwards.

The character layout within each entry is fragmented across three groups: 5 characters at offset 1, 6 characters at offset 14, and 2 characters at offset 28. This bizarre layout exists because LFN entries must look like invalid directory entries to old DOS FAT drivers — the attribute byte at offset 11 is set to `0x0F`, which is a combination of read-only, hidden, system, and volume-id flags that no real file would have. Old drivers skip entries with this attribute.

The checksum links LFN entries to their short entry:

```c
static uint8_t lfn_checksum(const uint8_t *short_name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    return sum;
}
```

A rolling checksum over the 11-byte short name. This ensures that if a short entry is modified without updating the LFN entries, the mismatch is detectable.

## Directory Operations

### Finding Files in a Directory

```c
static uint32_t find_in_dir(uint32_t dir_cluster, const char *name, int *entry_idx)
{
    uint8_t target[11];
    name_to_83(name, target);
    int use_lfn = needs_lfn(name);

    uint16_t lfn_buf[260];
    int lfn_active = 0;

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint64_t lba = cluster_to_lba(cluster);
        for (uint32_t sec = 0; sec < s_fs.spc; sec++) {
            if (read_sector((uint32_t)(lba + sec), s_dir) < 0)
                return 0;
            struct fat32_dir_entry *entries = (struct fat32_dir_entry *)s_dir;
            int count = SECTOR_SIZE / (int)sizeof(struct fat32_dir_entry);
            for (int i = 0; i < count; i++) {
                if (entries[i].name[0] == 0x00) return 0;  /* end of directory */
                if (entries[i].name[0] == 0xE5) { lfn_active = 0; continue; }  /* deleted */

                if (entries[i].attr == ATTR_LONG_NAME) {
                    /* Accumulate LFN characters */
                    uint8_t *e = (uint8_t *)&entries[i];
                    int seq = e[0] & 0x3F;
                    if (e[0] & 0x40) {
                        memset(lfn_buf, 0xFF, sizeof(lfn_buf));
                        lfn_active = 1;
                    }
                    if (lfn_active && seq >= 1 && seq <= 20)
                        extract_lfn_chars(e, lfn_buf, seq);
                    continue;
                }

                /* Regular entry — check LFN match then 8.3 match */
                int match = 0;
                if (use_lfn && lfn_active) {
                    match = 1;
                    for (int k = 0; name[k] || lfn_buf[k] != 0; k++) {
                        if (lfn_buf[k] != (uint16_t)(uint8_t)name[k]) {
                            match = 0; break;
                        }
                    }
                }
                if (!match)
                    match = (memcmp(entries[i].name, target, 11) == 0);
                lfn_active = 0;

                if (match) {
                    if (entry_idx) *entry_idx = (int)(sec * count + i);
                    return ((uint32_t)entries[i].first_cluster_hi << 16)
                         | (uint32_t)entries[i].first_cluster_lo;
                }
            }
        }
        cluster = fat_get(cluster);
    }
    return 0;
}
```

The search walks the directory's cluster chain, reading each sector and examining each 32-byte entry. It handles three entry types: deleted entries (0xE5 first byte), LFN entries (attribute 0x0F), and regular entries. LFN characters are accumulated across entries until a regular entry is found, then the accumulated name is compared against the target.

### Creating Directories

```c
static uint32_t ensure_dir(uint32_t parent_cluster, const char *name)
{
    uint32_t existing = find_in_dir(parent_cluster, name, NULL);
    if (existing >= 2) return existing;

    uint32_t new_cluster = alloc_cluster(0);
    if (new_cluster == 0) return 0;

    uint64_t lba = cluster_to_lba(new_cluster);
    zero_sectors((uint32_t)lba, s_fs.spc);

    /* . and .. entries */
    memset(s_dir, 0, SECTOR_SIZE);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry *)s_dir;

    memcpy(entries[0].name, ".          ", 11);
    entries[0].attr = ATTR_DIRECTORY;
    entries[0].first_cluster_hi = (uint16_t)(new_cluster >> 16);
    entries[0].first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

    memcpy(entries[1].name, "..         ", 11);
    entries[1].attr = ATTR_DIRECTORY;
    entries[1].first_cluster_hi = (uint16_t)(parent_cluster >> 16);
    entries[1].first_cluster_lo = (uint16_t)(parent_cluster & 0xFFFF);

    write_sector((uint32_t)lba, s_dir);
```

Every directory must contain `.` (pointing to itself) and `..` (pointing to its parent). These are required by the specification and by most filesystem tools — a directory without them is considered corrupt.

`ensure_dir` first checks if the directory already exists (via `find_in_dir`). If it does, it returns the existing cluster. If not, it creates a new one. This idempotent behavior lets the flasher call `ensure_dir` for each path component without worrying about duplicates.

### Walking Paths

```c
static uint32_t walk_path(const char *path)
{
    uint32_t cluster = 2; /* root */
    char component[64];

    while (*path) {
        while (*path == '/' || *path == '\\') path++;
        if (!*path) break;
        int len = 0;
        while (*path && *path != '/' && *path != '\\' && len < 63)
            component[len++] = *path++;
        component[len] = '\0';
        cluster = ensure_dir(cluster, component);
        if (cluster == 0) return 0;
    }
    return cluster;
}
```

`walk_path("EFI/BOOT")` splits on slashes and calls `ensure_dir` for each component: first `EFI` in the root directory, then `BOOT` inside `EFI`. Each call returns the cluster of the (possibly newly created) directory. The final cluster is where files will be written.

## The Streaming Write API

Large files can't be buffered in memory. The UEFI binary for aarch64 is over 650 KB — more than three times our available DRAM. Instead, we pre-allocate the cluster chain, then write data in chunks as the decompressor produces them:

```c
int fat32_stream_open(const char *path, uint32_t size)
{
    /* ... split path, walk dirs ... */

    /* Pre-allocate the cluster chain */
    uint32_t cluster_size = s_fs.spc * SECTOR_SIZE;
    uint32_t clusters_needed = (size > 0) ? (size + cluster_size - 1) / cluster_size : 1;

    uint32_t first = 0, prev = 0;
    for (uint32_t i = 0; i < clusters_needed; i++) {
        uint32_t cl = alloc_cluster(prev);
        if (cl == 0) return -1;
        if (i == 0) first = cl;
        prev = cl;
    }

    /* Set up stream state */
    s_stream.active = 1;
    s_stream.first_cluster = first;
    s_stream.current_cluster = first;
    /* ... */
    return 1;
}
```

Pre-allocation has two benefits: we know immediately if there's enough space (before starting the slow decompression), and the cluster chain is contiguous (sequential allocation from a fresh filesystem).

```c
int fat32_stream_write(int handle, const void *data, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    while (len > 0) {
        uint32_t space = SECTOR_SIZE - s_stream.buf_pos;
        uint32_t chunk = (len < space) ? len : space;

        memcpy(s_stream_buf + s_stream.buf_pos, src, chunk);
        s_stream.buf_pos += chunk;
        s_stream.bytes_written += chunk;
        src += chunk;
        len -= chunk;

        if (s_stream.buf_pos == SECTOR_SIZE) {
            if (stream_flush_sector() < 0)
                return -1;
        }
    }
    return 0;
}
```

Data arrives in arbitrary-sized chunks from the decompressor. The stream buffers them into a 512-byte sector buffer. When the buffer is full, `stream_flush_sector` writes it to the current position in the cluster chain:

```c
static int stream_flush_sector(void)
{
    uint64_t lba = cluster_to_lba(s_stream.current_cluster)
                 + s_stream.sector_in_cluster;
    if (write_sector((uint32_t)lba, s_stream_buf) < 0)
        return -1;

    memset(s_stream_buf, 0, SECTOR_SIZE);
    s_stream.buf_pos = 0;
    s_stream.sector_in_cluster++;

    if (s_stream.sector_in_cluster >= s_fs.spc) {
        s_stream.sector_in_cluster = 0;
        uint32_t next = fat_get(s_stream.current_cluster);
        if (next >= 2 && next < FAT32_EOC)
            s_stream.current_cluster = next;
    }
    return 0;
}
```

When all sectors in a cluster are written, the stream follows the FAT chain to the next cluster (which was pre-allocated during `stream_open`).

```c
int fat32_stream_close(int handle)
{
    if (s_stream.buf_pos > 0) {
        if (stream_flush_sector() < 0)
            return -1;
    }

    struct fat32_dir_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.nt_reserved = name_to_83(s_stream.filename, entry.name);
    entry.attr = ATTR_ARCHIVE;
    entry.first_cluster_hi = (uint16_t)(s_stream.first_cluster >> 16);
    entry.first_cluster_lo = (uint16_t)(s_stream.first_cluster & 0xFFFF);
    entry.file_size = s_stream.total_size;

    s_stream.active = 0;
    return add_named_entry(s_stream.dir_cluster, s_stream.filename, &entry);
}
```

Close flushes any remaining bytes (the last partial sector, zero-padded), then creates the directory entry. The file is now visible on the filesystem.

## What We Have

```
fat32.c   893 lines   FAT32 formatter, directory ops, LFN support, streaming writes
fat32.h    49 lines   Interface: format, mkdir, write_file, stream_open/write/close
```

893 lines — the largest file in the flasher, and for good reason. Building a FAT32 filesystem from scratch means implementing:

- Boot sector with the correct BPB layout
- FSInfo sector with signature validation
- Dual FAT copies, fully zeroed before use
- Cluster allocation with sequential optimization
- 8.3 name conversion with NT case flags
- VFAT long filename entries with checksums and reverse ordering
- Directory creation with `.` and `..` entries
- Path walking and automatic directory creation
- File writing for both in-memory and streaming data

This is the core of the flasher — the code that creates the filesystem that UEFI firmware reads. Every field, every magic number, every byte offset matters. Get the BPB wrong and the card won't boot. Get the FAT entries wrong and files are corrupted. Get the LFN checksum wrong and filenames appear as `TCC-HE~1` instead of `tcc-headers`.

Next: packing the workstation payload into the ESP32's flash.
