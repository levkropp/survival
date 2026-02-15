/*
 * fat32.c — FAT32 filesystem creation on block devices
 *
 * Creates a valid FAT32 filesystem (superfloppy — no MBR partition table).
 * Supports writing files and creating directories.
 *
 * Superfloppy layout (FAT32 BPB at LBA 0):
 *   LBA 0:                     BPB (Volume Boot Record)
 *   LBA 1:                     FSInfo
 *   LBA 6:                     Backup BPB
 *   LBA 32 (RESERVED_SECTORS): FAT1
 *   LBA 32+fat_sectors:        FAT2
 *   After FATs:                Data region (cluster 2 = root directory)
 */

#include "boot.h"
#include "mem.h"
#include "disk.h"
#include "fat32.h"
#include "shim.h"

/* FAT32 constants */
#define SECTOR_SIZE       512
#define RESERVED_SECTORS  32
#define NUM_FATS          2
#define FAT32_EOC        0x0FFFFFF8
#define FAT32_FREE       0x00000000
#define MIN_FAT32_CLUSTERS 65525  /* below this, FAT driver treats as FAT16 */

/* On-disk structures (packed) */
#pragma pack(1)

struct fat32_bpb {
    UINT8  jmp[3];               /* EB 58 90 */
    UINT8  oem[8];               /* "SURVIVAL" */
    UINT16 bytes_per_sector;     /* 512 */
    UINT8  sectors_per_cluster;  /* 8 */
    UINT16 reserved_sectors;     /* 32 */
    UINT8  num_fats;             /* 2 */
    UINT16 root_entry_count;     /* 0 for FAT32 */
    UINT16 total_sectors_16;     /* 0 for FAT32 */
    UINT8  media_type;           /* 0xF8 */
    UINT16 fat_size_16;          /* 0 for FAT32 */
    UINT16 sectors_per_track;    /* 63 */
    UINT16 num_heads;            /* 255 */
    UINT32 hidden_sectors;       /* partition start LBA */
    UINT32 total_sectors_32;
    /* FAT32-specific */
    UINT32 fat_size_32;
    UINT16 ext_flags;
    UINT16 fs_version;
    UINT32 root_cluster;         /* 2 */
    UINT16 fs_info_sector;       /* 1 */
    UINT16 backup_boot_sector;   /* 6 */
    UINT8  reserved[12];
    UINT8  drive_number;         /* 0x80 */
    UINT8  reserved1;
    UINT8  boot_sig;             /* 0x29 */
    UINT32 volume_id;
    UINT8  volume_label[11];     /* "SURVIVAL   " */
    UINT8  fs_type[8];           /* "FAT32   " */
    UINT8  boot_code[420];
    UINT16 signature;            /* 0xAA55 */
};

struct fat32_dir_entry {
    UINT8  name[11];             /* 8.3 format, space-padded */
    UINT8  attr;
    UINT8  nt_reserved;
    UINT8  create_time_tenth;
    UINT16 create_time;
    UINT16 create_date;
    UINT16 access_date;
    UINT16 first_cluster_hi;
    UINT16 modify_time;
    UINT16 modify_date;
    UINT16 first_cluster_lo;
    UINT32 file_size;
};

struct fat32_fsinfo {
    UINT32 lead_sig;             /* 0x41615252 */
    UINT8  reserved1[480];
    UINT32 struct_sig;           /* 0x61417272 */
    UINT32 free_count;
    UINT32 next_free;
    UINT8  reserved2[12];
    UINT32 trail_sig;            /* 0xAA550000 */
};

#pragma pack()

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20

/* ---- Internal state for a formatted device ---- */

struct fat32_state {
    struct disk_device *dev;
    UINT32 part_start;       /* LBA of partition start */
    UINT32 fat_start;        /* LBA of first FAT (relative to disk) */
    UINT32 fat_sectors;      /* sectors per FAT */
    UINT32 data_start;       /* LBA of data region */
    UINT32 total_clusters;
    UINT32 next_free_cluster;
    UINT32 spc;              /* sectors per cluster (dynamic) */
};

static struct fat32_state s_fs;

/* ---- Sector I/O helpers ---- */

static int write_sector(UINT64 lba, void *buf) {
    return disk_write_blocks(s_fs.dev, lba, 1, buf);
}

static int read_sector(UINT64 lba, void *buf) {
    return disk_read_blocks(s_fs.dev, lba, 1, buf);
}

static int write_sectors(UINT64 lba, UINT64 count, void *buf) {
    return disk_write_blocks(s_fs.dev, lba, count, buf);
}

/* ---- FAT table operations ---- */

static int fat_set(UINT32 cluster, UINT32 value) {
    UINT32 fat_offset = cluster * 4;
    UINT32 fat_sector = fat_offset / SECTOR_SIZE;
    UINT32 fat_entry_offset = fat_offset % SECTOR_SIZE;

    UINT8 sector_buf[SECTOR_SIZE];
    if (read_sector(s_fs.fat_start + fat_sector, sector_buf) < 0)
        return -1;

    /* Write 28-bit entry (preserve top 4 bits) */
    UINT32 *entry = (UINT32 *)(sector_buf + fat_entry_offset);
    *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);

    /* Write to both FAT copies */
    if (write_sector(s_fs.fat_start + fat_sector, sector_buf) < 0)
        return -1;
    if (write_sector(s_fs.fat_start + s_fs.fat_sectors + fat_sector, sector_buf) < 0)
        return -1;

    return 0;
}

static UINT32 fat_get(UINT32 cluster) {
    UINT32 fat_offset = cluster * 4;
    UINT32 fat_sector = fat_offset / SECTOR_SIZE;
    UINT32 fat_entry_offset = fat_offset % SECTOR_SIZE;

    UINT8 sector_buf[SECTOR_SIZE];
    if (read_sector(s_fs.fat_start + fat_sector, sector_buf) < 0)
        return FAT32_EOC;

    UINT32 *entry = (UINT32 *)(sector_buf + fat_entry_offset);
    return *entry & 0x0FFFFFFF;
}

/* Allocate a new cluster, chain it to prev (or 0 for new chain) */
static UINT32 alloc_cluster(UINT32 prev) {
    UINT32 cl = s_fs.next_free_cluster;
    if (cl >= s_fs.total_clusters + 2)
        return 0; /* out of space */

    s_fs.next_free_cluster = cl + 1;

    /* Mark as end of chain */
    fat_set(cl, FAT32_EOC);

    /* Link to previous */
    if (prev >= 2)
        fat_set(prev, cl);

    return cl;
}

/* Get the LBA of the first sector of a cluster */
static UINT64 cluster_to_lba(UINT32 cluster) {
    return (UINT64)s_fs.data_start +
           (UINT64)(cluster - 2) * (UINT64)s_fs.spc;
}

/* ---- Format ---- */

int fat32_format(struct disk_device *dev) {
    if (!dev || dev->is_boot_device)
        return -1;

    s_fs.dev = dev;
    s_fs.part_start = 0; /* superfloppy: BPB at LBA 0, no MBR */

    UINT32 total_disk_sectors = (UINT32)(dev->size_bytes / SECTOR_SIZE);
    if (total_disk_sectors <= RESERVED_SECTORS + 100)
        return -1; /* too small */

    /* Choose sectors_per_cluster to ensure >= MIN_FAT32_CLUSTERS.
     * Start at 8 (4KB clusters) and halve until cluster count is valid. */
    UINT32 spc = 8;
    while (spc > 1) {
        UINT32 est = total_disk_sectors - RESERVED_SECTORS;
        UINT32 ncl = est / spc;
        UINT32 fat_sec = (ncl * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
        UINT32 data_sec = total_disk_sectors - RESERVED_SECTORS - fat_sec * NUM_FATS;
        ncl = data_sec / spc;
        if (ncl >= MIN_FAT32_CLUSTERS) break;
        spc /= 2;
    }
    s_fs.spc = spc;

    /* Calculate FAT size */
    UINT32 est_data = total_disk_sectors - RESERVED_SECTORS;
    UINT32 est_clusters = est_data / spc;
    s_fs.fat_sectors = (est_clusters * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;

    s_fs.fat_start = RESERVED_SECTORS;
    s_fs.data_start = s_fs.fat_start + s_fs.fat_sectors * NUM_FATS;
    s_fs.total_clusters = (total_disk_sectors - RESERVED_SECTORS - s_fs.fat_sectors * NUM_FATS)
                          / spc;
    s_fs.next_free_cluster = 3; /* cluster 2 = root dir */

    /* Zero out the reserved sectors area */
    UINT8 zero[SECTOR_SIZE];
    mem_set(zero, 0, SECTOR_SIZE);
    for (UINT32 i = 0; i < RESERVED_SECTORS; i++) {
        if (write_sector(i, zero) < 0)
            return -1;
    }

    /* ---- Write BPB at LBA 0 ---- */
    UINT8 bpb_buf[SECTOR_SIZE];
    mem_set(bpb_buf, 0, SECTOR_SIZE);
    struct fat32_bpb *bpb = (struct fat32_bpb *)bpb_buf;
    bpb->jmp[0] = 0xEB; bpb->jmp[1] = 0x58; bpb->jmp[2] = 0x90;
    memcpy(bpb->oem, "SURVIVAL", 8);
    bpb->bytes_per_sector = SECTOR_SIZE;
    bpb->sectors_per_cluster = (UINT8)spc;
    bpb->reserved_sectors = RESERVED_SECTORS;
    bpb->num_fats = NUM_FATS;
    bpb->root_entry_count = 0;
    bpb->total_sectors_16 = 0;
    bpb->media_type = 0xF8;
    bpb->fat_size_16 = 0;
    bpb->sectors_per_track = 63;
    bpb->num_heads = 255;
    bpb->hidden_sectors = 0; /* superfloppy: no hidden sectors */
    bpb->total_sectors_32 = total_disk_sectors;
    bpb->fat_size_32 = s_fs.fat_sectors;
    bpb->ext_flags = 0;
    bpb->fs_version = 0;
    bpb->root_cluster = 2;
    bpb->fs_info_sector = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->boot_sig = 0x29;
    bpb->volume_id = 0x12345678;
    memcpy(bpb->volume_label, "SURVIVAL   ", 11);
    memcpy(bpb->fs_type, "FAT32   ", 8);
    bpb->signature = 0xAA55;
    if (write_sector(0, bpb_buf) < 0)
        return -1;
    /* Backup BPB at sector 6 */
    if (write_sector(6, bpb_buf) < 0)
        return -1;

    /* ---- Write FSInfo ---- */
    UINT8 fsinfo_buf[SECTOR_SIZE];
    mem_set(fsinfo_buf, 0, SECTOR_SIZE);
    struct fat32_fsinfo *fsi = (struct fat32_fsinfo *)fsinfo_buf;
    fsi->lead_sig = 0x41615252;
    fsi->struct_sig = 0x61417272;
    fsi->free_count = s_fs.total_clusters - 1; /* minus root dir cluster */
    fsi->next_free = 3;
    fsi->trail_sig = 0xAA550000;
    if (write_sector(1, fsinfo_buf) < 0)
        return -1;

    /* ---- Initialize FAT tables ---- */
    /* Zero out both FATs */
    for (UINT32 i = 0; i < s_fs.fat_sectors * NUM_FATS; i++) {
        if (write_sector(s_fs.fat_start + i, zero) < 0)
            return -1;
    }

    /* Set FAT entries for cluster 0, 1, and 2 (root dir) */
    fat_set(0, 0x0FFFFFF8); /* media descriptor */
    fat_set(1, 0x0FFFFFFF); /* end of chain marker */
    fat_set(2, FAT32_EOC);  /* root directory */

    /* ---- Initialize root directory cluster ---- */
    UINT64 root_lba = cluster_to_lba(2);
    for (UINT32 i = 0; i < spc; i++) {
        if (write_sector(root_lba + (UINT64)i, zero) < 0)
            return -1;
    }

    /* Write volume label directory entry */
    UINT8 dir_buf[SECTOR_SIZE];
    mem_set(dir_buf, 0, SECTOR_SIZE);
    struct fat32_dir_entry *de = (struct fat32_dir_entry *)dir_buf;
    memcpy(de->name, "SURVIVAL   ", 11);
    de->attr = ATTR_VOLUME_ID;
    de->modify_date = (2026 - 1980) << 9 | 1 << 5 | 1; /* 2026-01-01 */
    if (write_sector(root_lba, dir_buf) < 0)
        return -1;

    return 0;
}

/* ---- Name conversion helpers ---- */

/* Convert a filename component to 8.3 format */
static void name_to_83(const char *name, UINT8 *out) {
    mem_set(out, ' ', 11);

    /* Find the dot for extension */
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }

    int i = 0;
    const char *p = name;
    if (dot) {
        /* Copy name part (up to 8 chars) */
        while (p < dot && i < 8) {
            out[i++] = (UINT8)toupper((unsigned char)*p);
            p++;
        }
        /* Copy extension (up to 3 chars) */
        p = dot + 1;
        i = 8;
        while (*p && i < 11) {
            out[i++] = (UINT8)toupper((unsigned char)*p);
            p++;
        }
    } else {
        /* No extension */
        while (*p && i < 8) {
            out[i++] = (UINT8)toupper((unsigned char)*p);
            p++;
        }
    }
}

/* Find a directory entry in a directory cluster chain.
 * Returns cluster and entry index, or 0 if not found. */
static UINT32 find_in_dir(UINT32 dir_cluster, const char *name,
                          int *entry_idx) {
    UINT8 target[11];
    name_to_83(name, target);

    UINT32 cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        UINT64 lba = cluster_to_lba(cluster);
        for (int sec = 0; sec < s_fs.spc; sec++) {
            UINT8 buf[SECTOR_SIZE];
            if (read_sector(lba + (UINT64)sec, buf) < 0)
                return 0;
            struct fat32_dir_entry *entries = (struct fat32_dir_entry *)buf;
            int count = SECTOR_SIZE / (int)sizeof(struct fat32_dir_entry);
            for (int i = 0; i < count; i++) {
                if (entries[i].name[0] == 0x00)
                    return 0; /* end of dir */
                if (entries[i].name[0] == 0xE5)
                    continue; /* deleted */
                if (entries[i].attr == ATTR_VOLUME_ID)
                    continue;
                if (memcmp(entries[i].name, target, 11) == 0) {
                    if (entry_idx) *entry_idx = sec * count + i;
                    return ((UINT32)entries[i].first_cluster_hi << 16)
                         | (UINT32)entries[i].first_cluster_lo;
                }
            }
        }
        cluster = fat_get(cluster);
    }
    return 0;
}

/* Add a directory entry to a directory.
 * Returns 0 on success. */
static int add_dir_entry(UINT32 dir_cluster, struct fat32_dir_entry *new_entry) {
    UINT32 cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        UINT64 lba = cluster_to_lba(cluster);
        for (int sec = 0; sec < s_fs.spc; sec++) {
            UINT8 buf[SECTOR_SIZE];
            if (read_sector(lba + (UINT64)sec, buf) < 0)
                return -1;
            struct fat32_dir_entry *entries = (struct fat32_dir_entry *)buf;
            int count = SECTOR_SIZE / (int)sizeof(struct fat32_dir_entry);
            for (int i = 0; i < count; i++) {
                if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                    memcpy(&entries[i], new_entry, sizeof(struct fat32_dir_entry));
                    /* Mark next entry as end if this was the end */
                    if (i + 1 < count && entries[i + 1].name[0] != 0xE5)
                        ; /* next exists */
                    return write_sector(lba + (UINT64)sec, buf);
                }
            }
        }
        UINT32 next = fat_get(cluster);
        if (next >= FAT32_EOC) {
            /* Need to allocate a new cluster for the directory */
            UINT32 new_cl = alloc_cluster(cluster);
            if (new_cl == 0) return -1;
            /* Zero the new cluster */
            UINT8 zero[SECTOR_SIZE];
            mem_set(zero, 0, SECTOR_SIZE);
            UINT64 new_lba = cluster_to_lba(new_cl);
            for (int s = 0; s < s_fs.spc; s++)
                write_sector(new_lba + (UINT64)s, zero);
            cluster = new_cl;
            /* Write entry in first sector of new cluster */
            UINT8 buf2[SECTOR_SIZE];
            mem_set(buf2, 0, SECTOR_SIZE);
            memcpy(buf2, new_entry, sizeof(struct fat32_dir_entry));
            return write_sector(new_lba, buf2);
        }
        cluster = next;
    }
    return -1;
}

/* Create or find a directory, returns its first cluster */
static UINT32 ensure_dir(UINT32 parent_cluster, const char *name) {
    UINT32 existing = find_in_dir(parent_cluster, name, NULL);
    if (existing >= 2)
        return existing;

    /* Create new directory */
    UINT32 new_cluster = alloc_cluster(0);
    if (new_cluster == 0) return 0;

    /* Zero the cluster */
    UINT8 zero[SECTOR_SIZE];
    mem_set(zero, 0, SECTOR_SIZE);
    UINT64 lba = cluster_to_lba(new_cluster);
    for (int i = 0; i < s_fs.spc; i++)
        write_sector(lba + (UINT64)i, zero);

    /* Create . and .. entries */
    UINT8 dir_buf[SECTOR_SIZE];
    mem_set(dir_buf, 0, SECTOR_SIZE);
    struct fat32_dir_entry *entries = (struct fat32_dir_entry *)dir_buf;

    /* "." entry */
    memcpy(entries[0].name, ".          ", 11);
    entries[0].attr = ATTR_DIRECTORY;
    entries[0].first_cluster_hi = (UINT16)(new_cluster >> 16);
    entries[0].first_cluster_lo = (UINT16)(new_cluster & 0xFFFF);
    entries[0].modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;

    /* ".." entry */
    memcpy(entries[1].name, "..         ", 11);
    entries[1].attr = ATTR_DIRECTORY;
    entries[1].first_cluster_hi = (UINT16)(parent_cluster >> 16);
    entries[1].first_cluster_lo = (UINT16)(parent_cluster & 0xFFFF);
    entries[1].modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;

    write_sector(lba, dir_buf);

    /* Add entry to parent */
    struct fat32_dir_entry parent_entry;
    mem_set(&parent_entry, 0, sizeof(parent_entry));
    name_to_83(name, parent_entry.name);
    parent_entry.attr = ATTR_DIRECTORY;
    parent_entry.first_cluster_hi = (UINT16)(new_cluster >> 16);
    parent_entry.first_cluster_lo = (UINT16)(new_cluster & 0xFFFF);
    parent_entry.modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;
    add_dir_entry(parent_cluster, &parent_entry);

    return new_cluster;
}

/* ---- Public API ---- */

int fat32_mkdir(struct disk_device *dev, const char *path) {
    if (!dev || !path) return -1;
    if (s_fs.dev != dev) return -1; /* must format first */

    /* Walk the path, creating directories */
    UINT32 cluster = 2; /* root */
    char component[64];

    while (*path) {
        /* Skip separators */
        while (*path == '\\' || *path == '/') path++;
        if (!*path) break;

        /* Extract component */
        int len = 0;
        while (*path && *path != '\\' && *path != '/' && len < 63)
            component[len++] = *path++;
        component[len] = '\0';

        cluster = ensure_dir(cluster, component);
        if (cluster == 0) return -1;
    }
    return 0;
}

int fat32_write_file(struct disk_device *dev, const char *path,
                     void *data, UINT64 size) {
    if (!dev || !path || !data) return -1;
    if (s_fs.dev != dev) return -1; /* must format first */

    /* Split path into directory and filename */
    const char *last_sep = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') last_sep = p;
    }

    UINT32 dir_cluster = 2; /* root */
    const char *filename;

    if (last_sep) {
        /* Create parent directories */
        char dirpath[256];
        int dlen = (int)(last_sep - path);
        if (dlen >= 256) dlen = 255;
        memcpy(dirpath, path, (size_t)dlen);
        dirpath[dlen] = '\0';

        /* Walk directory path */
        char *p = dirpath;
        while (*p) {
            while (*p == '\\' || *p == '/') p++;
            if (!*p) break;
            char component[64];
            int len = 0;
            while (*p && *p != '\\' && *p != '/' && len < 63)
                component[len++] = *p++;
            component[len] = '\0';
            dir_cluster = ensure_dir(dir_cluster, component);
            if (dir_cluster == 0) return -1;
        }
        filename = last_sep + 1;
    } else {
        filename = path;
    }

    /* Allocate clusters for file data */
    UINT32 cluster_size = s_fs.spc * SECTOR_SIZE;
    UINT32 clusters_needed = (size > 0) ? (UINT32)((size + cluster_size - 1) / cluster_size) : 1;

    UINT32 first_cluster = 0;
    UINT32 prev_cluster = 0;
    UINT8 *src = (UINT8 *)data;
    UINT64 remaining = size;

    for (UINT32 i = 0; i < clusters_needed; i++) {
        UINT32 cl = alloc_cluster(prev_cluster);
        if (cl == 0) return -1;
        if (i == 0) first_cluster = cl;

        UINT64 lba = cluster_to_lba(cl);
        UINT32 bytes_to_write = (remaining > cluster_size) ? cluster_size : (UINT32)remaining;

        /* Write sectors for this cluster */
        for (int sec = 0; sec < s_fs.spc; sec++) {
            UINT8 buf[SECTOR_SIZE];
            UINT32 sec_offset = (UINT32)sec * SECTOR_SIZE;
            if (sec_offset < bytes_to_write) {
                UINT32 n = bytes_to_write - sec_offset;
                if (n > SECTOR_SIZE) n = SECTOR_SIZE;
                memcpy(buf, src + sec_offset, n);
                if (n < SECTOR_SIZE) mem_set(buf + n, 0, SECTOR_SIZE - n);
            } else {
                mem_set(buf, 0, SECTOR_SIZE);
            }
            if (write_sector(lba + (UINT64)sec, buf) < 0)
                return -1;
        }

        src += bytes_to_write;
        remaining -= bytes_to_write;
        prev_cluster = cl;
    }

    /* Add file entry to directory */
    struct fat32_dir_entry entry;
    mem_set(&entry, 0, sizeof(entry));
    name_to_83(filename, entry.name);
    entry.attr = ATTR_ARCHIVE;
    entry.first_cluster_hi = (UINT16)(first_cluster >> 16);
    entry.first_cluster_lo = (UINT16)(first_cluster & 0xFFFF);
    entry.file_size = (UINT32)size;
    entry.modify_date = (2026 - 1980) << 9 | 1 << 5 | 1;

    return add_dir_entry(dir_cluster, &entry);
}
