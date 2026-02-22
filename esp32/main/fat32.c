/*
 * fat32.c — FAT32 filesystem creation on an SD card partition
 *
 * Ported from src/fat32.c (Part 1 bare-metal UEFI workstation).
 * Changes from the original:
 *   - UINT8/UINT16/UINT32/UINT64 → uint8_t/uint16_t/uint32_t/uint64_t
 *   - mem_set → memset, memcpy stays
 *   - disk_read_blocks/disk_write_blocks → sdcard_read/sdcard_write
 *   - All LBAs offset by part_start (GPT partition, not superfloppy)
 *   - Added streaming write for decompressing large files chunk by chunk
 */

#include "fat32.h"
#include "sdcard.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "fat32";

/* FAT32 constants */
#define SECTOR_SIZE       512
#define RESERVED_SECTORS  32
#define NUM_FATS          2
#define FAT32_EOC        0x0FFFFFF8
#define FAT32_FREE       0x00000000
#define MIN_FAT32_CLUSTERS 65525

/* On-disk structures (packed) */
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

struct fat32_dir_entry {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
};

struct fat32_fsinfo {
    uint32_t lead_sig;
    uint8_t  reserved1[480];
    uint32_t struct_sig;
    uint32_t free_count;
    uint32_t next_free;
    uint8_t  reserved2[12];
    uint32_t trail_sig;
};

#pragma pack()

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20

/* Internal state */
static struct {
    uint32_t part_start;
    uint32_t fat_start;       /* absolute LBA */
    uint32_t fat_sectors;
    uint32_t data_start;      /* absolute LBA */
    uint32_t total_clusters;
    uint32_t next_free_cluster;
    uint32_t spc;
} s_fs;

/* Shared sector buffers — static for DMA compatibility with SD SPI.
 * s_buf: used by fat_set/fat_get/format (low-level FAT operations)
 * s_dir: used by directory operations (find_in_dir, add_dir_entry, ensure_dir)
 * s_zero: 8-sector zero buffer for bulk zeroing (FAT tables, clusters)
 * Two working buffers needed because dir ops call fat_get/fat_set internally. */
static uint8_t __attribute__((aligned(4))) s_buf[SECTOR_SIZE];
static uint8_t __attribute__((aligned(4))) s_dir[SECTOR_SIZE];

#define ZERO_BATCH 128  /* sectors per bulk zero write (64KB) */
static uint8_t __attribute__((aligned(4))) s_zero[SECTOR_SIZE * ZERO_BATCH]; /* BSS = already zero */

/* Sector I/O — all LBAs are absolute (partition offset already added) */
static int write_sector(uint32_t lba, const void *buf)
{
    return sdcard_write(lba, 1, buf);
}

static int read_sector(uint32_t lba, void *buf)
{
    return sdcard_read(lba, 1, buf);
}

/* Zero a range of sectors efficiently using batch writes.
 * If progress is non-NULL, calls it with cumulative sectors zeroed vs total_for_progress. */
static int zero_sectors_ex(uint32_t lba, uint32_t count,
                           fat32_progress_cb progress, int *done, int total_for_progress)
{
    while (count > 0) {
        uint32_t batch = (count > ZERO_BATCH) ? ZERO_BATCH : count;
        if (sdcard_write(lba, batch, s_zero) != 0)
            return -1;
        lba += batch;
        count -= batch;
        if (progress && done) {
            *done += (int)batch;
            progress(*done, total_for_progress);
        }
    }
    return 0;
}

static int zero_sectors(uint32_t lba, uint32_t count)
{
    return zero_sectors_ex(lba, count, NULL, NULL, 0);
}

/* ---- FAT table operations ---- */

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

static uint32_t fat_get(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_offset / SECTOR_SIZE;
    uint32_t fat_entry_offset = fat_offset % SECTOR_SIZE;

    if (read_sector(s_fs.fat_start + fat_sector, s_buf) < 0)
        return FAT32_EOC;

    uint32_t *entry = (uint32_t *)(s_buf + fat_entry_offset);
    return *entry & 0x0FFFFFFF;
}

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

static uint64_t cluster_to_lba(uint32_t cluster)
{
    return (uint64_t)s_fs.data_start +
           (uint64_t)(cluster - 2) * (uint64_t)s_fs.spc;
}

/* ---- Format ---- */

int fat32_format(uint32_t partition_start_lba, uint32_t partition_sectors,
                 fat32_progress_cb progress)
{
    s_fs.part_start = partition_start_lba;

    if (partition_sectors <= RESERVED_SECTORS + 100)
        return -1;

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

    uint32_t est_data = partition_sectors - RESERVED_SECTORS;
    uint32_t est_clusters = est_data / spc;
    s_fs.fat_sectors = (est_clusters * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;

    s_fs.fat_start = s_fs.part_start + RESERVED_SECTORS;
    s_fs.data_start = s_fs.fat_start + s_fs.fat_sectors * NUM_FATS;
    s_fs.total_clusters = (partition_sectors - RESERVED_SECTORS - s_fs.fat_sectors * NUM_FATS)
                          / spc;
    s_fs.next_free_cluster = 3;

    ESP_LOGI(TAG, "Formatting: %lu sectors, spc=%lu, clusters=%lu",
             (unsigned long)partition_sectors, (unsigned long)spc,
             (unsigned long)s_fs.total_clusters);

    /* Zero reserved sectors */
    if (zero_sectors(s_fs.part_start, RESERVED_SECTORS) < 0)
        return -1;

    /* Write BPB */
    memset(s_buf, 0, SECTOR_SIZE);
    struct fat32_bpb *bpb = (struct fat32_bpb *)s_buf;
    bpb->jmp[0] = 0xEB; bpb->jmp[1] = 0x58; bpb->jmp[2] = 0x90;
    memcpy(bpb->oem, "SURVIVAL", 8);
    bpb->bytes_per_sector = SECTOR_SIZE;
    bpb->sectors_per_cluster = (uint8_t)spc;
    bpb->reserved_sectors = RESERVED_SECTORS;
    bpb->num_fats = NUM_FATS;
    bpb->media_type = 0xF8;
    bpb->sectors_per_track = 63;
    bpb->num_heads = 255;
    bpb->hidden_sectors = partition_start_lba;
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

    if (write_sector(s_fs.part_start, s_buf) < 0)
        return -1;
    if (write_sector(s_fs.part_start + 6, s_buf) < 0)
        return -1;

    /* Write FSInfo */
    memset(s_buf, 0, SECTOR_SIZE);
    struct fat32_fsinfo *fsi = (struct fat32_fsinfo *)s_buf;
    fsi->lead_sig = 0x41615252;
    fsi->struct_sig = 0x61417272;
    fsi->free_count = s_fs.total_clusters - 1;
    fsi->next_free = 3;
    fsi->trail_sig = 0xAA550000;
    if (write_sector(s_fs.part_start + 1, s_buf) < 0)
        return -1;
    /* Backup FSInfo at backup_boot_sector + 1 */
    if (write_sector(s_fs.part_start + 7, s_buf) < 0)
        return -1;

    /* Zero both FAT copies completely. Garbage in unzeroed FAT sectors
     * (from previous card contents) appears as corrupted cluster entries,
     * causing fsck.fat to reject the filesystem on Linux. */
    int fat_total = (int)(s_fs.fat_sectors * NUM_FATS);
    int fat_done = 0;
    ESP_LOGI(TAG, "Zeroing FATs: %lu sectors per copy",
             (unsigned long)s_fs.fat_sectors);
    if (zero_sectors_ex(s_fs.fat_start, s_fs.fat_sectors,
                        progress, &fat_done, fat_total) < 0)
        return -1;
    if (zero_sectors_ex(s_fs.fat_start + s_fs.fat_sectors, s_fs.fat_sectors,
                        progress, &fat_done, fat_total) < 0)
        return -1;

    /* Set initial FAT entries */
    fat_set(0, 0x0FFFFFF8);
    fat_set(1, 0x0FFFFFFF);
    fat_set(2, FAT32_EOC);

    /* Zero root directory cluster */
    uint64_t root_lba = cluster_to_lba(2);
    if (zero_sectors((uint32_t)root_lba, spc) < 0)
        return -1;

    /* Volume label entry */
    memset(s_buf, 0, SECTOR_SIZE);
    struct fat32_dir_entry *de = (struct fat32_dir_entry *)s_buf;
    memcpy(de->name, "SURVIVAL   ", 11);
    de->attr = ATTR_VOLUME_ID;
    de->modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;
    if (write_sector((uint32_t)root_lba, s_buf) < 0)
        return -1;

    ESP_LOGI(TAG, "Format complete");
    return 0;
}

/* ---- Name conversion ---- */

/* Convert name to 8.3 format (uppercase) and return NT case flags.
 * Bit 3 (0x08): base name was all lowercase
 * Bit 4 (0x10): extension was all lowercase */
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

/* Convert 8.3 directory entry name back to readable string.
 * Uses nt_reserved flags to restore original case:
 *   bit 3 (0x08) = base name was all lowercase
 *   bit 4 (0x10) = extension was all lowercase */
static void name_from_83(const uint8_t *raw, uint8_t nt_flags, char *out)
{
    int pos = 0;
    /* Base name: up to 8 chars, trim trailing spaces */
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = (char)raw[i];
        if ((nt_flags & 0x08) && c >= 'A' && c <= 'Z') c += 32;
        out[pos++] = c;
    }
    /* Extension: up to 3 chars, trim trailing spaces */
    if (raw[8] != ' ') {
        out[pos++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            char c = (char)raw[i];
            if ((nt_flags & 0x10) && c >= 'A' && c <= 'Z') c += 32;
            out[pos++] = c;
        }
    }
    out[pos] = '\0';
}

/* ---- VFAT Long Filename (LFN) support ---- */

#define ATTR_LONG_NAME 0x0F
#define LFN_CHARS_PER_ENTRY 13

/* Check if a name requires LFN (doesn't fit in 8.3) */
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

/* Generate 8.3 short name with ~1 numeric tail for LFN entries */
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

/* Compute LFN checksum from 8.3 short name */
static uint8_t lfn_checksum(const uint8_t *short_name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    return sum;
}

/* Build LFN directory entries for a name. Returns number of LFN entries.
 * lfn_out must have room for ceil(namelen/13) entries (max 20). */
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

    /* LFN entries are stored in reverse order */
    for (int seq = n; seq >= 1; seq--) {
        uint8_t *e = (uint8_t *)&lfn_out[n - seq];
        memset(e, 0, 32);
        e[0] = (uint8_t)seq | ((seq == n) ? 0x40 : 0); /* order + last flag */
        e[11] = ATTR_LONG_NAME; /* attr */
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

/* ---- Directory operations ---- */

/* Extract LFN characters from a single LFN entry into a buffer at the right offset */
static void extract_lfn_chars(const uint8_t *e, uint16_t *uname, int seq)
{
    int base = (seq - 1) * LFN_CHARS_PER_ENTRY;
    for (int j = 0; j < 5; j++)
        uname[base + j] = (uint16_t)(e[1 + j * 2] | (e[1 + j * 2 + 1] << 8));
    for (int j = 0; j < 6; j++)
        uname[base + 5 + j] = (uint16_t)(e[14 + j * 2] | (e[14 + j * 2 + 1] << 8));
    for (int j = 0; j < 2; j++)
        uname[base + 11 + j] = (uint16_t)(e[28 + j * 2] | (e[28 + j * 2 + 1] << 8));
}

static uint32_t find_in_dir(uint32_t dir_cluster, const char *name, int *entry_idx)
{
    uint8_t target[11];
    name_to_83(name, target);
    int use_lfn = needs_lfn(name);

    /* For LFN matching: accumulate name across LFN entries */
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
                if (entries[i].name[0] == 0x00) return 0;
                if (entries[i].name[0] == 0xE5) { lfn_active = 0; continue; }

                if (entries[i].attr == ATTR_LONG_NAME) {
                    uint8_t *e = (uint8_t *)&entries[i];
                    int seq = e[0] & 0x3F;
                    if (e[0] & 0x40) { /* first (last in order) LFN entry */
                        memset(lfn_buf, 0xFF, sizeof(lfn_buf));
                        lfn_active = 1;
                    }
                    if (lfn_active && seq >= 1 && seq <= 20)
                        extract_lfn_chars(e, lfn_buf, seq);
                    continue;
                }

                if (entries[i].attr == ATTR_VOLUME_ID) { lfn_active = 0; continue; }

                /* Regular entry — check LFN match first, then 8.3 */
                int match = 0;
                if (use_lfn && lfn_active) {
                    /* Compare accumulated LFN with target name */
                    match = 1;
                    for (int k = 0; name[k] || lfn_buf[k] != 0; k++) {
                        if (lfn_buf[k] != (uint16_t)(uint8_t)name[k]) { match = 0; break; }
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

/* Write N consecutive directory entries to a directory cluster chain.
 * Used for LFN entries + short entry. */
static int add_dir_entries(uint32_t dir_cluster, struct fat32_dir_entry *entries_arr, int n)
{
    int written = 0;
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint64_t lba = cluster_to_lba(cluster);
        for (uint32_t sec = 0; sec < s_fs.spc && written < n; sec++) {
            if (read_sector((uint32_t)(lba + sec), s_dir) < 0)
                return -1;
            struct fat32_dir_entry *de = (struct fat32_dir_entry *)s_dir;
            int count = SECTOR_SIZE / (int)sizeof(struct fat32_dir_entry);
            int dirty = 0;
            for (int i = 0; i < count && written < n; i++) {
                if (de[i].name[0] == 0x00 || de[i].name[0] == 0xE5) {
                    memcpy(&de[i], &entries_arr[written++], sizeof(struct fat32_dir_entry));
                    dirty = 1;
                }
            }
            if (dirty) {
                if (write_sector((uint32_t)(lba + sec), s_dir) < 0)
                    return -1;
            }
        }
        if (written >= n) return 0;
        uint32_t next = fat_get(cluster);
        if (next >= FAT32_EOC) {
            uint32_t new_cl = alloc_cluster(cluster);
            if (new_cl == 0) return -1;
            uint64_t new_lba = cluster_to_lba(new_cl);
            zero_sectors((uint32_t)new_lba, s_fs.spc);
            cluster = new_cl;
            continue;
        }
        cluster = next;
    }
    return (written >= n) ? 0 : -1;
}

/* Add a named directory entry, with LFN if needed */
static int add_named_entry(uint32_t dir_cluster, const char *name,
                           struct fat32_dir_entry *short_entry)
{
    if (!needs_lfn(name)) {
        return add_dir_entries(dir_cluster, short_entry, 1);
    }

    /* Generate 8.3 short name with ~1 tail */
    make_short_name(name, short_entry->name);
    short_entry->nt_reserved = 0; /* no case flags for LFN entries */

    /* Build LFN entries + short entry into a buffer */
    struct fat32_dir_entry buf[21]; /* max 20 LFN + 1 short */
    int lfn_count = build_lfn_entries(name, short_entry->name, buf);
    memcpy(&buf[lfn_count], short_entry, sizeof(struct fat32_dir_entry));

    return add_dir_entries(dir_cluster, buf, lfn_count + 1);
}

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
    entries[0].modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;

    memcpy(entries[1].name, "..         ", 11);
    entries[1].attr = ATTR_DIRECTORY;
    entries[1].first_cluster_hi = (uint16_t)(parent_cluster >> 16);
    entries[1].first_cluster_lo = (uint16_t)(parent_cluster & 0xFFFF);
    entries[1].modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;

    write_sector((uint32_t)lba, s_dir);

    struct fat32_dir_entry parent_entry;
    memset(&parent_entry, 0, sizeof(parent_entry));
    parent_entry.nt_reserved = name_to_83(name, parent_entry.name);
    parent_entry.attr = ATTR_DIRECTORY;
    parent_entry.first_cluster_hi = (uint16_t)(new_cluster >> 16);
    parent_entry.first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    parent_entry.modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;
    add_named_entry(parent_cluster, name, &parent_entry);

    return new_cluster;
}

/* Walk a path, creating directories along the way */
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

/* Walk a path for reading — like walk_path() but never creates directories.
 * Returns cluster number of the final directory, or 0 if not found. */
static uint32_t find_path(const char *path)
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
        cluster = find_in_dir(cluster, component, NULL);
        if (cluster < 2) return 0;
    }
    return cluster;
}

/* ---- Public API ---- */

int fat32_mkdir(const char *path)
{
    return (walk_path(path) != 0) ? 0 : -1;
}

int fat32_write_file(const char *path, const void *data, uint32_t size)
{
    /* Split into directory + filename */
    const char *last_sep = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last_sep = p;

    uint32_t dir_cluster = 2;
    const char *filename;

    if (last_sep) {
        char dirpath[256];
        int dlen = (int)(last_sep - path);
        if (dlen >= 256) dlen = 255;
        memcpy(dirpath, path, (size_t)dlen);
        dirpath[dlen] = '\0';
        dir_cluster = walk_path(dirpath);
        if (dir_cluster == 0) return -1;
        filename = last_sep + 1;
    } else {
        filename = path;
    }

    uint32_t cluster_size = s_fs.spc * SECTOR_SIZE;
    uint32_t clusters_needed = (size > 0) ? (size + cluster_size - 1) / cluster_size : 1;

    uint32_t first_cluster = 0;
    uint32_t prev_cluster = 0;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t remaining = size;

    for (uint32_t i = 0; i < clusters_needed; i++) {
        uint32_t cl = alloc_cluster(prev_cluster);
        if (cl == 0) return -1;
        if (i == 0) first_cluster = cl;

        uint64_t lba = cluster_to_lba(cl);
        uint32_t bytes_to_write = (remaining > cluster_size) ? cluster_size : remaining;

        for (uint32_t sec = 0; sec < s_fs.spc; sec++) {
            uint32_t sec_offset = sec * SECTOR_SIZE;
            if (sec_offset < bytes_to_write) {
                uint32_t n = bytes_to_write - sec_offset;
                if (n > SECTOR_SIZE) n = SECTOR_SIZE;
                memcpy(s_dir, src + sec_offset, n);
                if (n < SECTOR_SIZE) memset(s_dir + n, 0, SECTOR_SIZE - n);
            } else {
                memset(s_dir, 0, SECTOR_SIZE);
            }
            if (write_sector((uint32_t)(lba + sec), s_dir) < 0)
                return -1;
        }

        src += bytes_to_write;
        remaining -= bytes_to_write;
        prev_cluster = cl;
    }

    struct fat32_dir_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.nt_reserved = name_to_83(filename, entry.name);
    entry.attr = ATTR_ARCHIVE;
    entry.first_cluster_hi = (uint16_t)(first_cluster >> 16);
    entry.first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    entry.file_size = size;
    entry.modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;

    return add_named_entry(dir_cluster, filename, &entry);
}

/* ---- Streaming write API ---- */

/* Single stream state (only one stream open at a time) */
static uint8_t __attribute__((aligned(4))) s_stream_buf[SECTOR_SIZE]; /* DMA-safe */
static struct {
    int active;
    uint32_t first_cluster;
    uint32_t current_cluster;
    uint32_t dir_cluster;
    char filename[64];
    uint32_t total_size;
    uint32_t bytes_written;
    uint32_t buf_pos;         /* bytes buffered in s_stream_buf */
    uint32_t sector_in_cluster; /* which sector within current cluster */
} s_stream;

int fat32_stream_open(const char *path, uint32_t size)
{
    if (s_stream.active) return -1;

    /* Split path */
    const char *last_sep = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last_sep = p;

    uint32_t dir_cluster = 2;
    const char *filename;

    if (last_sep) {
        char dirpath[256];
        int dlen = (int)(last_sep - path);
        if (dlen >= 256) dlen = 255;
        memcpy(dirpath, path, (size_t)dlen);
        dirpath[dlen] = '\0';
        dir_cluster = walk_path(dirpath);
        if (dir_cluster == 0) return -1;
        filename = last_sep + 1;
    } else {
        filename = path;
    }

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
    s_stream.dir_cluster = dir_cluster;
    s_stream.total_size = size;
    s_stream.bytes_written = 0;
    s_stream.buf_pos = 0;
    s_stream.sector_in_cluster = 0;
    int flen = 0;
    while (filename[flen] && flen < 63) {
        s_stream.filename[flen] = filename[flen];
        flen++;
    }
    s_stream.filename[flen] = '\0';
    memset(s_stream_buf, 0, SECTOR_SIZE);

    return 1;
}

/* Flush the sector buffer to the current position */
static int stream_flush_sector(void)
{
    uint64_t lba = cluster_to_lba(s_stream.current_cluster)
                 + s_stream.sector_in_cluster;
    if (write_sector((uint32_t)lba, s_stream_buf) < 0)
        return -1;

    memset(s_stream_buf, 0, SECTOR_SIZE);
    s_stream.buf_pos = 0;
    s_stream.sector_in_cluster++;

    /* Move to next cluster if needed */
    if (s_stream.sector_in_cluster >= s_fs.spc) {
        s_stream.sector_in_cluster = 0;
        uint32_t next = fat_get(s_stream.current_cluster);
        if (next >= 2 && next < FAT32_EOC)
            s_stream.current_cluster = next;
    }
    return 0;
}

int fat32_stream_write(int handle, const void *data, uint32_t len)
{
    if (!s_stream.active || handle != 1) return -1;

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

int fat32_stream_close(int handle)
{
    if (!s_stream.active || handle != 1) return -1;

    /* Flush remaining data */
    if (s_stream.buf_pos > 0) {
        if (stream_flush_sector() < 0)
            return -1;
    }

    /* Add directory entry */
    struct fat32_dir_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.nt_reserved = name_to_83(s_stream.filename, entry.name);
    entry.attr = ATTR_ARCHIVE;
    entry.first_cluster_hi = (uint16_t)(s_stream.first_cluster >> 16);
    entry.first_cluster_lo = (uint16_t)(s_stream.first_cluster & 0xFFFF);
    entry.file_size = s_stream.total_size;
    entry.modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;

    s_stream.active = 0;
    return add_named_entry(s_stream.dir_cluster, s_stream.filename, &entry);
}

/* ---- Reading API (Chapter 36) ---- */

int fat32_read_init(uint32_t partition_start_lba)
{
    s_fs.part_start = partition_start_lba;

    /* Read BPB from first sector of partition */
    if (read_sector(partition_start_lba, s_buf) < 0)
        return -1;

    struct fat32_bpb *bpb = (struct fat32_bpb *)s_buf;
    if (bpb->signature != 0xAA55 || bpb->fat_size_32 == 0) {
        ESP_LOGE(TAG, "read_init: invalid BPB (sig=0x%04X, fat32=%lu)",
                 bpb->signature, (unsigned long)bpb->fat_size_32);
        return -1;
    }

    s_fs.spc = bpb->sectors_per_cluster;
    s_fs.fat_sectors = bpb->fat_size_32;
    s_fs.fat_start = partition_start_lba + bpb->reserved_sectors;
    s_fs.data_start = s_fs.fat_start + s_fs.fat_sectors * bpb->num_fats;
    s_fs.total_clusters = (bpb->total_sectors_32 - bpb->reserved_sectors
                           - s_fs.fat_sectors * bpb->num_fats) / s_fs.spc;
    /* Prevent accidental allocation when reading */
    s_fs.next_free_cluster = s_fs.total_clusters + 2;

    ESP_LOGI(TAG, "read_init: spc=%lu, clusters=%lu",
             (unsigned long)s_fs.spc, (unsigned long)s_fs.total_clusters);
    return 0;
}

/* ---- Directory enumeration ---- */

static struct {
    int active;
    uint32_t cluster;       /* current cluster being scanned */
    uint32_t sector;        /* sector offset within cluster */
    int entry;              /* entry index within sector */
    uint16_t lfn_buf[260];  /* accumulated LFN characters */
    int lfn_active;         /* 1 if we're collecting LFN entries */
} s_readdir;

int fat32_open_dir(const char *path)
{
    if (s_readdir.active) return -1;

    uint32_t cluster;
    if (!path || !path[0] || (path[0] == '/' && !path[1]))
        cluster = 2; /* root */
    else
        cluster = find_path(path);

    if (cluster < 2) return -1;

    s_readdir.active = 1;
    s_readdir.cluster = cluster;
    s_readdir.sector = 0;
    s_readdir.entry = 0;
    s_readdir.lfn_active = 0;
    return 1;
}

int fat32_read_dir(int handle, struct fat32_dir_info *info)
{
    if (!s_readdir.active || handle != 1) return -1;

    int entries_per_sector = SECTOR_SIZE / (int)sizeof(struct fat32_dir_entry);

    while (s_readdir.cluster >= 2 && s_readdir.cluster < FAT32_EOC) {
        uint64_t base_lba = cluster_to_lba(s_readdir.cluster);

        for (; s_readdir.sector < s_fs.spc; s_readdir.sector++) {
            if (read_sector((uint32_t)(base_lba + s_readdir.sector), s_dir) < 0)
                return -1;

            struct fat32_dir_entry *entries = (struct fat32_dir_entry *)s_dir;

            for (; s_readdir.entry < entries_per_sector; s_readdir.entry++) {
                struct fat32_dir_entry *de = &entries[s_readdir.entry];

                if (de->name[0] == 0x00) {
                    /* End of directory */
                    s_readdir.active = 0;
                    return 0;
                }
                if (de->name[0] == 0xE5) {
                    s_readdir.lfn_active = 0;
                    continue;
                }

                if (de->attr == ATTR_LONG_NAME) {
                    uint8_t *e = (uint8_t *)de;
                    int seq = e[0] & 0x3F;
                    if (e[0] & 0x40) {
                        memset(s_readdir.lfn_buf, 0, sizeof(s_readdir.lfn_buf));
                        s_readdir.lfn_active = 1;
                    }
                    if (s_readdir.lfn_active && seq >= 1 && seq <= 20)
                        extract_lfn_chars(e, s_readdir.lfn_buf, seq);
                    continue;
                }

                /* Skip volume label */
                if (de->attr & ATTR_VOLUME_ID) {
                    s_readdir.lfn_active = 0;
                    continue;
                }

                /* Skip . and .. */
                if (de->name[0] == '.' &&
                    (de->name[1] == ' ' || de->name[1] == '.')) {
                    s_readdir.lfn_active = 0;
                    continue;
                }

                /* Real entry — fill info */
                if (s_readdir.lfn_active) {
                    /* Convert UCS-2 LFN to ASCII */
                    int k = 0;
                    while (k < 127 && s_readdir.lfn_buf[k] != 0)  {
                        info->name[k] = (char)(s_readdir.lfn_buf[k] & 0xFF);
                        k++;
                    }
                    info->name[k] = '\0';
                } else {
                    name_from_83(de->name, de->nt_reserved, info->name);
                }
                s_readdir.lfn_active = 0;

                info->size = de->file_size;
                info->is_dir = (de->attr & ATTR_DIRECTORY) ? 1 : 0;

                /* Advance cursor past this entry for next call */
                s_readdir.entry++;
                return 1;
            }
            s_readdir.entry = 0;
        }
        /* Move to next cluster in directory chain */
        s_readdir.sector = 0;
        s_readdir.cluster = fat_get(s_readdir.cluster);
    }

    s_readdir.active = 0;
    return 0;
}

void fat32_close_dir(int handle)
{
    if (handle == 1)
        s_readdir.active = 0;
}

/* ---- File reading ---- */

static struct {
    int active;
    uint32_t current_cluster;
    uint32_t file_size;
    uint32_t position;          /* bytes read so far */
    uint32_t sector_in_cluster; /* which sector within current cluster */
    uint32_t buf_pos;           /* read offset within s_stream_buf */
} s_readfile;

int fat32_file_open(const char *path)
{
    if (s_readfile.active) return -1;
    if (s_stream.active) return -1; /* s_stream_buf is shared */

    /* Split path into directory + filename */
    const char *last_sep = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last_sep = p;

    uint32_t dir_cluster = 2;
    const char *filename;

    if (last_sep) {
        char dirpath[256];
        int dlen = (int)(last_sep - path);
        if (dlen >= 256) dlen = 255;
        memcpy(dirpath, path, (size_t)dlen);
        dirpath[dlen] = '\0';
        dir_cluster = find_path(dirpath);
        if (dir_cluster < 2) return -1;
        filename = last_sep + 1;
    } else {
        filename = path;
    }

    int entry_idx;
    uint32_t cluster = find_in_dir(dir_cluster, filename, &entry_idx);
    if (cluster < 2) return -1;

    /* s_dir still contains the sector with our entry — extract file_size */
    struct fat32_dir_entry *entries = (struct fat32_dir_entry *)s_dir;
    int idx_in_sector = entry_idx % (SECTOR_SIZE / (int)sizeof(struct fat32_dir_entry));
    uint32_t file_size = entries[idx_in_sector].file_size;

    s_readfile.active = 1;
    s_readfile.current_cluster = cluster;
    s_readfile.file_size = file_size;
    s_readfile.position = 0;
    s_readfile.sector_in_cluster = 0;
    s_readfile.buf_pos = SECTOR_SIZE; /* force load on first read */
    return 1;
}

int fat32_file_read(int handle, void *buf, uint32_t len)
{
    if (!s_readfile.active || handle != 1) return -1;

    /* Clamp to remaining bytes */
    uint32_t remaining = s_readfile.file_size - s_readfile.position;
    if (remaining == 0) return 0;
    if (len > remaining) len = remaining;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t copied = 0;

    while (copied < len) {
        /* Load next sector if buffer exhausted */
        if (s_readfile.buf_pos >= SECTOR_SIZE) {
            /* Advance to next cluster if needed */
            if (s_readfile.sector_in_cluster >= s_fs.spc) {
                uint32_t next = fat_get(s_readfile.current_cluster);
                if (next < 2 || next >= FAT32_EOC) {
                    s_readfile.active = 0;
                    return (copied > 0) ? (int)copied : 0;
                }
                s_readfile.current_cluster = next;
                s_readfile.sector_in_cluster = 0;
            }

            uint64_t lba = cluster_to_lba(s_readfile.current_cluster)
                         + s_readfile.sector_in_cluster;
            if (read_sector((uint32_t)lba, s_stream_buf) < 0)
                return -1;

            s_readfile.sector_in_cluster++;
            s_readfile.buf_pos = 0;
        }

        uint32_t avail = SECTOR_SIZE - s_readfile.buf_pos;
        uint32_t want = len - copied;
        uint32_t chunk = (want < avail) ? want : avail;

        memcpy(dst + copied, s_stream_buf + s_readfile.buf_pos, chunk);
        s_readfile.buf_pos += chunk;
        s_readfile.position += chunk;
        copied += chunk;
    }

    return (int)copied;
}

void fat32_file_close(int handle)
{
    if (handle == 1)
        s_readfile.active = 0;
}

int fat32_volume_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    uint32_t cluster_size = (uint32_t)s_fs.spc * SECTOR_SIZE;
    *total_bytes = (uint64_t)s_fs.total_clusters * cluster_size;

    /* Count free clusters by scanning the FAT */
    uint32_t free_count = 0;
    uint32_t entries_per_sector = SECTOR_SIZE / 4;
    uint32_t cl = 2;  /* clusters start at 2 */

    for (uint32_t sec = 0; sec < s_fs.fat_sectors && cl < s_fs.total_clusters + 2; sec++) {
        if (read_sector(s_fs.fat_start + sec, s_buf) < 0)
            return -1;
        uint32_t *fat = (uint32_t *)s_buf;
        uint32_t start = (sec == 0) ? 2 : 0;  /* skip entries 0 and 1 */
        for (uint32_t i = start; i < entries_per_sector && cl < s_fs.total_clusters + 2; i++, cl++) {
            if ((fat[i] & 0x0FFFFFFF) == 0)
                free_count++;
        }
    }

    *free_bytes = (uint64_t)free_count * cluster_size;
    return 0;
}
