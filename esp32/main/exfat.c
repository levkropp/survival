/*
 * exfat.c — exFAT filesystem driver (read/write)
 *
 * Ported from src/exfat.c (Part 1 bare-metal UEFI workstation).
 * Changes from the original:
 *   - UINT8/UINT16/UINT32/UINT64 → uint8_t/uint16_t/uint32_t/uint64_t
 *   - UINTN → size_t, CHAR8 → char
 *   - mem_alloc/mem_free → malloc/free
 *   - mem_set/mem_copy → memset/memcpy
 *   - str_len → strlen, str_copy → local str_copy
 *   - struct fs_entry → struct exfat_dir_info, FS_MAX_NAME → EXFAT_MAX_NAME
 *   - Added exfat_mount_sdcard() for ESP32 SD card access
 *   - Added ESP_LOG for mount/unmount diagnostics
 *
 * exFAT on-disk layout (superfloppy — no MBR):
 *   Sector 0:           Boot sector (VBR)
 *   Sector 1-8:         Extended boot sectors
 *   Sector 9:           OEM parameters
 *   Sector 10:          Reserved
 *   Sector 11:          Boot checksum
 *   Sector 12-23:       Backup boot region (copy of 0-11)
 *   FatOffset sectors:  FAT (cluster allocation table)
 *   ClusterHeapOffset:  Data region (cluster 2 = first data cluster)
 */

#include "exfat.h"
#include "sdcard.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "exfat";

/* Safe string copy (matches Part 1 str_copy semantics) */
static void str_copy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---- Constants ---- */

#define EXFAT_EOC           0xFFFFFFFF
#define EXFAT_BAD           0xFFFFFFF7
#define EXFAT_FREE          0x00000000

#define EXFAT_CACHE_SIZE    8

/* Directory entry types */
#define ENTRY_EOD           0x00  /* end of directory */
#define ENTRY_BITMAP        0x81  /* allocation bitmap */
#define ENTRY_UPCASE        0x82  /* up-case table */
#define ENTRY_VLABEL        0x83  /* volume label */
#define ENTRY_FILE          0x85  /* file directory entry */
#define ENTRY_STREAM        0xC0  /* stream extension */
#define ENTRY_NAME          0xC1  /* file name extension */

/* File attributes (same as FAT) */
#define ATTR_READ_ONLY      0x01
#define ATTR_HIDDEN         0x02
#define ATTR_SYSTEM         0x04
#define ATTR_DIRECTORY      0x10
#define ATTR_ARCHIVE        0x20

/* Stream extension flags */
#define STREAM_ALLOC_POSSIBLE  0x01
#define STREAM_NO_FAT_CHAIN    0x02

/* ---- On-disk structures ---- */

#pragma pack(1)

struct exfat_boot_sector {
    uint8_t  jump_boot[3];         /* 0:   EB 76 90 */
    uint8_t  fs_name[8];           /* 3:   "EXFAT   " */
    uint8_t  must_be_zero[53];     /* 11:  zeros */
    uint64_t partition_offset;     /* 64:  sectors */
    uint64_t volume_length;        /* 72:  sectors */
    uint32_t fat_offset;           /* 80:  sectors from vol start */
    uint32_t fat_length;           /* 84:  sectors */
    uint32_t cluster_heap_offset;  /* 88:  sectors from vol start */
    uint32_t cluster_count;        /* 92:  total data clusters */
    uint32_t root_cluster;         /* 96:  first cluster of root dir */
    uint32_t volume_serial;        /* 100: serial number */
    uint16_t fs_revision;          /* 104: expect 0x0100 */
    uint16_t volume_flags;         /* 106: */
    uint8_t  bytes_per_sector_shift;    /* 108: log2 */
    uint8_t  sectors_per_cluster_shift; /* 109: log2 */
    uint8_t  number_of_fats;       /* 110: usually 1 */
    uint8_t  drive_select;         /* 111: */
    uint8_t  percent_in_use;       /* 112: */
    uint8_t  reserved[7];          /* 113: */
    uint8_t  boot_code[390];       /* 120: */
    uint16_t boot_signature;       /* 510: 0xAA55 */
};

/* Generic 32-byte directory entry (for reading entry type) */
struct exfat_dentry {
    uint8_t  type;
    uint8_t  data[31];
};

/* File Directory Entry (type 0x85) */
struct exfat_file_dentry {
    uint8_t  type;                 /* 0x85 */
    uint8_t  secondary_count;      /* count of stream + name entries */
    uint16_t set_checksum;
    uint16_t file_attributes;
    uint16_t reserved1;
    uint32_t create_timestamp;
    uint32_t modify_timestamp;
    uint32_t access_timestamp;
    uint8_t  create_10ms;
    uint8_t  modify_10ms;
    uint8_t  create_tz_offset;
    uint8_t  modify_tz_offset;
    uint8_t  access_tz_offset;
    uint8_t  reserved2[7];
};

/* Stream Extension Entry (type 0xC0) */
struct exfat_stream_dentry {
    uint8_t  type;                 /* 0xC0 */
    uint8_t  flags;                /* bit 0: alloc possible, bit 1: no FAT chain */
    uint8_t  reserved1;
    uint8_t  name_length;          /* in Unicode chars */
    uint16_t name_hash;
    uint16_t reserved2;
    uint64_t valid_data_length;
    uint32_t reserved3;
    uint32_t first_cluster;
    uint64_t data_length;
};

/* File Name Extension Entry (type 0xC1) */
struct exfat_name_dentry {
    uint8_t  type;                 /* 0xC1 */
    uint8_t  flags;
    uint16_t name[15];             /* UTF-16LE, 15 chars per entry */
};

/* Allocation Bitmap Entry (type 0x81) */
struct exfat_bitmap_dentry {
    uint8_t  type;                 /* 0x81 */
    uint8_t  bitmap_flags;         /* bit 0: 0=first bitmap, 1=second */
    uint8_t  reserved[18];
    uint32_t first_cluster;
    uint64_t data_length;
};

/* Volume Label Entry (type 0x83) */
struct exfat_label_dentry {
    uint8_t  type;                 /* 0x83 */
    uint8_t  char_count;           /* number of UTF-16 chars (max 11) */
    uint16_t label[11];            /* UTF-16LE volume label */
    uint8_t  reserved[8];
};

#pragma pack()

/* ---- Sector cache ---- */

struct exfat_cache_entry {
    uint64_t sector;
    uint8_t  *data;
    int      valid;
    int      dirty;
};

/* ---- Volume handle ---- */

struct exfat_vol {
    exfat_block_read_fn  read_fn;
    exfat_block_write_fn write_fn;
    void                *ctx;

    uint32_t dev_block_size;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_count;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t root_cluster;
    uint64_t volume_length;

    /* Allocation bitmap */
    uint8_t  *bitmap;
    uint32_t bitmap_size;          /* in bytes */
    uint32_t bitmap_cluster;
    int      bitmap_no_fat_chain;

    /* Volume label (ASCII) */
    char     label[48];

    /* Sector cache */
    struct exfat_cache_entry cache[EXFAT_CACHE_SIZE];
    uint32_t cache_clock;          /* simple clock for LRU eviction */
};

/* ---- Internal helpers: ASCII case conversion ---- */

static char to_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static int ascii_icmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = to_lower(*a);
        char cb = to_lower(*b);
        if (ca != cb)
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ---- Sector cache implementation ---- */

static void cache_init(struct exfat_vol *vol)
{
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        vol->cache[i].data = (uint8_t *)malloc(vol->bytes_per_sector);
        vol->cache[i].valid = 0;
        vol->cache[i].dirty = 0;
        vol->cache[i].sector = 0;
    }
    vol->cache_clock = 0;
}

static void cache_free(struct exfat_vol *vol)
{
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        if (vol->cache[i].data) {
            free(vol->cache[i].data);
            vol->cache[i].data = NULL;
        }
    }
}

/*
 * Flush a single dirty cache entry to disk.
 * We must translate exFAT sectors to device blocks.
 */
static int cache_flush_entry(struct exfat_vol *vol, int idx)
{
    struct exfat_cache_entry *ce = &vol->cache[idx];
    if (!ce->valid || !ce->dirty)
        return 0;

    uint32_t ratio = vol->bytes_per_sector / vol->dev_block_size;
    uint64_t dev_lba = ce->sector * ratio;

    if (vol->write_fn(vol->ctx, dev_lba, ratio, ce->data) != 0)
        return -1;

    ce->dirty = 0;
    return 0;
}

/* Flush all dirty cache entries */
static int cache_flush_all(struct exfat_vol *vol)
{
    int ret = 0;
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        if (vol->cache[i].valid && vol->cache[i].dirty) {
            if (cache_flush_entry(vol, i) != 0)
                ret = -1;
        }
    }
    return ret;
}

/* Find a cache slot (evicting LRU if needed) */
static int cache_find_slot(struct exfat_vol *vol)
{
    /* Look for an invalid entry first */
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        if (!vol->cache[i].valid)
            return i;
    }
    /* Evict using round-robin (simple clock) */
    int idx = (int)(vol->cache_clock % EXFAT_CACHE_SIZE);
    vol->cache_clock++;
    /* Flush if dirty */
    if (vol->cache[idx].dirty)
        cache_flush_entry(vol, idx);
    vol->cache[idx].valid = 0;
    return idx;
}

/*
 * Read a sector (in exFAT sector units) through the cache.
 * Returns pointer to cached buffer, or NULL on error.
 * The pointer is valid until the next cache operation.
 */
static uint8_t *cache_read(struct exfat_vol *vol, uint64_t sector)
{
    /* Check if already cached */
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        if (vol->cache[i].valid && vol->cache[i].sector == sector)
            return vol->cache[i].data;
    }

    /* Need to read from disk */
    int idx = cache_find_slot(vol);
    struct exfat_cache_entry *ce = &vol->cache[idx];

    uint32_t ratio = vol->bytes_per_sector / vol->dev_block_size;
    uint64_t dev_lba = sector * ratio;

    if (vol->read_fn(vol->ctx, dev_lba, ratio, ce->data) != 0)
        return NULL;

    ce->sector = sector;
    ce->valid = 1;
    ce->dirty = 0;
    return ce->data;
}

/*
 * Mark a cached sector as dirty (it will be written on flush/evict).
 * The sector must already be in cache (caller just called cache_read).
 */
static void cache_mark_dirty(struct exfat_vol *vol, uint64_t sector)
{
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        if (vol->cache[i].valid && vol->cache[i].sector == sector) {
            vol->cache[i].dirty = 1;
            return;
        }
    }
}

/* ---- Read/write raw sectors (bypassing cache, for bulk I/O) ---- */

static int read_sectors_raw(struct exfat_vol *vol, uint64_t exfat_sector,
                            uint32_t count, void *buf)
{
    uint32_t ratio = vol->bytes_per_sector / vol->dev_block_size;
    uint64_t dev_lba = exfat_sector * ratio;
    return vol->read_fn(vol->ctx, dev_lba, count * ratio, buf);
}

static int write_sectors_raw(struct exfat_vol *vol, uint64_t exfat_sector,
                             uint32_t count, const void *buf)
{
    uint32_t ratio = vol->bytes_per_sector / vol->dev_block_size;
    uint64_t dev_lba = exfat_sector * ratio;
    return vol->write_fn(vol->ctx, dev_lba, count * ratio, buf);
}

/* ---- Cluster addressing ---- */

static uint64_t cluster_to_sector(struct exfat_vol *vol, uint32_t cluster)
{
    return (uint64_t)vol->cluster_heap_offset +
           (uint64_t)(cluster - 2) * (uint64_t)vol->sectors_per_cluster;
}

static uint32_t cluster_size(struct exfat_vol *vol)
{
    return vol->sectors_per_cluster * vol->bytes_per_sector;
}

/* ---- FAT operations ---- */

static uint32_t fat_get(struct exfat_vol *vol, uint32_t cluster)
{
    if (cluster < 2 || cluster >= vol->cluster_count + 2)
        return EXFAT_EOC;

    uint64_t byte_off = (uint64_t)cluster * 4;
    uint64_t sector = (uint64_t)vol->fat_offset + byte_off / vol->bytes_per_sector;
    uint32_t offset = (uint32_t)(byte_off % vol->bytes_per_sector);

    uint8_t *buf = cache_read(vol, sector);
    if (!buf)
        return EXFAT_EOC;

    uint32_t val;
    memcpy(&val, buf + offset, 4);
    return val;
}

static int fat_set(struct exfat_vol *vol, uint32_t cluster, uint32_t value)
{
    if (cluster < 2 || cluster >= vol->cluster_count + 2)
        return -1;

    uint64_t byte_off = (uint64_t)cluster * 4;
    uint64_t sector = (uint64_t)vol->fat_offset + byte_off / vol->bytes_per_sector;
    uint32_t offset = (uint32_t)(byte_off % vol->bytes_per_sector);

    uint8_t *buf = cache_read(vol, sector);
    if (!buf)
        return -1;

    memcpy(buf + offset, &value, 4);
    cache_mark_dirty(vol, sector);
    return 0;
}

/* ---- Bitmap operations ---- */

static int bitmap_get(struct exfat_vol *vol, uint32_t cluster)
{
    if (cluster < 2)
        return 0;
    uint32_t idx = cluster - 2;
    uint32_t byte_idx = idx / 8;
    uint8_t  bit_idx  = (uint8_t)(idx % 8);
    if (byte_idx >= vol->bitmap_size)
        return 0;
    return (vol->bitmap[byte_idx] >> bit_idx) & 1;
}

static void bitmap_set(struct exfat_vol *vol, uint32_t cluster, int used)
{
    if (cluster < 2)
        return;
    uint32_t idx = cluster - 2;
    uint32_t byte_idx = idx / 8;
    uint8_t  bit_idx  = (uint8_t)(idx % 8);
    if (byte_idx >= vol->bitmap_size)
        return;
    if (used)
        vol->bitmap[byte_idx] |= (uint8_t)(1 << bit_idx);
    else
        vol->bitmap[byte_idx] &= (uint8_t)~(1 << bit_idx);
}

/* Write the allocation bitmap back to disk */
static int bitmap_flush(struct exfat_vol *vol)
{
    uint32_t bps = vol->bytes_per_sector;
    uint32_t spc = vol->sectors_per_cluster;
    uint32_t clust_size = bps * spc;
    uint32_t total_bytes = vol->bitmap_size;
    uint32_t offset = 0;
    uint32_t cluster = vol->bitmap_cluster;

    while (offset < total_bytes && cluster >= 2 && cluster != EXFAT_EOC) {
        uint64_t sec = cluster_to_sector(vol, cluster);
        uint32_t chunk = total_bytes - offset;
        if (chunk > clust_size)
            chunk = clust_size;

        /* Write full sectors */
        uint32_t full_secs = chunk / bps;
        if (full_secs > 0) {
            if (write_sectors_raw(vol, sec, full_secs, vol->bitmap + offset) != 0)
                return -1;
        }
        /* Partial last sector */
        uint32_t partial = chunk % bps;
        if (partial > 0) {
            uint8_t *tmp = cache_read(vol, sec + full_secs);
            if (!tmp)
                return -1;
            memcpy(tmp, vol->bitmap + offset + full_secs * bps, partial);
            cache_mark_dirty(vol, sec + full_secs);
        }

        offset += clust_size;
        if (vol->bitmap_no_fat_chain)
            cluster++;
        else
            cluster = fat_get(vol, cluster);
    }

    return cache_flush_all(vol);
}

/* Read the allocation bitmap from disk */
static int bitmap_load(struct exfat_vol *vol)
{
    uint32_t bps = vol->bytes_per_sector;
    uint32_t spc = vol->sectors_per_cluster;
    uint32_t clust_size = bps * spc;
    uint32_t total_bytes = vol->bitmap_size;
    uint32_t offset = 0;
    uint32_t cluster = vol->bitmap_cluster;

    while (offset < total_bytes && cluster >= 2 && cluster != EXFAT_EOC) {
        uint64_t sec = cluster_to_sector(vol, cluster);
        uint32_t chunk = total_bytes - offset;
        if (chunk > clust_size)
            chunk = clust_size;

        /* Read full sectors */
        uint32_t full_secs = chunk / bps;
        if (full_secs > 0) {
            if (read_sectors_raw(vol, sec, full_secs, vol->bitmap + offset) != 0)
                return -1;
        }
        /* Partial last sector */
        uint32_t partial = chunk % bps;
        if (partial > 0) {
            uint8_t *tmp = cache_read(vol, sec + full_secs);
            if (!tmp)
                return -1;
            memcpy(vol->bitmap + offset + full_secs * bps, tmp, partial);
        }

        offset += clust_size;
        if (vol->bitmap_no_fat_chain)
            cluster++;
        else
            cluster = fat_get(vol, cluster);
    }

    return 0;
}

/* Allocate a free cluster. Returns 0 on failure. */
static uint32_t alloc_cluster(struct exfat_vol *vol)
{
    for (uint32_t i = 0; i < vol->cluster_count; i++) {
        uint32_t cl = i + 2;
        if (!bitmap_get(vol, cl)) {
            bitmap_set(vol, cl, 1);
            fat_set(vol, cl, EXFAT_EOC);
            return cl;
        }
    }
    return 0;
}

/* Allocate a cluster and chain it to prev */
static uint32_t alloc_cluster_chain(struct exfat_vol *vol, uint32_t prev)
{
    uint32_t cl = alloc_cluster(vol);
    if (cl == 0)
        return 0;
    if (prev >= 2)
        fat_set(vol, prev, cl);
    return cl;
}

/* Free a cluster chain starting from the given cluster */
static void free_chain(struct exfat_vol *vol, uint32_t start, int no_fat_chain,
                       uint64_t data_length)
{
    uint32_t cluster = start;
    uint32_t clsz = cluster_size(vol);
    uint64_t remaining = data_length;

    while (cluster >= 2 && cluster != EXFAT_EOC && cluster != EXFAT_BAD) {
        bitmap_set(vol, cluster, 0);
        uint32_t next;
        if (no_fat_chain) {
            /* Contiguous: sequential clusters */
            if (remaining <= clsz)
                break;
            remaining -= clsz;
            next = cluster + 1;
        } else {
            next = fat_get(vol, cluster);
            fat_set(vol, cluster, EXFAT_FREE);
        }
        cluster = next;
    }
}

/* ---- UTF-16LE <-> ASCII conversion ---- */

/* Convert UTF-16LE to ASCII (lossy: non-ASCII becomes '?') */
static void utf16_to_ascii(const uint16_t *src, int nchars, char *dst, int max)
{
    int i, j;
    for (i = 0, j = 0; i < nchars && j < max - 1; i++) {
        uint16_t ch;
        memcpy(&ch, &src[i], 2);
        if (ch == 0)
            break;
        if (ch < 128)
            dst[j++] = (char)ch;
        else
            dst[j++] = '?';
    }
    dst[j] = '\0';
}

/* ---- exFAT name hash (per specification) ---- */

static uint16_t exfat_name_hash(const uint16_t *name, int length)
{
    uint16_t hash = 0;
    for (int i = 0; i < length; i++) {
        uint16_t ch;
        memcpy(&ch, &name[i], 2);
        /* Up-case for hashing (ASCII only) */
        if (ch >= 'a' && ch <= 'z')
            ch = (uint16_t)(ch - 'a' + 'A');
        uint8_t *bytes = (uint8_t *)&ch;
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + bytes[0]);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + bytes[1]);
    }
    return hash;
}

/* ---- Entry set checksum (per specification) ---- */

static uint16_t entry_set_checksum(const uint8_t *entries, int count)
{
    uint16_t checksum = 0;
    int total_bytes = count * 32;
    for (int i = 0; i < total_bytes; i++) {
        /* Skip the checksum field itself (bytes 2-3 of first entry) */
        if (i == 2 || i == 3)
            continue;
        checksum = (uint16_t)(((checksum << 15) | (checksum >> 1)) + entries[i]);
    }
    return checksum;
}

/* ---- Directory traversal ---- */

/*
 * Context for iterating over directory entries.
 * The directory is a chain of clusters; we iterate 32-byte entries.
 */
struct dir_iter {
    struct exfat_vol *vol;
    uint32_t first_cluster;
    int      no_fat_chain;
    uint64_t data_length;

    /* Current position */
    uint32_t cur_cluster;
    uint32_t sector_in_cluster;   /* 0 .. sectors_per_cluster-1 */
    uint32_t entry_in_sector;     /* 0 .. entries_per_sector-1 */
    uint64_t byte_offset;         /* total bytes walked */
    uint8_t  *sector_buf;         /* cached from sector cache */
    uint64_t cur_sector;          /* absolute sector number */
};

static int dir_iter_init(struct dir_iter *it, struct exfat_vol *vol,
                         uint32_t cluster, int no_fat_chain, uint64_t data_length)
{
    it->vol = vol;
    it->first_cluster = cluster;
    it->no_fat_chain = no_fat_chain;
    it->data_length = data_length;
    it->cur_cluster = cluster;
    it->sector_in_cluster = 0;
    it->entry_in_sector = 0;
    it->byte_offset = 0;
    it->sector_buf = NULL;
    it->cur_sector = 0;

    if (cluster < 2)
        return -1;

    it->cur_sector = cluster_to_sector(vol, cluster);
    it->sector_buf = cache_read(vol, it->cur_sector);
    if (!it->sector_buf)
        return -1;

    return 0;
}

/* Get pointer to current 32-byte directory entry. NULL if at end. */
static struct exfat_dentry *dir_iter_get(struct dir_iter *it)
{
    if (!it->sector_buf)
        return NULL;
    if (it->data_length > 0 && it->byte_offset >= it->data_length)
        return NULL;

    uint32_t off = it->entry_in_sector * 32;
    return (struct exfat_dentry *)(it->sector_buf + off);
}

/* Get the absolute sector and byte offset of the current entry */
static uint64_t dir_iter_sector(struct dir_iter *it)
{
    return it->cur_sector;
}

static uint32_t dir_iter_offset_in_sector(struct dir_iter *it)
{
    return it->entry_in_sector * 32;
}

/* Advance to next 32-byte entry. Returns 0 on success, -1 at end. */
static int dir_iter_next(struct dir_iter *it)
{
    struct exfat_vol *vol = it->vol;
    uint32_t entries_per_sector = vol->bytes_per_sector / 32;

    it->entry_in_sector++;
    it->byte_offset += 32;

    if (it->data_length > 0 && it->byte_offset >= it->data_length)
        return -1;

    if (it->entry_in_sector >= entries_per_sector) {
        it->entry_in_sector = 0;
        it->sector_in_cluster++;

        if (it->sector_in_cluster >= vol->sectors_per_cluster) {
            /* Move to next cluster */
            it->sector_in_cluster = 0;
            if (it->no_fat_chain) {
                it->cur_cluster++;
            } else {
                it->cur_cluster = fat_get(vol, it->cur_cluster);
            }
            if (it->cur_cluster < 2 || it->cur_cluster == EXFAT_EOC ||
                it->cur_cluster == EXFAT_BAD) {
                it->sector_buf = NULL;
                return -1;
            }
        }

        it->cur_sector = cluster_to_sector(vol, it->cur_cluster) +
                         it->sector_in_cluster;
        it->sector_buf = cache_read(vol, it->cur_sector);
        if (!it->sector_buf)
            return -1;
    }

    return 0;
}

/* ---- Parsed file entry set ---- */

struct exfat_entry_info {
    /* From the file entry (0x85) */
    uint16_t attributes;
    uint32_t create_ts;
    uint32_t modify_ts;

    /* From the stream entry (0xC0) */
    uint32_t first_cluster;
    uint64_t data_length;
    uint64_t valid_data_length;
    uint8_t  stream_flags;
    uint8_t  name_length;
    uint16_t name_hash;

    /* Assembled name (ASCII) */
    char     name[EXFAT_MAX_NAME];

    /* Location of the file entry on disk (for rename/delete) */
    uint64_t file_entry_sector;
    uint32_t file_entry_offset;
    uint8_t  secondary_count;
};

/*
 * Parse a complete file entry set starting at the current dir_iter position.
 * Advances the iterator past the entry set.
 * Returns 0 on success, -1 if not a valid file entry set.
 */
static int parse_entry_set(struct dir_iter *it, struct exfat_entry_info *info)
{
    struct exfat_dentry *de = dir_iter_get(it);
    if (!de || de->type != ENTRY_FILE)
        return -1;

    struct exfat_file_dentry fd;
    memcpy(&fd, de, 32);

    info->file_entry_sector = dir_iter_sector(it);
    info->file_entry_offset = dir_iter_offset_in_sector(it);
    info->secondary_count = fd.secondary_count;
    info->attributes = fd.file_attributes;
    info->create_ts = fd.create_timestamp;
    info->modify_ts = fd.modify_timestamp;

    if (fd.secondary_count < 2)
        return -1;  /* Need at least stream + one name entry */

    /* Advance to stream extension */
    if (dir_iter_next(it) != 0)
        return -1;

    de = dir_iter_get(it);
    if (!de || de->type != ENTRY_STREAM)
        return -1;

    struct exfat_stream_dentry sd;
    memcpy(&sd, de, 32);

    info->first_cluster = sd.first_cluster;
    info->data_length = sd.data_length;
    info->valid_data_length = sd.valid_data_length;
    info->stream_flags = sd.flags;
    info->name_length = sd.name_length;
    info->name_hash = sd.name_hash;

    /* Read file name from subsequent 0xC1 entries */
    uint16_t name_buf[256];
    int name_pos = 0;
    int name_entries = fd.secondary_count - 1; /* minus the stream entry */

    for (int i = 0; i < name_entries; i++) {
        if (dir_iter_next(it) != 0)
            break;
        de = dir_iter_get(it);
        if (!de || de->type != ENTRY_NAME)
            break;

        struct exfat_name_dentry nd;
        memcpy(&nd, de, 32);

        int chars_to_copy = 15;
        if (name_pos + chars_to_copy > (int)sd.name_length)
            chars_to_copy = (int)sd.name_length - name_pos;
        if (chars_to_copy > 0 && name_pos + chars_to_copy < 256) {
            memcpy(&name_buf[name_pos], nd.name, (size_t)chars_to_copy * 2);
            name_pos += chars_to_copy;
        }
    }

    utf16_to_ascii(name_buf, name_pos, info->name, EXFAT_MAX_NAME);
    return 0;
}

/* ---- Path resolution ---- */

/* Helper: find a named entry in a directory cluster */
static int find_in_dir(struct exfat_vol *vol, uint32_t dir_cluster,
                       const char *name, struct exfat_entry_info *info)
{
    struct dir_iter it;
    /* Directories have no size limit; use 0 to mean "follow chain to end" */
    if (dir_iter_init(&it, vol, dir_cluster, 0, 0) != 0)
        return -1;

    for (;;) {
        struct exfat_dentry *de = dir_iter_get(&it);
        if (!de)
            break;

        uint8_t type = de->type;

        if (type == ENTRY_EOD)
            break;

        if (type == ENTRY_FILE) {
            struct exfat_entry_info ei;
            if (parse_entry_set(&it, &ei) == 0) {
                if (ascii_icmp(ei.name, name) == 0) {
                    if (info)
                        memcpy(info, &ei, sizeof(ei));
                    return 0;
                }
            }
            /* parse_entry_set already advanced past the set, continue */
            if (dir_iter_next(&it) != 0)
                break;
            continue;
        }

        /* Skip non-file entries */
        if (dir_iter_next(&it) != 0)
            break;
    }

    return -1;  /* not found */
}

/*
 * Resolve a path like "/dir/subdir/file.txt"
 * Returns 0 on success, fills *info with the entry info if found.
 * For root ("/"), sets info->first_cluster = root_cluster, info->attributes = ATTR_DIRECTORY.
 */
static int resolve_path(struct exfat_vol *vol, const char *path,
                        struct exfat_entry_info *info)
{
    if (!path || !path[0])
        return -1;

    /* Handle root */
    if (path[0] == '/' && path[1] == '\0') {
        if (info) {
            memset(info, 0, sizeof(*info));
            info->first_cluster = vol->root_cluster;
            info->attributes = ATTR_DIRECTORY;
            info->name[0] = '/';
            info->name[1] = '\0';
        }
        return 0;
    }

    uint32_t cur_cluster = vol->root_cluster;
    const char *p = path;

    /* Skip leading slash */
    if (*p == '/')
        p++;

    while (*p) {
        /* Extract next component */
        char component[EXFAT_MAX_NAME];
        int len = 0;
        while (*p && *p != '/' && len < EXFAT_MAX_NAME - 1)
            component[len++] = *p++;
        component[len] = '\0';

        /* Skip separator */
        while (*p == '/')
            p++;

        /* Look up in current directory */
        struct exfat_entry_info ei;
        if (find_in_dir(vol, cur_cluster, component, &ei) != 0)
            return -1;

        if (*p) {
            /* More components: must be a directory */
            if (!(ei.attributes & ATTR_DIRECTORY))
                return -1;
            cur_cluster = ei.first_cluster;
        } else {
            /* Final component */
            if (info)
                memcpy(info, &ei, sizeof(ei));
            return 0;
        }
    }

    return -1;
}

/*
 * Resolve path to the parent directory.
 * Returns parent cluster, and copies last component to name_out.
 */
static uint32_t resolve_parent(struct exfat_vol *vol, const char *path,
                                char *name_out, int name_max)
{
    if (!path || !path[0])
        return 0;

    /* Find last slash */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            last_slash = p;
    }

    if (!last_slash || last_slash == path) {
        /* File is in root directory */
        const char *name = path;
        if (*name == '/')
            name++;
        int len = 0;
        while (name[len] && len < name_max - 1) {
            name_out[len] = name[len];
            len++;
        }
        name_out[len] = '\0';
        return vol->root_cluster;
    }

    /* Resolve parent directory path */
    char parent_path[512];
    int plen = (int)(last_slash - path);
    if (plen >= 512)
        plen = 511;
    memcpy(parent_path, path, (size_t)plen);
    parent_path[plen] = '\0';

    struct exfat_entry_info pinfo;
    if (resolve_path(vol, parent_path, &pinfo) != 0)
        return 0;
    if (!(pinfo.attributes & ATTR_DIRECTORY))
        return 0;

    /* Copy last component */
    const char *name = last_slash + 1;
    int len = 0;
    while (name[len] && len < name_max - 1) {
        name_out[len] = name[len];
        len++;
    }
    name_out[len] = '\0';

    return pinfo.first_cluster;
}

/* ---- Read cluster data ---- */

/*
 * Read data from a cluster chain into a buffer.
 * Handles both FAT-chained and contiguous (NoFatChain) modes.
 */
static int read_data(struct exfat_vol *vol, uint32_t first_cluster,
                     int no_fat_chain, uint64_t length, void *buf)
{
    uint32_t clsz = cluster_size(vol);
    uint8_t *dst = (uint8_t *)buf;
    uint64_t remaining = length;
    uint32_t cluster = first_cluster;

    while (remaining > 0 && cluster >= 2 && cluster != EXFAT_EOC) {
        uint64_t sec = cluster_to_sector(vol, cluster);
        uint32_t chunk = (remaining > clsz) ? clsz : (uint32_t)remaining;
        uint32_t full_secs = chunk / vol->bytes_per_sector;
        uint32_t partial = chunk % vol->bytes_per_sector;

        if (full_secs > 0) {
            if (read_sectors_raw(vol, sec, full_secs, dst) != 0)
                return -1;
            dst += full_secs * vol->bytes_per_sector;
        }

        if (partial > 0) {
            uint8_t *tmp = cache_read(vol, sec + full_secs);
            if (!tmp)
                return -1;
            memcpy(dst, tmp, partial);
            dst += partial;
        }

        remaining -= chunk;
        if (no_fat_chain)
            cluster++;
        else
            cluster = fat_get(vol, cluster);
    }

    return (remaining > 0) ? -1 : 0;
}

/* ---- Write cluster data ---- */

/*
 * Write data to a cluster chain. Allocates new clusters as needed.
 * Returns 0 on success. Sets *out_first to the first cluster of the chain.
 * The chain always uses FAT (not NoFatChain).
 */
static int write_data(struct exfat_vol *vol, const void *data, uint64_t size,
                      uint32_t *out_first)
{
    uint32_t clsz = cluster_size(vol);
    uint32_t clusters_needed = (size > 0)
        ? (uint32_t)((size + clsz - 1) / clsz) : 0;

    if (clusters_needed == 0) {
        *out_first = 0;
        return 0;
    }

    const uint8_t *src = (const uint8_t *)data;
    uint64_t remaining = size;
    uint32_t first = 0, prev = 0;

    for (uint32_t i = 0; i < clusters_needed; i++) {
        uint32_t cl = alloc_cluster_chain(vol, prev);
        if (cl == 0)
            return -1;
        if (i == 0)
            first = cl;

        uint64_t sec = cluster_to_sector(vol, cl);
        uint32_t chunk = (remaining > clsz) ? clsz : (uint32_t)remaining;
        uint32_t full_secs = chunk / vol->bytes_per_sector;
        uint32_t partial = chunk % vol->bytes_per_sector;

        if (full_secs > 0) {
            if (write_sectors_raw(vol, sec, full_secs, src) != 0)
                return -1;
            src += full_secs * vol->bytes_per_sector;
        }

        if (partial > 0) {
            /* Read-modify-write the last partial sector */
            uint8_t *tmp = cache_read(vol, sec + full_secs);
            if (!tmp)
                return -1;
            memset(tmp, 0, vol->bytes_per_sector);
            memcpy(tmp, src, partial);
            cache_mark_dirty(vol, sec + full_secs);
            src += partial;
        }

        /* Zero remaining sectors in the last cluster */
        if (i == clusters_needed - 1) {
            uint32_t used_secs = (chunk + vol->bytes_per_sector - 1) /
                                 vol->bytes_per_sector;
            for (uint32_t s = used_secs; s < vol->sectors_per_cluster; s++) {
                uint8_t *tmp = cache_read(vol, sec + s);
                if (tmp) {
                    memset(tmp, 0, vol->bytes_per_sector);
                    cache_mark_dirty(vol, sec + s);
                }
            }
        }

        remaining -= chunk;
        prev = cl;
    }

    *out_first = first;
    return 0;
}

/* ---- Directory entry creation ---- */

/*
 * Build a complete entry set (file + stream + name entries) for a new file/dir.
 * Returns the total number of 32-byte entries, or -1 on error.
 * Caller must provide a buffer of at least (3 + name_len/15) * 32 bytes.
 */
static int build_entry_set(uint8_t *buf, const char *name, uint16_t attributes,
                           uint32_t first_cluster, uint64_t data_length)
{
    int name_len = (int)strlen(name);
    int name_entries = (name_len + 14) / 15;  /* ceiling division */
    int total_entries = 1 + 1 + name_entries;  /* file + stream + names */

    memset(buf, 0, (size_t)total_entries * 32);

    /* Build UTF-16 name */
    uint16_t name_utf16[256];
    memset(name_utf16, 0, sizeof(name_utf16));
    for (int i = 0; i < name_len && i < 255; i++) {
        uint16_t ch = (uint16_t)(unsigned char)name[i];
        memcpy(&name_utf16[i], &ch, 2);
    }

    uint16_t hash = exfat_name_hash(name_utf16, name_len);

    /* File Directory Entry (0x85) */
    struct exfat_file_dentry *fd = (struct exfat_file_dentry *)buf;
    fd->type = ENTRY_FILE;
    fd->secondary_count = (uint8_t)(total_entries - 1);
    fd->file_attributes = attributes;
    /* Timestamps: encode a fixed date (2026-01-01 00:00:00)
     * exFAT timestamp: bits 31-25=year(since 1980), 24-21=month, 20-16=day,
     *                  15-11=hour, 10-5=minute, 4-0=seconds/2 */
    uint32_t ts = ((2026 - 1980) << 25) | (1 << 21) | (1 << 16);
    fd->create_timestamp = ts;
    fd->modify_timestamp = ts;
    fd->access_timestamp = ts;

    /* Stream Extension (0xC0) */
    struct exfat_stream_dentry *sd = (struct exfat_stream_dentry *)(buf + 32);
    sd->type = ENTRY_STREAM;
    sd->flags = STREAM_ALLOC_POSSIBLE;  /* uses FAT chain */
    sd->name_length = (uint8_t)name_len;
    sd->name_hash = hash;
    sd->first_cluster = first_cluster;
    sd->data_length = data_length;
    sd->valid_data_length = data_length;

    /* File Name Extension entries (0xC1) */
    for (int i = 0; i < name_entries; i++) {
        struct exfat_name_dentry *nd =
            (struct exfat_name_dentry *)(buf + (2 + i) * 32);
        nd->type = ENTRY_NAME;
        nd->flags = 0;

        int start = i * 15;
        int chars = 15;
        if (start + chars > name_len)
            chars = name_len - start;
        if (chars > 0)
            memcpy(nd->name, &name_utf16[start], (size_t)chars * 2);
        /* Remaining chars are zero (already cleared by memset) */
    }

    /* Compute and set checksum */
    uint16_t checksum = entry_set_checksum(buf, total_entries);
    memcpy(buf + 2, &checksum, 2);

    return total_entries;
}

/*
 * Find a free slot in a directory that can hold 'count' consecutive entries.
 * Returns the sector and offset of the first free slot.
 * If no space, extends the directory by allocating a new cluster.
 */
static int find_free_dir_slot(struct exfat_vol *vol, uint32_t dir_cluster,
                              int count, uint64_t *out_sector, uint32_t *out_offset)
{
    struct dir_iter it;
    if (dir_iter_init(&it, vol, dir_cluster, 0, 0) != 0)
        return -1;

    /* Track consecutive free entries */
    int free_run = 0;
    uint64_t run_start_sector = 0;
    uint32_t run_start_offset = 0;

    for (;;) {
        struct exfat_dentry *de = dir_iter_get(&it);
        if (!de)
            break;

        if (de->type == ENTRY_EOD || (de->type & 0x80) == 0) {
            /* Free or end-of-directory entry */
            if (free_run == 0) {
                run_start_sector = dir_iter_sector(&it);
                run_start_offset = dir_iter_offset_in_sector(&it);
            }
            free_run++;
            if (free_run >= count) {
                *out_sector = run_start_sector;
                *out_offset = run_start_offset;
                return 0;
            }
            if (de->type == ENTRY_EOD) {
                while (free_run < count) {
                    if (dir_iter_next(&it) != 0) {
                        goto alloc_new;
                    }
                    free_run++;
                }
                *out_sector = run_start_sector;
                *out_offset = run_start_offset;
                return 0;
            }
        } else {
            /* In-use entry: reset run */
            free_run = 0;
        }

        if (dir_iter_next(&it) != 0)
            break;
    }

alloc_new:
    /* Allocate a new cluster for the directory */
    {
        /* Find the last cluster in the chain */
        uint32_t cl = dir_cluster;
        uint32_t prev = cl;
        while (cl >= 2 && cl != EXFAT_EOC) {
            prev = cl;
            cl = fat_get(vol, cl);
        }

        uint32_t new_cl = alloc_cluster_chain(vol, prev);
        if (new_cl == 0)
            return -1;

        /* Zero the new cluster */
        uint64_t sec = cluster_to_sector(vol, new_cl);
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            uint8_t *buf = cache_read(vol, sec + s);
            if (!buf)
                return -1;
            memset(buf, 0, vol->bytes_per_sector);
            cache_mark_dirty(vol, sec + s);
        }
        cache_flush_all(vol);

        if (free_run > 0) {
            *out_sector = run_start_sector;
            *out_offset = run_start_offset;
        } else {
            *out_sector = sec;
            *out_offset = 0;
        }
        return 0;
    }
}

/*
 * Write entry set into directory at the specified position.
 */
static int write_entry_set(struct exfat_vol *vol, uint64_t start_sector,
                           uint32_t start_offset, const uint8_t *entries,
                           int count)
{
    uint64_t sector = start_sector;
    uint32_t offset = start_offset;
    const uint8_t *src = entries;

    for (int i = 0; i < count; i++) {
        uint8_t *buf = cache_read(vol, sector);
        if (!buf)
            return -1;

        memcpy(buf + offset, src, 32);
        cache_mark_dirty(vol, sector);
        src += 32;
        offset += 32;

        if (offset >= vol->bytes_per_sector) {
            offset = 0;
            sector++;
        }
    }

    return cache_flush_all(vol);
}

/*
 * Add an entry set to a directory.
 */
static int add_entry_to_dir(struct exfat_vol *vol, uint32_t dir_cluster,
                            const uint8_t *entry_set, int entry_count)
{
    uint64_t slot_sector;
    uint32_t slot_offset;

    if (find_free_dir_slot(vol, dir_cluster, entry_count,
                           &slot_sector, &slot_offset) != 0)
        return -1;

    return write_entry_set(vol, slot_sector, slot_offset,
                           entry_set, entry_count);
}

/* ---- Sorting for readdir ---- */

static void sort_entries(struct exfat_dir_info *entries, int count)
{
    /* Simple insertion sort: directories first, then alphabetical */
    for (int i = 1; i < count; i++) {
        struct exfat_dir_info tmp;
        memcpy(&tmp, &entries[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0) {
            int swap = 0;
            if (tmp.is_dir && !entries[j].is_dir) {
                swap = 1;
            } else if (tmp.is_dir == entries[j].is_dir) {
                if (ascii_icmp(tmp.name, entries[j].name) < 0)
                    swap = 1;
            }
            if (!swap)
                break;
            memcpy(&entries[j + 1], &entries[j], sizeof(struct exfat_dir_info));
            j--;
        }
        memcpy(&entries[j + 1], &tmp, sizeof(struct exfat_dir_info));
    }
}

/* ---- Mount helpers ---- */

/*
 * Scan the root directory for the allocation bitmap entry
 * and the volume label entry. Load the bitmap into memory.
 */
static int load_metadata(struct exfat_vol *vol)
{
    struct dir_iter it;
    if (dir_iter_init(&it, vol, vol->root_cluster, 0, 0) != 0)
        return -1;

    int found_bitmap = 0;

    for (;;) {
        struct exfat_dentry *de = dir_iter_get(&it);
        if (!de)
            break;

        uint8_t type = de->type;

        if (type == ENTRY_EOD)
            break;

        if (type == ENTRY_BITMAP) {
            struct exfat_bitmap_dentry bd;
            memcpy(&bd, de, 32);

            /* Only use the first bitmap (bitmap_flags bit 0 = 0) */
            if ((bd.bitmap_flags & 1) == 0) {
                vol->bitmap_cluster = bd.first_cluster;
                vol->bitmap_size = (uint32_t)bd.data_length;
                vol->bitmap_no_fat_chain = 0;
                found_bitmap = 1;
            }
        } else if (type == ENTRY_VLABEL) {
            struct exfat_label_dentry ld;
            memcpy(&ld, de, 32);
            int nchars = ld.char_count;
            if (nchars > 11)
                nchars = 11;
            utf16_to_ascii(ld.label, nchars, vol->label, 48);
        }

        if (dir_iter_next(&it) != 0)
            break;
    }

    if (!found_bitmap)
        return -1;

    /* Allocate and load the bitmap */
    vol->bitmap = (uint8_t *)malloc(vol->bitmap_size);
    if (!vol->bitmap) {
        ESP_LOGE(TAG, "bitmap alloc failed: %lu bytes",
                 (unsigned long)vol->bitmap_size);
        return -1;
    }
    memset(vol->bitmap, 0, vol->bitmap_size);

    if (bitmap_load(vol) != 0) {
        free(vol->bitmap);
        vol->bitmap = NULL;
        return -1;
    }

    return 0;
}

/* ---- Public API ---- */

struct exfat_vol *exfat_mount(exfat_block_read_fn read_fn,
                               exfat_block_write_fn write_fn,
                               void *ctx, uint32_t block_size)
{
    if (!read_fn || block_size == 0)
        return NULL;

    /* Read the boot sector (LBA 0 of volume) */
    uint8_t *boot_buf = (uint8_t *)malloc(4096);
    if (!boot_buf)
        return NULL;

    uint32_t blocks_needed = (512 + block_size - 1) / block_size;
    if (blocks_needed == 0)
        blocks_needed = 1;

    if (read_fn(ctx, 0, blocks_needed, boot_buf) != 0) {
        free(boot_buf);
        return NULL;
    }

    /* Validate the boot sector */
    struct exfat_boot_sector bs;
    memcpy(&bs, boot_buf, sizeof(bs));
    free(boot_buf);

    /* Check filesystem name */
    static const uint8_t exfat_sig[8] = { 'E','X','F','A','T',' ',' ',' ' };
    int sig_ok = 1;
    for (int i = 0; i < 8; i++) {
        if (bs.fs_name[i] != exfat_sig[i]) {
            sig_ok = 0;
            break;
        }
    }
    if (!sig_ok)
        return NULL;

    /* Check boot signature */
    if (bs.boot_signature != 0xAA55)
        return NULL;

    /* Check MustBeZero region */
    {
        int all_zero = 1;
        for (int i = 0; i < 53; i++) {
            if (bs.must_be_zero[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (!all_zero)
            return NULL;
    }

    /* Validate shifts */
    if (bs.bytes_per_sector_shift < 9 || bs.bytes_per_sector_shift > 12)
        return NULL;
    if (bs.sectors_per_cluster_shift > 25)
        return NULL;

    /* Allocate volume structure */
    struct exfat_vol *vol = (struct exfat_vol *)malloc(sizeof(struct exfat_vol));
    if (!vol)
        return NULL;
    memset(vol, 0, sizeof(*vol));

    vol->read_fn = read_fn;
    vol->write_fn = write_fn;
    vol->ctx = ctx;
    vol->dev_block_size = block_size;
    vol->bytes_per_sector = (uint32_t)1 << bs.bytes_per_sector_shift;
    vol->sectors_per_cluster = (uint32_t)1 << bs.sectors_per_cluster_shift;
    vol->cluster_count = bs.cluster_count;
    vol->fat_offset = bs.fat_offset;
    vol->fat_length = bs.fat_length;
    vol->cluster_heap_offset = bs.cluster_heap_offset;
    vol->root_cluster = bs.root_cluster;
    vol->volume_length = bs.volume_length;

    ESP_LOGI(TAG, "mounting: %lu clusters, %luB/sec, %lu sec/cl",
             (unsigned long)vol->cluster_count,
             (unsigned long)vol->bytes_per_sector,
             (unsigned long)vol->sectors_per_cluster);

    /* Initialize sector cache */
    cache_init(vol);

    /* Load metadata (bitmap + volume label) */
    if (load_metadata(vol) != 0) {
        ESP_LOGE(TAG, "mount failed: metadata load error");
        cache_free(vol);
        free(vol);
        return NULL;
    }

    ESP_LOGI(TAG, "mounted: label=\"%s\", bitmap=%lu bytes",
             vol->label, (unsigned long)vol->bitmap_size);
    return vol;
}

void exfat_unmount(struct exfat_vol *vol)
{
    if (!vol)
        return;

    /* Flush everything */
    bitmap_flush(vol);
    cache_flush_all(vol);

    /* Free resources */
    if (vol->bitmap)
        free(vol->bitmap);
    cache_free(vol);
    free(vol);

    ESP_LOGI(TAG, "unmounted");
}

int exfat_readdir(struct exfat_vol *vol, const char *path,
                  struct exfat_dir_info *entries, int max_entries)
{
    if (!vol || !path || !entries || max_entries <= 0)
        return -1;

    /* Resolve the path to a directory */
    struct exfat_entry_info dir_info;
    if (resolve_path(vol, path, &dir_info) != 0)
        return -1;

    if (!(dir_info.attributes & ATTR_DIRECTORY))
        return -1;

    uint32_t dir_cluster = dir_info.first_cluster;
    int count = 0;

    struct dir_iter it;
    if (dir_iter_init(&it, vol, dir_cluster, 0, 0) != 0)
        return -1;

    for (;;) {
        struct exfat_dentry *de = dir_iter_get(&it);
        if (!de)
            break;

        uint8_t type = de->type;

        if (type == ENTRY_EOD)
            break;

        if (type == ENTRY_FILE) {
            struct exfat_entry_info ei;
            if (parse_entry_set(&it, &ei) == 0) {
                if (count < max_entries) {
                    str_copy(entries[count].name, ei.name, EXFAT_MAX_NAME);
                    entries[count].size = ei.data_length;
                    entries[count].is_dir =
                        (ei.attributes & ATTR_DIRECTORY) ? 1 : 0;
                    count++;
                }
            }
            if (dir_iter_next(&it) != 0)
                break;
            continue;
        }

        if (dir_iter_next(&it) != 0)
            break;
    }

    sort_entries(entries, count);
    return count;
}

void *exfat_readfile(struct exfat_vol *vol, const char *path, size_t *out_size)
{
    if (!vol || !path || !out_size)
        return NULL;

    struct exfat_entry_info info;
    if (resolve_path(vol, path, &info) != 0)
        return NULL;

    if (info.attributes & ATTR_DIRECTORY)
        return NULL;

    uint64_t size = info.data_length;
    if (size == 0) {
        /* Empty file: return a 1-byte buffer with NUL */
        uint8_t *buf = (uint8_t *)malloc(1);
        if (buf)
            buf[0] = 0;
        *out_size = 0;
        return buf;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf)
        return NULL;

    int no_fat = (info.stream_flags & STREAM_NO_FAT_CHAIN) ? 1 : 0;

    if (read_data(vol, info.first_cluster, no_fat, size, buf) != 0) {
        free(buf);
        return NULL;
    }

    *out_size = (size_t)size;
    return buf;
}

int exfat_writefile(struct exfat_vol *vol, const char *path,
                    const void *data, size_t size)
{
    if (!vol || !path)
        return -1;
    if (!vol->write_fn)
        return -1;

    /* Resolve parent directory */
    char filename[EXFAT_MAX_NAME];
    uint32_t parent_cluster = resolve_parent(vol, path, filename, EXFAT_MAX_NAME);
    if (parent_cluster == 0)
        return -1;

    /* Check if file already exists — if so, delete it first */
    struct exfat_entry_info existing;
    if (find_in_dir(vol, parent_cluster, filename, &existing) == 0) {
        /* Delete the existing file */
        if (existing.attributes & ATTR_DIRECTORY)
            return -1;  /* Can't overwrite a directory */

        /* Free the data clusters */
        if (existing.first_cluster >= 2) {
            int no_fat = (existing.stream_flags & STREAM_NO_FAT_CHAIN) ? 1 : 0;
            free_chain(vol, existing.first_cluster, no_fat,
                       existing.data_length);
        }

        /* Mark directory entries as deleted (clear InUse bit) */
        uint64_t sec = existing.file_entry_sector;
        uint32_t off = existing.file_entry_offset;
        int total = 1 + existing.secondary_count;

        for (int i = 0; i < total; i++) {
            uint8_t *buf = cache_read(vol, sec);
            if (!buf)
                return -1;
            buf[off] &= 0x7F;  /* Clear InUse bit */
            cache_mark_dirty(vol, sec);
            off += 32;
            if (off >= vol->bytes_per_sector) {
                off = 0;
                sec++;
            }
        }
        cache_flush_all(vol);
    }

    /* Write file data to newly allocated clusters */
    uint32_t first_cluster = 0;
    if (size > 0) {
        if (write_data(vol, data, (uint64_t)size, &first_cluster) != 0)
            return -1;
    }

    /* Build entry set */
    uint8_t entry_buf[32 * 20]; /* max ~18 name entries + file + stream */
    memset(entry_buf, 0, sizeof(entry_buf));
    int entry_count = build_entry_set(entry_buf, filename, ATTR_ARCHIVE,
                                      first_cluster, (uint64_t)size);
    if (entry_count < 0)
        return -1;

    /* Add to parent directory */
    if (add_entry_to_dir(vol, parent_cluster, entry_buf, entry_count) != 0)
        return -1;

    /* Flush bitmap */
    bitmap_flush(vol);
    cache_flush_all(vol);

    return 0;
}

int exfat_mkdir(struct exfat_vol *vol, const char *path)
{
    if (!vol || !path)
        return -1;
    if (!vol->write_fn)
        return -1;

    /* Check if already exists */
    struct exfat_entry_info info;
    if (resolve_path(vol, path, &info) == 0) {
        if (info.attributes & ATTR_DIRECTORY)
            return 0;  /* Already exists as a directory */
        return -1;     /* Exists as a file */
    }

    /* Walk the path, creating directories as needed */
    uint32_t cur_cluster = vol->root_cluster;
    const char *p = path;

    if (*p == '/')
        p++;

    while (*p) {
        char component[EXFAT_MAX_NAME];
        int len = 0;
        while (*p && *p != '/' && len < EXFAT_MAX_NAME - 1)
            component[len++] = *p++;
        component[len] = '\0';

        while (*p == '/')
            p++;

        /* Check if this component exists */
        struct exfat_entry_info ei;
        if (find_in_dir(vol, cur_cluster, component, &ei) == 0) {
            if (!(ei.attributes & ATTR_DIRECTORY))
                return -1;
            cur_cluster = ei.first_cluster;
            continue;
        }

        /* Need to create this directory */
        uint32_t new_cluster = alloc_cluster(vol);
        if (new_cluster == 0)
            return -1;

        /* Zero the cluster (empty directory) */
        uint64_t sec = cluster_to_sector(vol, new_cluster);
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            uint8_t *buf = cache_read(vol, sec + s);
            if (!buf)
                return -1;
            memset(buf, 0, vol->bytes_per_sector);
            cache_mark_dirty(vol, sec + s);
        }
        cache_flush_all(vol);

        /* Build entry set for the new directory */
        uint8_t entry_buf[32 * 20];
        memset(entry_buf, 0, sizeof(entry_buf));
        int entry_count = build_entry_set(entry_buf, component,
                                          ATTR_DIRECTORY, new_cluster, 0);
        if (entry_count < 0)
            return -1;

        /* Add to parent directory */
        if (add_entry_to_dir(vol, cur_cluster, entry_buf, entry_count) != 0)
            return -1;

        bitmap_flush(vol);
        cache_flush_all(vol);

        cur_cluster = new_cluster;
    }

    return 0;
}

int exfat_rename(struct exfat_vol *vol, const char *path, const char *new_name)
{
    if (!vol || !path || !new_name)
        return -1;
    if (!vol->write_fn)
        return -1;

    /* Find the existing entry */
    struct exfat_entry_info info;
    if (resolve_path(vol, path, &info) != 0)
        return -1;

    /* Resolve parent for the new name conflict check */
    char old_name[EXFAT_MAX_NAME];
    uint32_t parent_cluster = resolve_parent(vol, path, old_name, EXFAT_MAX_NAME);
    if (parent_cluster == 0)
        return -1;

    /* Check that the new name doesn't already exist */
    struct exfat_entry_info conflict;
    if (find_in_dir(vol, parent_cluster, new_name, &conflict) == 0)
        return -1;  /* Name already taken */

    /* Strategy: delete old entry, create new one with same data */
    uint32_t first_cluster = info.first_cluster;
    uint64_t data_length = info.data_length;
    uint16_t attributes = info.attributes;

    /* Mark old directory entries as deleted */
    uint64_t sec = info.file_entry_sector;
    uint32_t off = info.file_entry_offset;
    int total = 1 + info.secondary_count;

    for (int i = 0; i < total; i++) {
        uint8_t *buf = cache_read(vol, sec);
        if (!buf)
            return -1;
        buf[off] &= 0x7F;  /* Clear InUse bit */
        cache_mark_dirty(vol, sec);
        off += 32;
        if (off >= vol->bytes_per_sector) {
            off = 0;
            sec++;
        }
    }
    cache_flush_all(vol);

    /* Build new entry set with the new name */
    uint8_t entry_buf[32 * 20];
    memset(entry_buf, 0, sizeof(entry_buf));
    int entry_count = build_entry_set(entry_buf, new_name, attributes,
                                      first_cluster, data_length);
    if (entry_count < 0)
        return -1;

    /* Add to parent directory */
    if (add_entry_to_dir(vol, parent_cluster, entry_buf, entry_count) != 0)
        return -1;

    cache_flush_all(vol);
    return 0;
}

int exfat_delete(struct exfat_vol *vol, const char *path)
{
    if (!vol || !path)
        return -1;
    if (!vol->write_fn)
        return -1;

    struct exfat_entry_info info;
    if (resolve_path(vol, path, &info) != 0)
        return -1;

    /* If it's a directory, check that it's empty */
    if (info.attributes & ATTR_DIRECTORY) {
        if (info.first_cluster >= 2) {
            struct dir_iter it;
            if (dir_iter_init(&it, vol, info.first_cluster, 0, 0) == 0) {
                for (;;) {
                    struct exfat_dentry *de = dir_iter_get(&it);
                    if (!de)
                        break;
                    if (de->type == ENTRY_EOD)
                        break;
                    if (de->type == ENTRY_FILE)
                        return -1;  /* Directory not empty */
                    if (dir_iter_next(&it) != 0)
                        break;
                }
            }
        }
    }

    /* Free the data clusters */
    if (info.first_cluster >= 2) {
        int no_fat = (info.stream_flags & STREAM_NO_FAT_CHAIN) ? 1 : 0;
        free_chain(vol, info.first_cluster, no_fat, info.data_length);
    }

    /* Mark directory entries as deleted */
    uint64_t sec = info.file_entry_sector;
    uint32_t off = info.file_entry_offset;
    int total = 1 + info.secondary_count;

    for (int i = 0; i < total; i++) {
        uint8_t *buf = cache_read(vol, sec);
        if (!buf)
            return -1;
        buf[off] &= 0x7F;  /* Clear InUse bit */
        cache_mark_dirty(vol, sec);
        off += 32;
        if (off >= vol->bytes_per_sector) {
            off = 0;
            sec++;
        }
    }

    /* Flush everything */
    bitmap_flush(vol);
    cache_flush_all(vol);

    return 0;
}

int exfat_volume_info(struct exfat_vol *vol, uint64_t *total_bytes,
                      uint64_t *free_bytes)
{
    if (!vol)
        return -1;

    uint32_t clsz = cluster_size(vol);

    if (total_bytes)
        *total_bytes = (uint64_t)vol->cluster_count * (uint64_t)clsz;

    if (free_bytes) {
        uint32_t free_clusters = 0;
        for (uint32_t i = 0; i < vol->cluster_count; i++) {
            if (!bitmap_get(vol, i + 2))
                free_clusters++;
        }
        *free_bytes = (uint64_t)free_clusters * (uint64_t)clsz;
    }

    return 0;
}

uint64_t exfat_file_size(struct exfat_vol *vol, const char *path)
{
    if (!vol || !path)
        return 0;

    struct exfat_entry_info info;
    if (resolve_path(vol, path, &info) != 0)
        return 0;

    if (info.attributes & ATTR_DIRECTORY)
        return 0;

    return info.data_length;
}

int exfat_exists(struct exfat_vol *vol, const char *path)
{
    if (!vol || !path)
        return 0;

    struct exfat_entry_info info;
    return (resolve_path(vol, path, &info) == 0) ? 1 : 0;
}

const char *exfat_get_label(struct exfat_vol *vol)
{
    if (!vol)
        return "";
    return vol->label;
}

/* ---- SD card convenience wrapper ---- */

static struct {
    uint32_t partition_start_lba;
} s_exfat_ctx;

static int exfat_sd_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    return sdcard_read((uint32_t)(s_exfat_ctx.partition_start_lba + lba),
                       count, buf);
}

static int exfat_sd_write(void *ctx, uint64_t lba, uint32_t count,
                          const void *buf)
{
    (void)ctx;
    return sdcard_write((uint32_t)(s_exfat_ctx.partition_start_lba + lba),
                        count, buf);
}

struct exfat_vol *exfat_mount_sdcard(uint32_t partition_start_lba)
{
    s_exfat_ctx.partition_start_lba = partition_start_lba;
    return exfat_mount(exfat_sd_read, exfat_sd_write, &s_exfat_ctx, 512);
}
