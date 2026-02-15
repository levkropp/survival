/*
 * ntfs.c -- NTFS filesystem driver (read-only)
 *
 * Bare-metal UEFI implementation.  No libc -- uses only mem_alloc/mem_free/
 * mem_set/mem_copy/str_len/str_cmp/str_copy from mem.h.
 *
 * Supports: path resolution, readdir (small + large directories),
 *           readfile (resident + non-resident $DATA), volume label.
 * Does NOT support: compression, encryption, sparse, ADS, journaling.
 */

#include "ntfs.h"
#include "mem.h"

/* ------------------------------------------------------------------ */
/* On-disk structure definitions                                       */
/* ------------------------------------------------------------------ */

#pragma pack(1)

/* NTFS boot sector (BPB) */
struct ntfs_bpb {
    UINT8   jmp[3];
    UINT8   oem_id[8];             /* "NTFS    " */
    UINT16  bytes_per_sector;
    UINT8   sectors_per_cluster;
    UINT16  reserved_sectors;      /* 0 */
    UINT8   number_of_fats;        /* 0 */
    UINT16  root_entries;          /* 0 */
    UINT16  total_sectors_16;      /* 0 */
    UINT8   media_type;
    UINT16  fat_size;              /* 0 */
    UINT16  sectors_per_track;
    UINT16  number_of_heads;
    UINT32  hidden_sectors;
    UINT32  total_sectors_32;      /* 0 */
    UINT32  unused0;               /* 0x800080 or similar */
    UINT64  total_sectors;
    UINT64  mft_cluster;
    UINT64  mft_mirr_cluster;
    INT8    clusters_per_mft_record;
    UINT8   unused1[3];
    INT8    clusters_per_index_block;
    UINT8   unused2[3];
    UINT64  volume_serial;
    UINT32  checksum;
};

/* MFT record header */
struct ntfs_mft_header {
    UINT8   signature[4];          /* "FILE" */
    UINT16  usa_offset;            /* offset to update sequence array */
    UINT16  usa_count;             /* number of entries (including value) */
    UINT64  lsn;
    UINT16  sequence_number;
    UINT16  hard_link_count;
    UINT16  first_attr_offset;
    UINT16  flags;                 /* 0x01=InUse, 0x02=Directory */
    UINT32  used_size;
    UINT32  allocated_size;
    UINT64  base_record;
    UINT16  next_attr_id;
};

/* Attribute header (common part) */
struct ntfs_attr_header {
    UINT32  type;
    UINT32  length;
    UINT8   non_resident;
    UINT8   name_length;           /* in UTF-16 chars */
    UINT16  name_offset;
    UINT16  flags;
    UINT16  attribute_id;
};

/* Resident attribute (follows common header) */
struct ntfs_attr_resident {
    UINT32  value_length;
    UINT16  value_offset;
    UINT16  flags;
};

/* Non-resident attribute (follows common header) */
struct ntfs_attr_nonresident {
    UINT64  starting_vcn;
    UINT64  last_vcn;
    UINT16  data_runs_offset;
    UINT16  compression_unit;
    UINT32  padding;
    UINT64  allocated_size;
    UINT64  real_size;
    UINT64  initialized_size;
};

/* $FILE_NAME attribute body */
struct ntfs_filename {
    UINT64  parent_ref;            /* low 6 bytes = MFT record, high 2 = seq */
    UINT64  creation_time;
    UINT64  modification_time;
    UINT64  mft_modification_time;
    UINT64  read_time;
    UINT64  allocated_size;
    UINT64  real_size;
    UINT32  flags;                 /* 0x10000000 = directory */
    UINT32  reparse_value;
    UINT8   name_length;           /* in UTF-16 chars */
    UINT8   name_namespace;        /* 0=POSIX, 1=Win32, 2=DOS, 3=Win32+DOS */
    /* UINT16 name[] follows */
};

/* INDEX_ROOT header (value of 0x90 attribute) */
struct ntfs_index_root {
    UINT32  attr_type;             /* 0x30 for filename index */
    UINT32  collation_rule;
    UINT32  index_block_size;
    UINT8   clusters_per_index_block;
    UINT8   padding[3];
    /* Index node header follows at offset 16 */
};

/* Index node header (appears in INDEX_ROOT and INDX blocks) */
struct ntfs_index_node_header {
    UINT32  entries_offset;        /* relative to start of this header */
    UINT32  total_size;
    UINT32  allocated_size;
    UINT32  flags;                 /* 0x01 = has sub-nodes */
};

/* INDX block header */
struct ntfs_indx_header {
    UINT8   signature[4];          /* "INDX" */
    UINT16  usa_offset;
    UINT16  usa_count;
    UINT64  lsn;
    UINT64  vcn;
    /* Index node header at offset 24 */
};

/* Index entry */
struct ntfs_index_entry_header {
    UINT64  mft_reference;         /* low 6 bytes = record num, high 2 = seq */
    UINT16  entry_length;
    UINT16  stream_length;         /* length of $FILE_NAME data */
    UINT32  flags;                 /* 0x01=has sub-node, 0x02=last entry */
};

#pragma pack()

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/* Attribute types */
#define NTFS_AT_STANDARD_INFO     0x10
#define NTFS_AT_ATTRIBUTE_LIST    0x20
#define NTFS_AT_FILE_NAME         0x30
#define NTFS_AT_OBJECT_ID         0x40
#define NTFS_AT_SECURITY          0x50
#define NTFS_AT_VOLUME_NAME       0x60
#define NTFS_AT_VOLUME_INFO       0x70
#define NTFS_AT_DATA              0x80
#define NTFS_AT_INDEX_ROOT        0x90
#define NTFS_AT_INDEX_ALLOCATION  0xA0
#define NTFS_AT_BITMAP            0xB0
#define NTFS_AT_END               0xFFFFFFFF

/* MFT flags */
#define NTFS_MFT_IN_USE           0x0001
#define NTFS_MFT_DIRECTORY        0x0002

/* File name namespace */
#define NTFS_NS_POSIX             0
#define NTFS_NS_WIN32             1
#define NTFS_NS_DOS               2
#define NTFS_NS_WIN32_DOS         3

/* Well-known MFT records */
#define NTFS_MFT_RECORD_MFT       0
#define NTFS_MFT_RECORD_VOLUME    3
#define NTFS_MFT_RECORD_ROOT      5

/* Index entry flags */
#define NTFS_INDEX_ENTRY_SUBNODE  0x01
#define NTFS_INDEX_ENTRY_LAST     0x02

/* $FILE_NAME flags */
#define NTFS_FILE_ATTR_DIRECTORY  0x10000000

/* Sector cache */
#define NTFS_CACHE_SIZE           16

/* Maximum number of data runs we track */
#define NTFS_MAX_RUNS             512

/* Maximum path components */
#define NTFS_MAX_PATH_DEPTH       32

/* Maximum directory entries for readdir dedup */
#define NTFS_MAX_DIR_ENTRIES      1024

/* Maximum MFT record size we expect */
#define NTFS_MAX_MFT_SIZE         4096

/* Maximum index block size we expect */
#define NTFS_MAX_INDX_SIZE        65536

/* ------------------------------------------------------------------ */
/* Volume structure                                                    */
/* ------------------------------------------------------------------ */

struct ntfs_cache_entry {
    UINT64  sector;
    UINT8  *data;
    int     valid;
};

/* A decoded data run extent */
struct ntfs_extent {
    UINT64  vcn;        /* starting VCN for this extent */
    UINT64  lcn;        /* starting LCN on disk */
    UINT64  length;     /* number of clusters */
};

struct ntfs_vol {
    ntfs_block_read_fn  read_fn;
    void               *ctx;

    UINT32  dev_block_size;
    UINT32  bytes_per_sector;
    UINT32  sectors_per_cluster;
    UINT32  bytes_per_cluster;
    UINT32  mft_record_size;       /* typically 1024 */
    UINT32  index_block_size;      /* typically 4096 */
    UINT64  mft_cluster;           /* starting cluster of $MFT */
    UINT64  total_sectors;

    /* Cached $MFT data runs (raw bytes from the attribute) */
    UINT8  *mft_runs;
    UINT32  mft_runs_len;

    /* Decoded $MFT extents for fast lookup */
    struct ntfs_extent *mft_extents;
    int     mft_extent_count;

    /* Volume label */
    char    label[48];

    /* Total cluster count (for volume_info) */
    UINT64  total_clusters;

    /* Sector cache */
    struct ntfs_cache_entry cache[NTFS_CACHE_SIZE];
    int     cache_clock;           /* simple clock eviction */
};

/* ------------------------------------------------------------------ */
/* Forward declarations (internal)                                     */
/* ------------------------------------------------------------------ */

static int ntfs_read_sectors(struct ntfs_vol *vol, UINT64 sector,
                             UINT32 count, void *buf);
static int ntfs_read_clusters(struct ntfs_vol *vol, UINT64 cluster,
                              UINT32 count, void *buf);
static int ntfs_apply_fixup(UINT8 *buf, UINT32 record_size,
                            UINT32 sector_size);
static int ntfs_parse_data_runs(const UINT8 *runs, UINT32 runs_len,
                                struct ntfs_extent *extents, int max_extents);
static int ntfs_read_mft_record(struct ntfs_vol *vol, UINT64 record_num,
                                UINT8 *buf);
static UINT8 *ntfs_find_attr(UINT8 *mft_buf, UINT32 mft_size, UINT32 type,
                             const UINT16 *name, UINT8 name_len);
static UINT8 *ntfs_find_attr_next(UINT8 *mft_buf, UINT32 mft_size,
                                  UINT32 type, const UINT16 *name,
                                  UINT8 name_len, UINT8 *after);
static int ntfs_read_attr_data(struct ntfs_vol *vol, UINT8 *attr,
                               UINT8 **out_data, UINT64 *out_size);
static INT64 ntfs_resolve_path(struct ntfs_vol *vol, const char *path);
static int ntfs_utf16_to_ascii(const UINT16 *src, int src_len,
                               char *dst, int dst_max);
static int ntfs_ascii_icmp(const char *a, const char *b);
static int ntfs_name_icmp_utf16(const UINT16 *uname, int ulen,
                                const char *ascii);
static void ntfs_sort_entries(struct fs_entry *entries, int count);

/* ------------------------------------------------------------------ */
/* Utility: read little-endian values from unaligned buffer            */
/* ARM alignment safety: use mem_copy for multi-byte reads             */
/* ------------------------------------------------------------------ */

static UINT16 rd16(const void *p)
{
    UINT16 v;
    mem_copy(&v, p, 2);
    return v;
}

static UINT32 rd32(const void *p)
{
    UINT32 v;
    mem_copy(&v, p, 4);
    return v;
}

static UINT64 rd64(const void *p)
{
    UINT64 v;
    mem_copy(&v, p, 8);
    return v;
}

static INT8 rd_s8(const void *p)
{
    INT8 v;
    mem_copy(&v, p, 1);
    return v;
}

/* ------------------------------------------------------------------ */
/* Sector cache                                                        */
/* ------------------------------------------------------------------ */

static void ntfs_cache_init(struct ntfs_vol *vol)
{
    mem_set(vol->cache, 0, sizeof(vol->cache));
    vol->cache_clock = 0;
}

static void ntfs_cache_free(struct ntfs_vol *vol)
{
    for (int i = 0; i < NTFS_CACHE_SIZE; i++) {
        if (vol->cache[i].data) {
            mem_free(vol->cache[i].data);
            vol->cache[i].data = 0;
            vol->cache[i].valid = 0;
        }
    }
}

/* Read a single device-block-sized sector through the cache */
static int ntfs_cached_read_block(struct ntfs_vol *vol, UINT64 sector,
                                  UINT8 **out)
{
    /* Check cache */
    for (int i = 0; i < NTFS_CACHE_SIZE; i++) {
        if (vol->cache[i].valid && vol->cache[i].sector == sector) {
            *out = vol->cache[i].data;
            return 0;
        }
    }

    /* Cache miss — find slot (simple clock) */
    int slot = vol->cache_clock;
    vol->cache_clock = (vol->cache_clock + 1) % NTFS_CACHE_SIZE;

    if (!vol->cache[slot].data) {
        vol->cache[slot].data = (UINT8 *)mem_alloc(vol->dev_block_size);
        if (!vol->cache[slot].data)
            return -1;
    }

    /* Convert NTFS sector to device blocks */
    UINT64 dev_lba;
    if (vol->dev_block_size == vol->bytes_per_sector) {
        dev_lba = sector;
    } else if (vol->dev_block_size < vol->bytes_per_sector) {
        /* Multiple device blocks per NTFS sector -- not using cache for this */
        dev_lba = sector * (vol->bytes_per_sector / vol->dev_block_size);
    } else {
        /* Device blocks larger than NTFS sector -- read one device block */
        dev_lba = (sector * vol->bytes_per_sector) / vol->dev_block_size;
    }

    if (vol->read_fn(vol->ctx, dev_lba, 1, vol->cache[slot].data) != 0)
        return -1;

    vol->cache[slot].sector = sector;
    vol->cache[slot].valid = 1;
    *out = vol->cache[slot].data;
    return 0;
}

/* Invalidate entire cache (e.g., if we switch volumes) */
static void ntfs_cache_invalidate(struct ntfs_vol *vol)
{
    for (int i = 0; i < NTFS_CACHE_SIZE; i++)
        vol->cache[i].valid = 0;
}

/* ------------------------------------------------------------------ */
/* Low-level I/O                                                       */
/* ------------------------------------------------------------------ */

/*
 * Read 'count' NTFS sectors starting at 'sector' into 'buf'.
 * Handles device block size != NTFS sector size.
 */
static int ntfs_read_sectors(struct ntfs_vol *vol, UINT64 sector,
                             UINT32 count, void *buf)
{
    UINT8 *dst = (UINT8 *)buf;
    UINT32 bps = vol->bytes_per_sector;

    if (vol->dev_block_size == bps) {
        /* 1:1 mapping -- read directly */
        return vol->read_fn(vol->ctx, sector, count, buf);
    }

    if (vol->dev_block_size < bps) {
        /* Multiple device blocks per NTFS sector */
        UINT32 ratio = bps / vol->dev_block_size;
        UINT64 dev_lba = sector * ratio;
        UINT32 dev_count = count * ratio;
        return vol->read_fn(vol->ctx, dev_lba, dev_count, buf);
    }

    /* Device blocks larger than NTFS sector -- rare but handle it */
    /* Read each sector individually, extracting from device block */
    for (UINT32 i = 0; i < count; i++) {
        UINT64 s = sector + i;
        UINT64 byte_offset = s * bps;
        UINT64 dev_lba = byte_offset / vol->dev_block_size;
        UINT32 off_in_block = (UINT32)(byte_offset % vol->dev_block_size);

        UINT8 *cached;
        if (ntfs_cached_read_block(vol, dev_lba, &cached) != 0)
            return -1;
        mem_copy(dst + i * bps, cached + off_in_block, bps);
    }
    return 0;
}

/*
 * Read 'count' clusters starting at 'cluster' into 'buf'.
 */
static int ntfs_read_clusters(struct ntfs_vol *vol, UINT64 cluster,
                              UINT32 count, void *buf)
{
    UINT64 sector = cluster * vol->sectors_per_cluster;
    UINT32 nsectors = count * vol->sectors_per_cluster;
    return ntfs_read_sectors(vol, sector, nsectors, buf);
}

/*
 * Read arbitrary byte range from disk (cluster-aligned start, byte length).
 * Reads starting at byte offset 'byte_off' for 'byte_len' bytes.
 */
static int ntfs_read_bytes(struct ntfs_vol *vol, UINT64 byte_off,
                           UINT64 byte_len, void *buf)
{
    UINT32 bps = vol->bytes_per_sector;
    UINT64 start_sector = byte_off / bps;
    UINT32 off_in_sector = (UINT32)(byte_off % bps);

    if (off_in_sector == 0 && (byte_len % bps) == 0) {
        /* Aligned read */
        return ntfs_read_sectors(vol, start_sector,
                                 (UINT32)(byte_len / bps), buf);
    }

    /* Unaligned -- read sector by sector */
    UINT8 *dst = (UINT8 *)buf;
    UINT64 remaining = byte_len;
    UINT64 cur_sector = start_sector;
    UINT32 cur_off = off_in_sector;

    /* Allocate a temp sector buffer */
    UINT8 *tmp = (UINT8 *)mem_alloc(bps);
    if (!tmp) return -1;

    while (remaining > 0) {
        if (ntfs_read_sectors(vol, cur_sector, 1, tmp) != 0) {
            mem_free(tmp);
            return -1;
        }
        UINT32 avail = bps - cur_off;
        UINT32 chunk = (remaining < avail) ? (UINT32)remaining : avail;
        mem_copy(dst, tmp + cur_off, chunk);
        dst += chunk;
        remaining -= chunk;
        cur_sector++;
        cur_off = 0;
    }

    mem_free(tmp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Update Sequence Array (fixup)                                       */
/* ------------------------------------------------------------------ */

/*
 * Apply fixup to an MFT record or INDX block that has been read into buf.
 * record_size = total size of the record (e.g. 1024 for MFT, 4096 for INDX).
 * sector_size = NTFS logical sector size (bytes_per_sector from BPB).
 * Returns 0 on success, -1 on corruption.
 */
static int ntfs_apply_fixup(UINT8 *buf, UINT32 record_size,
                            UINT32 sector_size)
{
    UINT16 usa_offset = rd16(buf + 4);
    UINT16 usa_count = rd16(buf + 6);

    /* usa_count includes the update sequence value itself */
    if (usa_count < 2)
        return 0; /* nothing to fix */

    /* Number of sectors in this record */
    UINT32 nsectors = record_size / sector_size;

    /* usa_count - 1 should equal nsectors */
    if ((UINT32)(usa_count - 1) != nsectors)
        return -1;

    /* Bounds check: USA must fit within record */
    if (usa_offset + usa_count * 2 > record_size)
        return -1;

    /* Read the update sequence value */
    UINT16 usv = rd16(buf + usa_offset);

    /* For each sector, verify the last two bytes match usv,
       then replace with the corresponding fixup value */
    for (UINT32 i = 0; i < nsectors; i++) {
        UINT32 last2 = (i + 1) * sector_size - 2;
        UINT16 orig = rd16(buf + last2);

        if (orig != usv)
            return -1; /* corrupt record */

        /* Replacement value is at usa_offset + 2 + i*2 */
        UINT16 replacement = rd16(buf + usa_offset + 2 + i * 2);
        mem_copy(buf + last2, &replacement, 2);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Data run decoding                                                   */
/* ------------------------------------------------------------------ */

/*
 * Parse a data run list from raw bytes into an array of extents.
 * Returns number of extents parsed, or -1 on error.
 *
 * Data runs are a compressed encoding:
 *   byte 0: header — low nibble = length_size, high nibble = offset_size
 *   next length_size bytes: cluster count (unsigned LE)
 *   next offset_size bytes: cluster offset delta (signed LE)
 *   ...repeat until header byte = 0x00
 *
 * Offsets are delta-encoded relative to the previous run's starting LCN.
 */
static int ntfs_parse_data_runs(const UINT8 *runs, UINT32 runs_len,
                                struct ntfs_extent *extents, int max_extents)
{
    int count = 0;
    UINT32 pos = 0;
    INT64 prev_lcn = 0;
    UINT64 vcn = 0;

    while (pos < runs_len && count < max_extents) {
        UINT8 header = runs[pos];
        if (header == 0)
            break; /* end of runs */

        UINT8 len_size = header & 0x0F;
        UINT8 off_size = (header >> 4) & 0x0F;
        pos++;

        /* Validate sizes */
        if (len_size == 0 || len_size > 8)
            return -1;
        if (off_size > 8)
            return -1;
        if (pos + len_size + off_size > runs_len)
            return -1;

        /* Read length (unsigned) */
        UINT64 run_length = 0;
        for (UINT8 i = 0; i < len_size; i++)
            run_length |= ((UINT64)runs[pos + i]) << (i * 8);
        pos += len_size;

        /* Read offset (signed, delta from previous) */
        INT64 run_offset = 0;
        if (off_size > 0) {
            for (UINT8 i = 0; i < off_size; i++)
                run_offset |= ((UINT64)runs[pos + i]) << (i * 8);

            /* Sign extend */
            if (runs[pos + off_size - 1] & 0x80) {
                for (UINT8 i = off_size; i < 8; i++)
                    run_offset |= ((UINT64)0xFF) << (i * 8);
            }
            pos += off_size;
        }

        /* off_size == 0 means sparse run (no LCN on disk).
           We still record it but with LCN = 0, caller must handle. */
        INT64 lcn;
        if (off_size == 0) {
            lcn = 0; /* sparse */
        } else {
            lcn = prev_lcn + run_offset;
            prev_lcn = lcn;
        }

        extents[count].vcn = vcn;
        extents[count].lcn = (UINT64)lcn;
        extents[count].length = run_length;
        count++;

        vcn += run_length;
    }

    return count;
}

/*
 * Read data described by a set of extents.  Reads up to data_size bytes
 * into buf (which must be pre-allocated to at least data_size bytes).
 * Returns 0 on success, -1 on error.
 */
static int ntfs_read_data_from_extents(struct ntfs_vol *vol,
                                       struct ntfs_extent *extents,
                                       int extent_count,
                                       UINT64 data_size, void *buf)
{
    UINT8 *dst = (UINT8 *)buf;
    UINT64 remaining = data_size;
    UINT32 bpc = vol->bytes_per_cluster;

    for (int i = 0; i < extent_count && remaining > 0; i++) {
        UINT64 run_bytes = extents[i].length * bpc;
        UINT64 to_read = (remaining < run_bytes) ? remaining : run_bytes;

        if (extents[i].lcn == 0 && extents[i].length > 0) {
            /* Sparse run — fill with zeros */
            UINT64 fill = to_read;
            mem_set(dst, 0, (UINTN)fill);
            dst += fill;
            remaining -= fill;
            continue;
        }

        /* Read cluster by cluster to handle large runs without huge
           intermediate buffers (the block read fn may have limits) */
        UINT64 clusters_to_read = (to_read + bpc - 1) / bpc;
        UINT64 bytes_read = 0;

        /* Read in chunks of up to 64 clusters to reduce call overhead */
        UINT64 lcn = extents[i].lcn;
        UINT64 clust_remaining = clusters_to_read;

        while (clust_remaining > 0 && remaining > 0) {
            UINT32 chunk = (clust_remaining > 64) ? 64 :
                           (UINT32)clust_remaining;

            /* For the last chunk, we might read more than remaining,
               so read into temp if partial cluster needed */
            UINT64 chunk_bytes = (UINT64)chunk * bpc;
            UINT64 want = (remaining < chunk_bytes) ? remaining : chunk_bytes;

            if (want == chunk_bytes) {
                /* Full cluster-aligned read */
                if (ntfs_read_clusters(vol, lcn, chunk, dst) != 0)
                    return -1;
            } else {
                /* Partial last cluster — need temp buffer */
                UINT8 *tmp = (UINT8 *)mem_alloc(chunk_bytes);
                if (!tmp) return -1;
                if (ntfs_read_clusters(vol, lcn, chunk, tmp) != 0) {
                    mem_free(tmp);
                    return -1;
                }
                mem_copy(dst, tmp, (UINTN)want);
                mem_free(tmp);
            }

            dst += want;
            remaining -= want;
            lcn += chunk;
            clust_remaining -= chunk;
            bytes_read += want;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* MFT record reading                                                  */
/* ------------------------------------------------------------------ */

/*
 * Read MFT record by record number.  Uses the cached $MFT extents
 * to locate the record on disk.  buf must be at least mft_record_size.
 * Returns 0 on success, -1 on error.
 */
static int ntfs_read_mft_record(struct ntfs_vol *vol, UINT64 record_num,
                                UINT8 *buf)
{
    UINT32 rec_size = vol->mft_record_size;
    UINT32 bpc = vol->bytes_per_cluster;

    /* Calculate byte offset of this record within $MFT data */
    UINT64 byte_offset = record_num * rec_size;

    /* Find which extent contains this byte offset */
    UINT64 cluster_offset = byte_offset / bpc;
    UINT32 off_in_cluster = (UINT32)(byte_offset % bpc);

    /* Walk the MFT extents to find the cluster on disk */
    UINT64 vcn_start = 0;
    int found = 0;

    for (int i = 0; i < vol->mft_extent_count; i++) {
        UINT64 ext_vcn = vol->mft_extents[i].vcn;
        UINT64 ext_len = vol->mft_extents[i].length;
        UINT64 ext_lcn = vol->mft_extents[i].lcn;

        if (cluster_offset >= ext_vcn &&
            cluster_offset < ext_vcn + ext_len) {
            /* Found it */
            UINT64 delta = cluster_offset - ext_vcn;
            UINT64 disk_cluster = ext_lcn + delta;

            /* The MFT record might span cluster boundaries if
               off_in_cluster + rec_size > bpc. Handle this. */
            UINT32 avail_in_cluster = bpc - off_in_cluster;

            if (avail_in_cluster >= rec_size) {
                /* Entire record fits in this cluster */
                UINT64 disk_byte = disk_cluster * bpc + off_in_cluster;
                if (ntfs_read_bytes(vol, disk_byte, rec_size, buf) != 0)
                    return -1;
            } else {
                /* Record spans clusters — read first part */
                UINT64 disk_byte = disk_cluster * bpc + off_in_cluster;
                if (ntfs_read_bytes(vol, disk_byte, avail_in_cluster,
                                    buf) != 0)
                    return -1;

                /* Read remaining from next cluster(s) */
                UINT32 remaining = rec_size - avail_in_cluster;
                UINT64 next_cluster_offset = cluster_offset + 1;
                UINT8 *dst = buf + avail_in_cluster;

                /* The next cluster might be in a different extent */
                while (remaining > 0) {
                    /* Find extent for next_cluster_offset */
                    int fi = 0;
                    for (int j = 0; j < vol->mft_extent_count; j++) {
                        if (next_cluster_offset >= vol->mft_extents[j].vcn &&
                            next_cluster_offset < vol->mft_extents[j].vcn +
                            vol->mft_extents[j].length) {
                            UINT64 d2 = next_cluster_offset -
                                        vol->mft_extents[j].vcn;
                            UINT64 dc2 = vol->mft_extents[j].lcn + d2;
                            UINT64 db2 = dc2 * bpc;
                            UINT32 chunk = (remaining < bpc) ?
                                           remaining : bpc;
                            if (ntfs_read_bytes(vol, db2, chunk, dst) != 0)
                                return -1;
                            dst += chunk;
                            remaining -= chunk;
                            next_cluster_offset++;
                            fi = 1;
                            break;
                        }
                    }
                    if (!fi)
                        return -1; /* extent not found */
                }
            }

            found = 1;
            break;
        }
    }

    if (!found)
        return -1;

    /* Verify signature "FILE" */
    if (buf[0] != 'F' || buf[1] != 'I' || buf[2] != 'L' || buf[3] != 'E')
        return -1;

    /* Apply fixup */
    if (ntfs_apply_fixup(buf, rec_size, vol->bytes_per_sector) != 0)
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Attribute search within an MFT record                               */
/* ------------------------------------------------------------------ */

/*
 * Find the first attribute of given type in an MFT record buffer.
 * If name is not NULL, also match the attribute name (UTF-16).
 * Returns pointer to the attribute header, or NULL if not found.
 */
static UINT8 *ntfs_find_attr(UINT8 *mft_buf, UINT32 mft_size, UINT32 type,
                             const UINT16 *name, UINT8 name_len)
{
    return ntfs_find_attr_next(mft_buf, mft_size, type, name, name_len, NULL);
}

/*
 * Find the next attribute of given type after 'after' (or first if NULL).
 */
static UINT8 *ntfs_find_attr_next(UINT8 *mft_buf, UINT32 mft_size,
                                  UINT32 type, const UINT16 *name,
                                  UINT8 name_len, UINT8 *after)
{
    UINT16 first_off = rd16(mft_buf + 20); /* first_attr_offset */
    UINT32 used_size = rd32(mft_buf + 24);

    if (first_off >= mft_size || used_size > mft_size)
        return 0;
    if (used_size < first_off)
        return 0;

    UINT32 pos;
    if (after) {
        /* Start after 'after' */
        UINT32 after_off = (UINT32)(after - mft_buf);
        UINT32 after_len = rd32(after + 4);
        if (after_len < 16 || after_len > mft_size)
            return 0;
        pos = after_off + after_len;
    } else {
        pos = first_off;
    }

    while (pos + 16 <= used_size) {
        UINT32 attr_type = rd32(mft_buf + pos);
        if (attr_type == NTFS_AT_END)
            break;

        UINT32 attr_len = rd32(mft_buf + pos + 4);
        if (attr_len < 16 || attr_len > mft_size - pos)
            break;

        if (attr_type == type) {
            /* Check name match if requested */
            UINT8 attr_nlen = mft_buf[pos + 9];
            UINT16 attr_noff = rd16(mft_buf + pos + 10);

            if (name == 0 && name_len == 0) {
                /* Looking for unnamed attribute */
                if (attr_nlen == 0)
                    return mft_buf + pos;
            } else if (name != 0 && attr_nlen == name_len) {
                /* Compare names */
                UINT16 *attr_name = (UINT16 *)(mft_buf + pos + attr_noff);
                int match = 1;
                for (UINT8 k = 0; k < name_len; k++) {
                    UINT16 a = rd16((UINT8 *)attr_name + k * 2);
                    UINT16 b = rd16((UINT8 *)name + k * 2);
                    if (a != b) { match = 0; break; }
                }
                if (match)
                    return mft_buf + pos;
            }
        }

        pos += attr_len;
    }

    return 0;
}

/*
 * Find any attribute of given type regardless of name.
 */
static UINT8 *ntfs_find_attr_any(UINT8 *mft_buf, UINT32 mft_size,
                                 UINT32 type)
{
    UINT16 first_off = rd16(mft_buf + 20);
    UINT32 used_size = rd32(mft_buf + 24);

    if (first_off >= mft_size || used_size > mft_size)
        return 0;

    UINT32 pos = first_off;

    while (pos + 16 <= used_size) {
        UINT32 attr_type = rd32(mft_buf + pos);
        if (attr_type == NTFS_AT_END)
            break;

        UINT32 attr_len = rd32(mft_buf + pos + 4);
        if (attr_len < 16 || attr_len > mft_size - pos)
            break;

        if (attr_type == type)
            return mft_buf + pos;

        pos += attr_len;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Read attribute data (resident or non-resident)                      */
/* ------------------------------------------------------------------ */

/*
 * Read the data of an attribute.  For resident attributes, copies the
 * value.  For non-resident, decodes data runs and reads from disk.
 *
 * Allocates *out_data (caller must mem_free).  Sets *out_size.
 * Returns 0 on success, -1 on error.
 */
static int ntfs_read_attr_data(struct ntfs_vol *vol, UINT8 *attr,
                               UINT8 **out_data, UINT64 *out_size)
{
    UINT8 non_resident = attr[8];
    UINT32 attr_len = rd32(attr + 4);

    if (!non_resident) {
        /* Resident */
        UINT32 val_len = rd32(attr + 16);
        UINT16 val_off = rd16(attr + 20);

        if (val_off + val_len > attr_len)
            return -1;

        UINT8 *data = (UINT8 *)mem_alloc(val_len + 1);
        if (!data) return -1;

        mem_copy(data, attr + val_off, val_len);
        data[val_len] = 0; /* null-terminate for convenience */

        *out_data = data;
        *out_size = val_len;
        return 0;
    }

    /* Non-resident */
    UINT16 runs_off = rd16(attr + 32);
    UINT64 real_size = rd64(attr + 48);

    if (runs_off >= attr_len)
        return -1;

    UINT32 runs_len = attr_len - runs_off;
    const UINT8 *runs = attr + runs_off;

    /* Parse data runs */
    struct ntfs_extent *extents = (struct ntfs_extent *)
        mem_alloc(NTFS_MAX_RUNS * sizeof(struct ntfs_extent));
    if (!extents) return -1;

    int ext_count = ntfs_parse_data_runs(runs, runs_len,
                                         extents, NTFS_MAX_RUNS);
    if (ext_count <= 0) {
        mem_free(extents);
        return -1;
    }

    /* Allocate buffer for the data */
    UINT8 *data = (UINT8 *)mem_alloc((UINTN)real_size + 1);
    if (!data) {
        mem_free(extents);
        return -1;
    }

    if (ntfs_read_data_from_extents(vol, extents, ext_count,
                                    real_size, data) != 0) {
        mem_free(data);
        mem_free(extents);
        return -1;
    }

    data[real_size] = 0;
    mem_free(extents);

    *out_data = data;
    *out_size = real_size;
    return 0;
}

/*
 * Read non-resident attribute data into a pre-allocated buffer.
 * Only reads up to buf_size bytes.
 * Returns 0 on success, -1 on error.
 */
static int ntfs_read_attr_data_into(struct ntfs_vol *vol, UINT8 *attr,
                                    void *buf, UINT64 buf_size)
{
    UINT8 non_resident = attr[8];
    UINT32 attr_len = rd32(attr + 4);

    if (!non_resident) {
        UINT32 val_len = rd32(attr + 16);
        UINT16 val_off = rd16(attr + 20);
        if (val_off + val_len > attr_len)
            return -1;
        UINT64 to_copy = (val_len < buf_size) ? val_len : buf_size;
        mem_copy(buf, attr + val_off, (UINTN)to_copy);
        return 0;
    }

    UINT16 runs_off = rd16(attr + 32);
    UINT64 real_size = rd64(attr + 48);
    if (runs_off >= attr_len)
        return -1;

    UINT32 runs_len = attr_len - runs_off;
    const UINT8 *runs = attr + runs_off;

    struct ntfs_extent *extents = (struct ntfs_extent *)
        mem_alloc(NTFS_MAX_RUNS * sizeof(struct ntfs_extent));
    if (!extents) return -1;

    int ext_count = ntfs_parse_data_runs(runs, runs_len,
                                         extents, NTFS_MAX_RUNS);
    if (ext_count <= 0) {
        mem_free(extents);
        return -1;
    }

    UINT64 to_read = (real_size < buf_size) ? real_size : buf_size;
    int rc = ntfs_read_data_from_extents(vol, extents, ext_count,
                                         to_read, buf);
    mem_free(extents);
    return rc;
}

/* ------------------------------------------------------------------ */
/* UTF-16 / name utilities                                             */
/* ------------------------------------------------------------------ */

/*
 * Convert UTF-16LE to ASCII.  Characters outside 0x20..0x7E become '?'.
 * Returns number of ASCII chars written (not counting NUL).
 */
static int ntfs_utf16_to_ascii(const UINT16 *src, int src_len,
                               char *dst, int dst_max)
{
    int i, j = 0;
    for (i = 0; i < src_len && j < dst_max - 1; i++) {
        UINT16 ch = rd16((const UINT8 *)src + i * 2);
        if (ch >= 0x20 && ch <= 0x7E)
            dst[j++] = (char)ch;
        else if (ch == 0)
            break;
        else
            dst[j++] = '?';
    }
    dst[j] = '\0';
    return j;
}

/*
 * Case-insensitive ASCII comparison. Returns 0 if equal.
 */
static int ntfs_ascii_icmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)(UINT8)ca - (int)(UINT8)cb;
        a++; b++;
    }
    return (int)(UINT8)*a - (int)(UINT8)*b;
}

/*
 * Compare a UTF-16LE name (from disk) to an ASCII name, case-insensitive.
 * Returns 0 if equal.
 */
static int ntfs_name_icmp_utf16(const UINT16 *uname, int ulen,
                                const char *ascii)
{
    int alen = (int)str_len((const CHAR8 *)ascii);
    if (ulen != alen)
        return 1;

    for (int i = 0; i < ulen; i++) {
        UINT16 uc = rd16((const UINT8 *)uname + i * 2);
        char ac = ascii[i];

        /* Lowercase both */
        char u8;
        if (uc >= 0x20 && uc <= 0x7E)
            u8 = (char)uc;
        else
            return 1; /* non-ASCII char, can't match */

        if (u8 >= 'A' && u8 <= 'Z') u8 += 32;
        if (ac >= 'A' && ac <= 'Z') ac += 32;
        if (u8 != ac)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Sort directory entries                                               */
/* ------------------------------------------------------------------ */

static void ntfs_sort_entries(struct fs_entry *entries, int count)
{
    /* Insertion sort: directories first, then alphabetical */
    for (int i = 1; i < count; i++) {
        struct fs_entry tmp;
        mem_copy(&tmp, &entries[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0) {
            int swap = 0;
            if (tmp.is_dir && !entries[j].is_dir) {
                swap = 1;
            } else if (tmp.is_dir == entries[j].is_dir) {
                if (ntfs_ascii_icmp(tmp.name, entries[j].name) < 0)
                    swap = 1;
            }
            if (!swap) break;
            mem_copy(&entries[j + 1], &entries[j], sizeof(tmp));
            j--;
        }
        mem_copy(&entries[j + 1], &tmp, sizeof(tmp));
    }
}

/* ------------------------------------------------------------------ */
/* Directory reading (index entries)                                   */
/* ------------------------------------------------------------------ */

/*
 * State for collecting directory entries with deduplication.
 * We prefer Win32 or Win32+DOS names over DOS-only names.
 */
struct ntfs_dir_state {
    struct fs_entry *entries;
    int              count;
    int              max_entries;

    /* Dedup array: MFT reference of each entry already added */
    UINT64          *mft_refs;
    UINT8           *namespaces; /* namespace of each added entry */
};

/*
 * Add an entry to the dir_state, handling dedup.
 * Returns 0 on success (or skip), -1 on error.
 */
static int ntfs_dir_add_entry(struct ntfs_dir_state *state,
                              UINT64 mft_ref_raw,
                              const UINT8 *fn_data, UINT32 fn_len)
{
    /* fn_data points to a $FILE_NAME structure */
    if (fn_len < 66)
        return 0; /* too short */

    UINT8 name_length = fn_data[64];
    UINT8 name_ns = fn_data[65];

    /* Skip . and .. (parent ref == self or name is "." ) */
    if (name_length == 1) {
        UINT16 ch = rd16(fn_data + 66);
        if (ch == '.')
            return 0; /* skip "." */
    }
    if (name_length == 2) {
        UINT16 c1 = rd16(fn_data + 66);
        UINT16 c2 = rd16(fn_data + 68);
        if (c1 == '.' && c2 == '.')
            return 0; /* skip ".." */
    }

    /* Extract MFT record number from reference (low 6 bytes) */
    UINT64 mft_num = mft_ref_raw & 0x0000FFFFFFFFFFFFULL;

    /* Check for duplicate MFT ref */
    for (int i = 0; i < state->count; i++) {
        if (state->mft_refs[i] == mft_num) {
            /* Duplicate. Prefer Win32/Win32+DOS over DOS. */
            if (name_ns == NTFS_NS_DOS)
                return 0; /* already have a better name */

            if (state->namespaces[i] == NTFS_NS_DOS &&
                (name_ns == NTFS_NS_WIN32 || name_ns == NTFS_NS_WIN32_DOS)) {
                /* Replace the DOS name with Win32 name */
                UINT32 flags = rd32(fn_data + 56);
                UINT64 real_size = rd64(fn_data + 48);

                state->entries[i].is_dir =
                    (flags & NTFS_FILE_ATTR_DIRECTORY) ? 1 : 0;
                state->entries[i].size = state->entries[i].is_dir ?
                                         0 : real_size;
                ntfs_utf16_to_ascii((const UINT16 *)(fn_data + 66),
                                    name_length,
                                    state->entries[i].name, FS_MAX_NAME);
                state->namespaces[i] = name_ns;
                return 0;
            }

            /* Otherwise keep existing (it's already Win32 or POSIX) */
            return 0;
        }
    }

    /* Skip pure DOS names if we might see a Win32 name later?
       No — we collect everything and dedup above handles it.
       But we DO skip DOS-only to avoid clutter if we already
       have entries. Actually, we should add it and let dedup
       handle replacement if a Win32 name comes later. */

    if (state->count >= state->max_entries)
        return 0; /* overflow, silently drop */

    int idx = state->count;
    UINT32 flags = rd32(fn_data + 56);
    UINT64 real_size = rd64(fn_data + 48);

    state->entries[idx].is_dir =
        (flags & NTFS_FILE_ATTR_DIRECTORY) ? 1 : 0;
    state->entries[idx].size = state->entries[idx].is_dir ? 0 : real_size;
    ntfs_utf16_to_ascii((const UINT16 *)(fn_data + 66),
                        name_length,
                        state->entries[idx].name, FS_MAX_NAME);
    state->mft_refs[idx] = mft_num;
    state->namespaces[idx] = name_ns;
    state->count++;

    return 0;
}

/*
 * Parse index entries from a buffer (used for both INDEX_ROOT and INDX).
 * entries_buf points to the start of the first index entry.
 * entries_size is the total size of all entries.
 */
static int ntfs_parse_index_entries(struct ntfs_dir_state *state,
                                    UINT8 *entries_buf,
                                    UINT32 entries_size)
{
    UINT32 pos = 0;

    while (pos + 16 <= entries_size) {
        UINT64 mft_ref = rd64(entries_buf + pos);
        UINT16 entry_len = rd16(entries_buf + pos + 8);
        UINT16 stream_len = rd16(entries_buf + pos + 10);
        UINT32 entry_flags = rd32(entries_buf + pos + 12);

        if (entry_len < 16)
            break; /* malformed */

        if (entry_flags & NTFS_INDEX_ENTRY_LAST)
            break; /* last entry (no data) */

        /* The stream data ($FILE_NAME) starts at offset 16 */
        if (stream_len > 0 && pos + 16 + stream_len <= entries_size) {
            ntfs_dir_add_entry(state, mft_ref,
                              entries_buf + pos + 16, stream_len);
        }

        if (entry_len == 0)
            break;
        pos += entry_len;
    }

    return 0;
}

/*
 * Parse an INDX block: verify signature, apply fixup, parse entries.
 */
static int ntfs_parse_indx_block(struct ntfs_vol *vol,
                                 struct ntfs_dir_state *state,
                                 UINT8 *block_buf, UINT32 block_size)
{
    /* Verify "INDX" signature */
    if (block_buf[0] != 'I' || block_buf[1] != 'N' ||
        block_buf[2] != 'D' || block_buf[3] != 'X')
        return -1;

    /* Apply fixup */
    if (ntfs_apply_fixup(block_buf, block_size,
                         vol->bytes_per_sector) != 0)
        return -1;

    /* Index node header is at offset 24 */
    UINT32 entries_offset = rd32(block_buf + 24);
    UINT32 total_size = rd32(block_buf + 28);

    /* Entries start at 24 + entries_offset */
    UINT32 entries_start = 24 + entries_offset;
    if (entries_start >= block_size)
        return -1;

    UINT32 entries_size = total_size;
    if (entries_start + entries_size > block_size)
        entries_size = block_size - entries_start;

    return ntfs_parse_index_entries(state, block_buf + entries_start,
                                   entries_size);
}

/*
 * Read all directory entries from an MFT record (by record number).
 * Handles both small (INDEX_ROOT only) and large (INDEX_ROOT + INDEX_ALLOCATION)
 * directories.
 *
 * Returns entry count or -1 on error.
 */
static int ntfs_read_dir_entries(struct ntfs_vol *vol, UINT64 mft_num,
                                 struct fs_entry *entries, int max_entries)
{
    UINT8 *mft_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
    if (!mft_buf)
        return -1;

    if (ntfs_read_mft_record(vol, mft_num, mft_buf) != 0) {
        mem_free(mft_buf);
        return -1;
    }

    /* Verify this is a directory */
    UINT16 mft_flags = rd16(mft_buf + 22);
    if (!(mft_flags & NTFS_MFT_DIRECTORY)) {
        mem_free(mft_buf);
        return -1;
    }

    /* Set up dir state with dedup arrays */
    int alloc_max = (max_entries < NTFS_MAX_DIR_ENTRIES) ?
                    NTFS_MAX_DIR_ENTRIES : max_entries;

    struct ntfs_dir_state state;
    state.entries = entries;
    state.count = 0;
    state.max_entries = max_entries;

    state.mft_refs = (UINT64 *)mem_alloc(alloc_max * sizeof(UINT64));
    state.namespaces = (UINT8 *)mem_alloc(alloc_max * sizeof(UINT8));

    if (!state.mft_refs || !state.namespaces) {
        if (state.mft_refs) mem_free(state.mft_refs);
        if (state.namespaces) mem_free(state.namespaces);
        mem_free(mft_buf);
        return -1;
    }
    mem_set(state.mft_refs, 0, alloc_max * sizeof(UINT64));
    mem_set(state.namespaces, 0, alloc_max * sizeof(UINT8));

    /* --- Parse INDEX_ROOT (0x90) — named "$I30" --- */
    UINT8 *ir_attr = ntfs_find_attr_any(mft_buf, vol->mft_record_size,
                                        NTFS_AT_INDEX_ROOT);
    int has_large_index = 0;

    if (ir_attr) {
        /* INDEX_ROOT is always resident */
        UINT32 val_len = rd32(ir_attr + 16);
        UINT16 val_off = rd16(ir_attr + 20);
        UINT8 *ir_val = ir_attr + val_off;

        if (val_len >= 32) {
            /* Index node header starts at offset 16 within the value */
            UINT32 node_entries_off = rd32(ir_val + 16);
            UINT32 node_total_size = rd32(ir_val + 20);
            UINT32 node_flags = rd32(ir_val + 28);

            has_large_index = (node_flags & 0x01) ? 1 : 0;

            /* Entries start at ir_val + 16 + node_entries_off */
            UINT32 entries_start = 16 + node_entries_off;
            if (entries_start < val_len) {
                UINT32 entries_size = node_total_size;
                if (entries_start + entries_size > val_len)
                    entries_size = val_len - entries_start;

                ntfs_parse_index_entries(&state,
                                        ir_val + entries_start,
                                        entries_size);
            }
        }
    }

    /* --- Parse INDEX_ALLOCATION (0xA0) if present --- */
    if (has_large_index) {
        UINT8 *ia_attr = ntfs_find_attr_any(mft_buf, vol->mft_record_size,
                                             NTFS_AT_INDEX_ALLOCATION);
        if (ia_attr && ia_attr[8]) { /* must be non-resident */
            UINT16 runs_off = rd16(ia_attr + 32);
            UINT64 real_size = rd64(ia_attr + 48);
            UINT32 ia_attr_len = rd32(ia_attr + 4);

            if (runs_off < ia_attr_len) {
                UINT32 runs_len = ia_attr_len - runs_off;
                const UINT8 *runs = ia_attr + runs_off;

                struct ntfs_extent *extents = (struct ntfs_extent *)
                    mem_alloc(NTFS_MAX_RUNS * sizeof(struct ntfs_extent));

                if (extents) {
                    int ext_count = ntfs_parse_data_runs(runs, runs_len,
                                                         extents,
                                                         NTFS_MAX_RUNS);

                    if (ext_count > 0) {
                        UINT32 ibs = vol->index_block_size;

                        /* Read index blocks and parse each one */
                        /* We'll read one block at a time */
                        UINT8 *ibuf = (UINT8 *)mem_alloc(ibs);

                        if (ibuf) {
                            UINT64 total_read = 0;
                            UINT32 bpc = vol->bytes_per_cluster;

                            for (int e = 0; e < ext_count &&
                                 total_read < real_size; e++) {
                                UINT64 lcn = extents[e].lcn;
                                UINT64 run_len = extents[e].length;

                                if (lcn == 0 && run_len > 0)
                                    continue; /* sparse, skip */

                                UINT64 run_bytes = run_len * bpc;

                                /* How many index blocks in this run? */
                                UINT64 blocks_in_run;
                                if (ibs >= bpc)
                                    blocks_in_run = run_bytes / ibs;
                                else
                                    blocks_in_run = run_bytes / ibs;

                                for (UINT64 b = 0; b < blocks_in_run &&
                                     total_read < real_size; b++) {
                                    UINT64 block_byte_off = b * ibs;
                                    UINT64 block_cluster = lcn +
                                        (block_byte_off / bpc);
                                    UINT32 off_in_clust =
                                        (UINT32)(block_byte_off % bpc);

                                    UINT64 disk_byte = block_cluster * bpc +
                                                       off_in_clust;

                                    if (ntfs_read_bytes(vol, disk_byte,
                                                        ibs, ibuf) != 0)
                                        break;

                                    ntfs_parse_indx_block(vol, &state,
                                                          ibuf, ibs);
                                    total_read += ibs;
                                }
                            }

                            mem_free(ibuf);
                        }
                    }

                    mem_free(extents);
                }
            }
        }
    }

    int result = state.count;

    mem_free(state.mft_refs);
    mem_free(state.namespaces);
    mem_free(mft_buf);

    return result;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

/*
 * Look up a single name component in a directory (given by MFT record number).
 * Returns the MFT record number of the matching entry, or -1 if not found.
 */
static INT64 ntfs_lookup_name(struct ntfs_vol *vol, UINT64 dir_mft,
                              const char *name)
{
    UINT8 *mft_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
    if (!mft_buf)
        return -1;

    if (ntfs_read_mft_record(vol, dir_mft, mft_buf) != 0) {
        mem_free(mft_buf);
        return -1;
    }

    INT64 result = -1;

    /* First search INDEX_ROOT (named "$I30") */
    UINT8 *ir_attr = ntfs_find_attr_any(mft_buf, vol->mft_record_size,
                                        NTFS_AT_INDEX_ROOT);
    int has_large_index = 0;

    if (ir_attr) {
        UINT32 val_len = rd32(ir_attr + 16);
        UINT16 val_off = rd16(ir_attr + 20);
        UINT8 *ir_val = ir_attr + val_off;

        if (val_len >= 32) {
            UINT32 node_entries_off = rd32(ir_val + 16);
            UINT32 node_total_size = rd32(ir_val + 20);
            UINT32 node_flags = rd32(ir_val + 28);
            has_large_index = (node_flags & 0x01) ? 1 : 0;

            UINT32 entries_start = 16 + node_entries_off;
            if (entries_start < val_len) {
                UINT32 entries_size = node_total_size;
                if (entries_start + entries_size > val_len)
                    entries_size = val_len - entries_start;

                /* Walk entries looking for name */
                UINT32 pos = 0;
                UINT8 *eb = ir_val + entries_start;

                while (pos + 16 <= entries_size) {
                    UINT64 mft_ref = rd64(eb + pos);
                    UINT16 entry_len = rd16(eb + pos + 8);
                    UINT16 stream_len = rd16(eb + pos + 10);
                    UINT32 eflags = rd32(eb + pos + 12);

                    if (eflags & NTFS_INDEX_ENTRY_LAST)
                        break;

                    if (stream_len >= 66 && pos + 16 + stream_len <= entries_size) {
                        UINT8 *fn = eb + pos + 16;
                        UINT8 fn_nlen = fn[64];
                        UINT8 fn_ns = fn[65];

                        /* Skip DOS-only if possible, but still check */
                        if (ntfs_name_icmp_utf16(
                                (const UINT16 *)(fn + 66),
                                fn_nlen, name) == 0) {
                            result = (INT64)(mft_ref & 0x0000FFFFFFFFFFFFULL);
                            /* If it's a Win32/POSIX match, use it immediately */
                            if (fn_ns != NTFS_NS_DOS)
                                goto done;
                            /* If DOS, keep looking for Win32 */
                        }
                    }

                    if (entry_len == 0)
                        break;
                    pos += entry_len;
                }
            }
        }
    }

    /* If found in INDEX_ROOT (non-DOS), we're done */
    if (result >= 0 && !has_large_index)
        goto done;

    /* Search INDEX_ALLOCATION if present */
    if (has_large_index) {
        UINT8 *ia_attr = ntfs_find_attr_any(mft_buf, vol->mft_record_size,
                                             NTFS_AT_INDEX_ALLOCATION);
        if (ia_attr && ia_attr[8]) {
            UINT16 runs_off = rd16(ia_attr + 32);
            UINT64 real_size = rd64(ia_attr + 48);
            UINT32 ia_attr_len = rd32(ia_attr + 4);

            if (runs_off < ia_attr_len) {
                UINT32 runs_len = ia_attr_len - runs_off;
                const UINT8 *runs = ia_attr + runs_off;

                struct ntfs_extent *extents = (struct ntfs_extent *)
                    mem_alloc(NTFS_MAX_RUNS * sizeof(struct ntfs_extent));

                if (extents) {
                    int ext_count = ntfs_parse_data_runs(runs, runs_len,
                                                         extents,
                                                         NTFS_MAX_RUNS);

                    if (ext_count > 0) {
                        UINT32 ibs = vol->index_block_size;
                        UINT8 *ibuf = (UINT8 *)mem_alloc(ibs);
                        UINT32 bpc = vol->bytes_per_cluster;

                        if (ibuf) {
                            UINT64 total_read = 0;

                            for (int e = 0; e < ext_count &&
                                 total_read < real_size; e++) {
                                UINT64 lcn = extents[e].lcn;
                                UINT64 run_len = extents[e].length;

                                if (lcn == 0 && run_len > 0) {
                                    total_read += run_len * bpc;
                                    continue;
                                }

                                UINT64 run_bytes = run_len * bpc;
                                UINT64 blocks_in_run = run_bytes / ibs;

                                for (UINT64 b = 0; b < blocks_in_run &&
                                     total_read < real_size; b++) {
                                    UINT64 bbo = b * ibs;
                                    UINT64 bc = lcn + (bbo / bpc);
                                    UINT32 oic = (UINT32)(bbo % bpc);
                                    UINT64 db = bc * bpc + oic;

                                    if (ntfs_read_bytes(vol, db,
                                                        ibs, ibuf) != 0)
                                        break;

                                    /* Verify and fixup INDX block */
                                    if (ibuf[0] != 'I' || ibuf[1] != 'N' ||
                                        ibuf[2] != 'D' || ibuf[3] != 'X') {
                                        total_read += ibs;
                                        continue;
                                    }

                                    if (ntfs_apply_fixup(ibuf, ibs,
                                            vol->bytes_per_sector) != 0) {
                                        total_read += ibs;
                                        continue;
                                    }

                                    /* Parse entries in this INDX block */
                                    UINT32 ie_off = rd32(ibuf + 24);
                                    UINT32 ie_size = rd32(ibuf + 28);
                                    UINT32 ie_start = 24 + ie_off;

                                    if (ie_start < ibs) {
                                        if (ie_start + ie_size > ibs)
                                            ie_size = ibs - ie_start;

                                        UINT32 pos = 0;
                                        UINT8 *eb = ibuf + ie_start;

                                        while (pos + 16 <= ie_size) {
                                            UINT64 mr = rd64(eb + pos);
                                            UINT16 el = rd16(eb + pos + 8);
                                            UINT16 sl = rd16(eb + pos + 10);
                                            UINT32 ef = rd32(eb + pos + 12);

                                            if (ef & NTFS_INDEX_ENTRY_LAST)
                                                break;

                                            if (sl >= 66 &&
                                                pos + 16 + sl <= ie_size) {
                                                UINT8 *fn = eb + pos + 16;
                                                UINT8 fnl = fn[64];
                                                UINT8 fns = fn[65];

                                                if (ntfs_name_icmp_utf16(
                                                    (const UINT16 *)(fn + 66),
                                                    fnl, name) == 0) {
                                                    INT64 new_ref =
                                                        (INT64)(mr &
                                                        0x0000FFFFFFFFFFFFULL);
                                                    if (fns != NTFS_NS_DOS) {
                                                        result = new_ref;
                                                        mem_free(ibuf);
                                                        mem_free(extents);
                                                        goto done;
                                                    }
                                                    /* DOS match — save but
                                                       keep looking */
                                                    result = new_ref;
                                                }
                                            }

                                            if (el == 0)
                                                break;
                                            pos += el;
                                        }
                                    }

                                    total_read += ibs;
                                }
                            }

                            mem_free(ibuf);
                        }
                    }

                    mem_free(extents);
                }
            }
        }
    }

done:
    mem_free(mft_buf);
    return result;
}

/*
 * Resolve a full path to an MFT record number.
 * Path format: "/" for root, "/dir/subdir/file.txt".
 * Returns MFT record number or -1 on error.
 */
static INT64 ntfs_resolve_path(struct ntfs_vol *vol, const char *path)
{
    /* Empty path or "/" => root directory (MFT record 5) */
    if (!path || path[0] == '\0')
        return -1;

    if (path[0] == '/' && path[1] == '\0')
        return NTFS_MFT_RECORD_ROOT;

    /* Skip leading '/' */
    const char *p = path;
    if (*p == '/') p++;

    UINT64 current_mft = NTFS_MFT_RECORD_ROOT;

    while (*p) {
        /* Extract next component */
        char component[FS_MAX_NAME];
        int ci = 0;

        while (*p && *p != '/' && ci < FS_MAX_NAME - 1)
            component[ci++] = *p++;
        component[ci] = '\0';

        /* Skip trailing '/' */
        if (*p == '/') p++;

        if (ci == 0)
            continue;

        /* Look up this component in the current directory */
        INT64 next = ntfs_lookup_name(vol, current_mft, component);
        if (next < 0)
            return -1;

        current_mft = (UINT64)next;
    }

    return (INT64)current_mft;
}

/* ------------------------------------------------------------------ */
/* Get file/dir info from MFT record                                   */
/* ------------------------------------------------------------------ */

/*
 * Check if an MFT record is a directory.
 * Returns 1 if directory, 0 if file, -1 on error.
 */
static int ntfs_is_directory(struct ntfs_vol *vol, UINT64 mft_num)
{
    UINT8 *mft_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
    if (!mft_buf)
        return -1;

    if (ntfs_read_mft_record(vol, mft_num, mft_buf) != 0) {
        mem_free(mft_buf);
        return -1;
    }

    UINT16 flags = rd16(mft_buf + 22);
    mem_free(mft_buf);

    return (flags & NTFS_MFT_DIRECTORY) ? 1 : 0;
}

/*
 * Get file size from MFT record's $DATA attribute.
 * Returns file size, or 0 if not found/error.
 */
static UINT64 ntfs_get_file_size(struct ntfs_vol *vol, UINT64 mft_num)
{
    UINT8 *mft_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
    if (!mft_buf)
        return 0;

    if (ntfs_read_mft_record(vol, mft_num, mft_buf) != 0) {
        mem_free(mft_buf);
        return 0;
    }

    /* Check if directory */
    UINT16 flags = rd16(mft_buf + 22);
    if (flags & NTFS_MFT_DIRECTORY) {
        mem_free(mft_buf);
        return 0;
    }

    /* Find $DATA attribute (unnamed) */
    UINT8 *data_attr = ntfs_find_attr(mft_buf, vol->mft_record_size,
                                      NTFS_AT_DATA, 0, 0);
    if (!data_attr) {
        mem_free(mft_buf);
        return 0;
    }

    UINT64 size;
    if (data_attr[8]) {
        /* Non-resident: real_size at offset 48 */
        size = rd64(data_attr + 48);
    } else {
        /* Resident: value_length at offset 16 */
        size = rd32(data_attr + 16);
    }

    mem_free(mft_buf);
    return size;
}

/* ------------------------------------------------------------------ */
/* Read volume label from $Volume (MFT record 3)                       */
/* ------------------------------------------------------------------ */

static void ntfs_read_volume_label(struct ntfs_vol *vol)
{
    vol->label[0] = '\0';

    UINT8 *mft_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
    if (!mft_buf)
        return;

    if (ntfs_read_mft_record(vol, NTFS_MFT_RECORD_VOLUME, mft_buf) != 0) {
        mem_free(mft_buf);
        return;
    }

    /* Find $VOLUME_NAME attribute (type 0x60) */
    UINT8 *vn_attr = ntfs_find_attr_any(mft_buf, vol->mft_record_size,
                                        NTFS_AT_VOLUME_NAME);
    if (!vn_attr) {
        mem_free(mft_buf);
        return;
    }

    /* Must be resident */
    if (vn_attr[8] != 0) {
        mem_free(mft_buf);
        return;
    }

    UINT32 val_len = rd32(vn_attr + 16);
    UINT16 val_off = rd16(vn_attr + 20);

    if (val_len > 0) {
        UINT8 *val = vn_attr + val_off;
        int nchars = val_len / 2; /* UTF-16 chars */
        ntfs_utf16_to_ascii((const UINT16 *)val, nchars,
                            vol->label, sizeof(vol->label));
    }

    mem_free(mft_buf);
}

/* ------------------------------------------------------------------ */
/* Handle $ATTRIBUTE_LIST for MFT records with external attributes     */
/* ------------------------------------------------------------------ */

/*
 * Some MFT records are too large to fit all attributes in a single
 * 1024-byte record.  When this happens, the base record contains an
 * $ATTRIBUTE_LIST (type 0x20) that references other MFT records holding
 * the additional attributes.
 *
 * This function reads the $DATA attribute for a file that may have its
 * $DATA attribute in an extension record.  Returns 0 on success.
 */
static int ntfs_read_file_data_via_attrlist(struct ntfs_vol *vol,
                                            UINT8 *base_mft_buf,
                                            UINT8 **out_data,
                                            UINT64 *out_size)
{
    /* Find $ATTRIBUTE_LIST */
    UINT8 *al_attr = ntfs_find_attr_any(base_mft_buf, vol->mft_record_size,
                                        NTFS_AT_ATTRIBUTE_LIST);
    if (!al_attr)
        return -1;

    /* Read attribute list data */
    UINT8 *al_data = 0;
    UINT64 al_size = 0;
    if (ntfs_read_attr_data(vol, al_attr, &al_data, &al_size) != 0)
        return -1;

    /*
     * Walk the attribute list looking for $DATA (type 0x80) entries.
     * Each entry in the attribute list:
     *   0  UINT32  type
     *   4  UINT16  record_length
     *   6  UINT8   name_length
     *   7  UINT8   name_offset
     *   8  UINT64  starting_vcn
     *  16  UINT64  mft_reference (low 6 = record, high 2 = seq)
     *  24  UINT16  attribute_id
     */
    struct ntfs_extent *all_extents = (struct ntfs_extent *)
        mem_alloc(NTFS_MAX_RUNS * sizeof(struct ntfs_extent));
    if (!all_extents) {
        mem_free(al_data);
        return -1;
    }

    int total_extents = 0;
    UINT64 total_data_size = 0;
    int found_data = 0;

    UINT64 pos = 0;
    while (pos + 26 <= al_size) {
        UINT32 al_type = rd32(al_data + pos);
        UINT16 al_rec_len = rd16(al_data + pos + 4);
        UINT8 al_nlen = al_data[pos + 6];

        if (al_rec_len < 26)
            break;
        if (pos + al_rec_len > al_size)
            break;

        if (al_type == NTFS_AT_DATA && al_nlen == 0) {
            /* Found a $DATA entry. Read the MFT record it points to. */
            UINT64 ext_mft_ref = rd64(al_data + pos + 16);
            UINT64 ext_mft_num = ext_mft_ref & 0x0000FFFFFFFFFFFFULL;

            UINT8 *ext_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
            if (!ext_buf) {
                mem_free(all_extents);
                mem_free(al_data);
                return -1;
            }

            if (ntfs_read_mft_record(vol, ext_mft_num, ext_buf) == 0) {
                UINT8 *da = ntfs_find_attr(ext_buf, vol->mft_record_size,
                                           NTFS_AT_DATA, 0, 0);
                if (da) {
                    found_data = 1;

                    if (!da[8]) {
                        /* Resident $DATA in extension record —
                           unlikely for split files but handle it */
                        UINT32 vl = rd32(da + 16);
                        UINT16 vo = rd16(da + 20);

                        UINT8 *data = (UINT8 *)mem_alloc(vl + 1);
                        if (data) {
                            mem_copy(data, da + vo, vl);
                            data[vl] = 0;
                            *out_data = data;
                            *out_size = vl;
                            mem_free(ext_buf);
                            mem_free(all_extents);
                            mem_free(al_data);
                            return 0;
                        }
                    } else {
                        /* Non-resident: extract data runs and
                           collect real_size */
                        UINT16 runs_off = rd16(da + 32);
                        UINT64 real_size = rd64(da + 48);
                        UINT32 da_len = rd32(da + 4);

                        if (real_size > total_data_size)
                            total_data_size = real_size;

                        if (runs_off < da_len) {
                            UINT32 rl = da_len - runs_off;
                            int space = NTFS_MAX_RUNS - total_extents;
                            if (space > 0) {
                                int n = ntfs_parse_data_runs(
                                    da + runs_off, rl,
                                    all_extents + total_extents,
                                    space);
                                if (n > 0) {
                                    /* Adjust VCNs: the starting_vcn
                                       in the attr list entry tells us
                                       the base VCN for these runs */
                                    UINT64 svcn = rd64(da + 16);
                                    for (int k = 0; k < n; k++) {
                                        all_extents[total_extents + k].vcn +=
                                            svcn;
                                    }
                                    total_extents += n;
                                }
                            }
                        }
                    }
                }
            }

            mem_free(ext_buf);
        }

        pos += al_rec_len;
    }

    mem_free(al_data);

    if (!found_data || total_extents == 0) {
        mem_free(all_extents);
        return -1;
    }

    /* Sort extents by VCN (they should already be in order from the
       attribute list, but be safe) */
    for (int i = 1; i < total_extents; i++) {
        struct ntfs_extent tmp;
        mem_copy(&tmp, &all_extents[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && all_extents[j].vcn > tmp.vcn) {
            mem_copy(&all_extents[j + 1], &all_extents[j], sizeof(tmp));
            j--;
        }
        mem_copy(&all_extents[j + 1], &tmp, sizeof(tmp));
    }

    /* Now read all the data */
    UINT8 *data = (UINT8 *)mem_alloc((UINTN)total_data_size + 1);
    if (!data) {
        mem_free(all_extents);
        return -1;
    }

    if (ntfs_read_data_from_extents(vol, all_extents, total_extents,
                                    total_data_size, data) != 0) {
        mem_free(data);
        mem_free(all_extents);
        return -1;
    }

    data[total_data_size] = 0;
    mem_free(all_extents);

    *out_data = data;
    *out_size = total_data_size;
    return 0;
}

/*
 * Similar to above but for INDEX_ALLOCATION via attribute list.
 * Collects extents from all extension records and reads INDX blocks.
 */
static int ntfs_read_dir_via_attrlist(struct ntfs_vol *vol,
                                      UINT8 *base_mft_buf,
                                      struct ntfs_dir_state *state)
{
    UINT8 *al_attr = ntfs_find_attr_any(base_mft_buf, vol->mft_record_size,
                                        NTFS_AT_ATTRIBUTE_LIST);
    if (!al_attr)
        return -1;

    UINT8 *al_data = 0;
    UINT64 al_size = 0;
    if (ntfs_read_attr_data(vol, al_attr, &al_data, &al_size) != 0)
        return -1;

    struct ntfs_extent *all_extents = (struct ntfs_extent *)
        mem_alloc(NTFS_MAX_RUNS * sizeof(struct ntfs_extent));
    if (!all_extents) {
        mem_free(al_data);
        return -1;
    }

    int total_extents = 0;
    UINT64 total_alloc_size = 0;

    UINT64 pos = 0;
    while (pos + 26 <= al_size) {
        UINT32 al_type = rd32(al_data + pos);
        UINT16 al_rec_len = rd16(al_data + pos + 4);

        if (al_rec_len < 26)
            break;
        if (pos + al_rec_len > al_size)
            break;

        if (al_type == NTFS_AT_INDEX_ALLOCATION) {
            UINT64 ext_mft_ref = rd64(al_data + pos + 16);
            UINT64 ext_mft_num = ext_mft_ref & 0x0000FFFFFFFFFFFFULL;

            UINT8 *ext_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
            if (!ext_buf) break;

            if (ntfs_read_mft_record(vol, ext_mft_num, ext_buf) == 0) {
                UINT8 *ia = ntfs_find_attr_any(ext_buf, vol->mft_record_size,
                                               NTFS_AT_INDEX_ALLOCATION);
                if (ia && ia[8]) {
                    UINT16 runs_off = rd16(ia + 32);
                    UINT64 real_size = rd64(ia + 48);
                    UINT32 ia_len = rd32(ia + 4);

                    if (real_size > total_alloc_size)
                        total_alloc_size = real_size;

                    if (runs_off < ia_len) {
                        UINT32 rl = ia_len - runs_off;
                        int space = NTFS_MAX_RUNS - total_extents;
                        if (space > 0) {
                            int n = ntfs_parse_data_runs(
                                ia + runs_off, rl,
                                all_extents + total_extents, space);
                            if (n > 0) {
                                UINT64 svcn = rd64(ia + 16);
                                for (int k = 0; k < n; k++)
                                    all_extents[total_extents + k].vcn += svcn;
                                total_extents += n;
                            }
                        }
                    }
                }
            }

            mem_free(ext_buf);
        }

        pos += al_rec_len;
    }

    mem_free(al_data);

    if (total_extents > 0 && total_alloc_size > 0) {
        UINT32 ibs = vol->index_block_size;
        UINT32 bpc = vol->bytes_per_cluster;
        UINT8 *ibuf = (UINT8 *)mem_alloc(ibs);

        if (ibuf) {
            /* Sort extents by VCN */
            for (int i = 1; i < total_extents; i++) {
                struct ntfs_extent tmp;
                mem_copy(&tmp, &all_extents[i], sizeof(tmp));
                int j = i - 1;
                while (j >= 0 && all_extents[j].vcn > tmp.vcn) {
                    mem_copy(&all_extents[j + 1], &all_extents[j], sizeof(tmp));
                    j--;
                }
                mem_copy(&all_extents[j + 1], &tmp, sizeof(tmp));
            }

            UINT64 total_read = 0;
            for (int e = 0; e < total_extents && total_read < total_alloc_size; e++) {
                UINT64 lcn = all_extents[e].lcn;
                UINT64 run_len = all_extents[e].length;
                if (lcn == 0 && run_len > 0) {
                    total_read += run_len * bpc;
                    continue;
                }
                UINT64 run_bytes = run_len * bpc;
                UINT64 blocks_in_run = run_bytes / ibs;

                for (UINT64 b = 0; b < blocks_in_run && total_read < total_alloc_size; b++) {
                    UINT64 bbo = b * ibs;
                    UINT64 bc = lcn + (bbo / bpc);
                    UINT32 oic = (UINT32)(bbo % bpc);
                    UINT64 db = bc * bpc + oic;

                    if (ntfs_read_bytes(vol, db, ibs, ibuf) != 0)
                        break;

                    ntfs_parse_indx_block(vol, state, ibuf, ibs);
                    total_read += ibs;
                }
            }

            mem_free(ibuf);
        }
    }

    mem_free(all_extents);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Extended directory reading with ATTRIBUTE_LIST support               */
/* ------------------------------------------------------------------ */

/*
 * Read directory entries, handling the case where the directory's
 * INDEX_ALLOCATION attribute is split across extension MFT records
 * via $ATTRIBUTE_LIST.
 */
static int ntfs_read_dir_entries_full(struct ntfs_vol *vol, UINT64 mft_num,
                                      struct fs_entry *entries,
                                      int max_entries)
{
    /* Try simple read first */
    int result = ntfs_read_dir_entries(vol, mft_num, entries, max_entries);
    if (result >= 0)
        return result;

    /* If that failed, check if the MFT record has an ATTRIBUTE_LIST */
    UINT8 *mft_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
    if (!mft_buf)
        return -1;

    if (ntfs_read_mft_record(vol, mft_num, mft_buf) != 0) {
        mem_free(mft_buf);
        return -1;
    }

    UINT8 *al_attr = ntfs_find_attr_any(mft_buf, vol->mft_record_size,
                                        NTFS_AT_ATTRIBUTE_LIST);
    if (!al_attr) {
        mem_free(mft_buf);
        return -1;
    }

    /* Set up dir state */
    int alloc_max = (max_entries < NTFS_MAX_DIR_ENTRIES) ?
                    NTFS_MAX_DIR_ENTRIES : max_entries;

    struct ntfs_dir_state state;
    state.entries = entries;
    state.count = 0;
    state.max_entries = max_entries;
    state.mft_refs = (UINT64 *)mem_alloc(alloc_max * sizeof(UINT64));
    state.namespaces = (UINT8 *)mem_alloc(alloc_max * sizeof(UINT8));

    if (!state.mft_refs || !state.namespaces) {
        if (state.mft_refs) mem_free(state.mft_refs);
        if (state.namespaces) mem_free(state.namespaces);
        mem_free(mft_buf);
        return -1;
    }
    mem_set(state.mft_refs, 0, alloc_max * sizeof(UINT64));
    mem_set(state.namespaces, 0, alloc_max * sizeof(UINT8));

    /* Parse INDEX_ROOT from base record */
    UINT8 *ir_attr = ntfs_find_attr_any(mft_buf, vol->mft_record_size,
                                        NTFS_AT_INDEX_ROOT);
    if (ir_attr) {
        UINT32 val_len = rd32(ir_attr + 16);
        UINT16 val_off = rd16(ir_attr + 20);
        UINT8 *ir_val = ir_attr + val_off;

        if (val_len >= 32) {
            UINT32 node_entries_off = rd32(ir_val + 16);
            UINT32 node_total_size = rd32(ir_val + 20);

            UINT32 entries_start = 16 + node_entries_off;
            if (entries_start < val_len) {
                UINT32 entries_size = node_total_size;
                if (entries_start + entries_size > val_len)
                    entries_size = val_len - entries_start;
                ntfs_parse_index_entries(&state,
                                        ir_val + entries_start,
                                        entries_size);
            }
        }
    }

    /* Read INDEX_ALLOCATION from extension records via attribute list */
    ntfs_read_dir_via_attrlist(vol, mft_buf, &state);

    result = state.count;

    mem_free(state.mft_refs);
    mem_free(state.namespaces);
    mem_free(mft_buf);

    return result;
}

/* ------------------------------------------------------------------ */
/* Public API: ntfs_mount                                              */
/* ------------------------------------------------------------------ */

struct ntfs_vol *ntfs_mount(ntfs_block_read_fn read_fn,
                            void *ctx, UINT32 block_size)
{
    /* Allocate volume structure */
    struct ntfs_vol *vol = (struct ntfs_vol *)
        mem_alloc(sizeof(struct ntfs_vol));
    if (!vol)
        return 0;
    mem_set(vol, 0, sizeof(struct ntfs_vol));

    vol->read_fn = read_fn;
    vol->ctx = ctx;
    vol->dev_block_size = block_size;

    /* Initialize cache */
    ntfs_cache_init(vol);

    /* Read boot sector (sector 0) */
    UINT32 boot_buf_size = (block_size < 512) ? 512 : block_size;
    /* We need at least 512 bytes for the BPB. If device blocks are
       smaller, read multiple. If larger, read one. */
    UINT8 *boot = (UINT8 *)mem_alloc(boot_buf_size);
    if (!boot) {
        mem_free(vol);
        return 0;
    }

    /* Read LBA 0 */
    UINT32 blocks_for_bpb = 1;
    if (block_size < 512)
        blocks_for_bpb = 512 / block_size;

    if (read_fn(ctx, 0, blocks_for_bpb, boot) != 0) {
        mem_free(boot);
        mem_free(vol);
        return 0;
    }

    /* Verify NTFS OEM ID */
    if (boot[3] != 'N' || boot[4] != 'T' || boot[5] != 'F' ||
        boot[6] != 'S') {
        mem_free(boot);
        mem_free(vol);
        return 0;
    }

    /* Parse BPB */
    vol->bytes_per_sector = rd16(boot + 11);
    vol->sectors_per_cluster = boot[13];
    vol->total_sectors = rd64(boot + 40);

    if (vol->bytes_per_sector == 0 || vol->sectors_per_cluster == 0) {
        mem_free(boot);
        mem_free(vol);
        return 0;
    }

    vol->bytes_per_cluster = vol->bytes_per_sector * vol->sectors_per_cluster;
    vol->total_clusters = vol->total_sectors / vol->sectors_per_cluster;

    /* MFT record size */
    INT8 mft_rec_val = rd_s8(boot + 64);
    if (mft_rec_val > 0) {
        vol->mft_record_size = (UINT32)mft_rec_val * vol->bytes_per_cluster;
    } else {
        /* Negative means 2^abs(val) bytes */
        vol->mft_record_size = 1U << (-(int)mft_rec_val);
    }

    /* Index block size */
    INT8 idx_blk_val = rd_s8(boot + 68);
    if (idx_blk_val > 0) {
        vol->index_block_size = (UINT32)idx_blk_val * vol->bytes_per_cluster;
    } else {
        vol->index_block_size = 1U << (-(int)idx_blk_val);
    }

    /* MFT starting cluster */
    vol->mft_cluster = rd64(boot + 48);

    mem_free(boot);

    /* Sanity checks */
    if (vol->mft_record_size == 0 || vol->mft_record_size > NTFS_MAX_MFT_SIZE) {
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }
    if (vol->index_block_size == 0 ||
        vol->index_block_size > NTFS_MAX_INDX_SIZE) {
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }
    if (vol->bytes_per_cluster == 0) {
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    /* --- Read $MFT record 0 to get the $MFT data runs --- */
    /* We need these to locate any MFT record by number */
    UINT8 *mft0 = (UINT8 *)mem_alloc(vol->mft_record_size);
    if (!mft0) {
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    /* Read $MFT record 0 directly from mft_cluster */
    UINT64 mft_byte = vol->mft_cluster * vol->bytes_per_cluster;
    if (ntfs_read_bytes(vol, mft_byte, vol->mft_record_size, mft0) != 0) {
        mem_free(mft0);
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    /* Verify signature */
    if (mft0[0] != 'F' || mft0[1] != 'I' || mft0[2] != 'L' ||
        mft0[3] != 'E') {
        mem_free(mft0);
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    /* Apply fixup to MFT record 0 */
    if (ntfs_apply_fixup(mft0, vol->mft_record_size,
                         vol->bytes_per_sector) != 0) {
        mem_free(mft0);
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    /* Find the $DATA attribute (type 0x80, unnamed) in $MFT */
    UINT8 *mft_data_attr = ntfs_find_attr(mft0, vol->mft_record_size,
                                          NTFS_AT_DATA, 0, 0);
    if (!mft_data_attr) {
        mem_free(mft0);
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    /* The $MFT $DATA attribute should be non-resident */
    if (!mft_data_attr[8]) {
        /* Resident $MFT data — extremely unlikely, but handle it */
        mem_free(mft0);
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    /* Save the raw data runs */
    UINT16 runs_off = rd16(mft_data_attr + 32);
    UINT32 attr_len = rd32(mft_data_attr + 4);

    if (runs_off >= attr_len) {
        mem_free(mft0);
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    vol->mft_runs_len = attr_len - runs_off;
    vol->mft_runs = (UINT8 *)mem_alloc(vol->mft_runs_len);
    if (!vol->mft_runs) {
        mem_free(mft0);
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }
    mem_copy(vol->mft_runs, mft_data_attr + runs_off, vol->mft_runs_len);

    /* Pre-decode MFT extents */
    vol->mft_extents = (struct ntfs_extent *)
        mem_alloc(NTFS_MAX_RUNS * sizeof(struct ntfs_extent));
    if (!vol->mft_extents) {
        mem_free(vol->mft_runs);
        mem_free(mft0);
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    vol->mft_extent_count = ntfs_parse_data_runs(vol->mft_runs,
                                                  vol->mft_runs_len,
                                                  vol->mft_extents,
                                                  NTFS_MAX_RUNS);
    if (vol->mft_extent_count <= 0) {
        mem_free(vol->mft_extents);
        mem_free(vol->mft_runs);
        mem_free(mft0);
        ntfs_cache_free(vol);
        mem_free(vol);
        return 0;
    }

    /* Check if $MFT has an attribute list (very large MFT) */
    UINT8 *al_attr = ntfs_find_attr_any(mft0, vol->mft_record_size,
                                        NTFS_AT_ATTRIBUTE_LIST);
    if (al_attr) {
        /* Read the attribute list to find extension $DATA runs */
        UINT8 *al_data = 0;
        UINT64 al_size = 0;
        if (ntfs_read_attr_data(vol, al_attr, &al_data, &al_size) == 0) {
            /* Collect additional $DATA extents from extension records */
            struct ntfs_extent *extra = (struct ntfs_extent *)
                mem_alloc(NTFS_MAX_RUNS * sizeof(struct ntfs_extent));
            int extra_count = 0;

            if (extra) {
                /* Copy existing extents */
                mem_copy(extra, vol->mft_extents,
                         vol->mft_extent_count * sizeof(struct ntfs_extent));
                extra_count = vol->mft_extent_count;

                UINT64 apos = 0;
                while (apos + 26 <= al_size) {
                    UINT32 at = rd32(al_data + apos);
                    UINT16 arl = rd16(al_data + apos + 4);
                    UINT8 anl = al_data[apos + 6];
                    UINT64 svcn = rd64(al_data + apos + 8);

                    if (arl < 26) break;
                    if (apos + arl > al_size) break;

                    if (at == NTFS_AT_DATA && anl == 0 && svcn > 0) {
                        UINT64 emr = rd64(al_data + apos + 16);
                        UINT64 emn = emr & 0x0000FFFFFFFFFFFFULL;

                        /* Read extension MFT record - use current extents */
                        UINT8 *ebuf = (UINT8 *)mem_alloc(vol->mft_record_size);
                        if (ebuf) {
                            if (ntfs_read_mft_record(vol, emn, ebuf) == 0) {
                                UINT8 *eda = ntfs_find_attr(
                                    ebuf, vol->mft_record_size,
                                    NTFS_AT_DATA, 0, 0);
                                if (eda && eda[8]) {
                                    UINT16 ero = rd16(eda + 32);
                                    UINT32 eal = rd32(eda + 4);
                                    if (ero < eal) {
                                        int sp = NTFS_MAX_RUNS - extra_count;
                                        if (sp > 0) {
                                            int n = ntfs_parse_data_runs(
                                                eda + ero, eal - ero,
                                                extra + extra_count, sp);
                                            if (n > 0) {
                                                for (int k = 0; k < n; k++)
                                                    extra[extra_count + k].vcn += svcn;
                                                extra_count += n;
                                            }
                                        }
                                    }
                                }
                            }
                            mem_free(ebuf);
                        }
                    }

                    apos += arl;
                }

                if (extra_count > vol->mft_extent_count) {
                    /* Sort by VCN */
                    for (int i = 1; i < extra_count; i++) {
                        struct ntfs_extent tmp;
                        mem_copy(&tmp, &extra[i], sizeof(tmp));
                        int j = i - 1;
                        while (j >= 0 && extra[j].vcn > tmp.vcn) {
                            mem_copy(&extra[j + 1], &extra[j], sizeof(tmp));
                            j--;
                        }
                        mem_copy(&extra[j + 1], &tmp, sizeof(tmp));
                    }

                    /* Replace the old extent array */
                    mem_free(vol->mft_extents);
                    vol->mft_extents = extra;
                    vol->mft_extent_count = extra_count;
                    extra = 0; /* don't free below */
                }

                if (extra)
                    mem_free(extra);
            }

            mem_free(al_data);
        }
    }

    mem_free(mft0);

    /* Read volume label */
    ntfs_read_volume_label(vol);

    return vol;
}

/* ------------------------------------------------------------------ */
/* Public API: ntfs_unmount                                            */
/* ------------------------------------------------------------------ */

void ntfs_unmount(struct ntfs_vol *vol)
{
    if (!vol)
        return;

    ntfs_cache_free(vol);

    if (vol->mft_runs)
        mem_free(vol->mft_runs);
    if (vol->mft_extents)
        mem_free(vol->mft_extents);

    mem_free(vol);
}

/* ------------------------------------------------------------------ */
/* Public API: ntfs_readdir                                            */
/* ------------------------------------------------------------------ */

int ntfs_readdir(struct ntfs_vol *vol, const char *path,
                 struct fs_entry *entries, int max_entries)
{
    if (!vol || !path || !entries || max_entries <= 0)
        return -1;

    INT64 mft_num = ntfs_resolve_path(vol, path);
    if (mft_num < 0)
        return -1;

    int count = ntfs_read_dir_entries_full(vol, (UINT64)mft_num,
                                           entries, max_entries);
    if (count < 0)
        return -1;

    /* Sort results */
    ntfs_sort_entries(entries, count);

    return count;
}

/* ------------------------------------------------------------------ */
/* Public API: ntfs_readfile                                           */
/* ------------------------------------------------------------------ */

void *ntfs_readfile(struct ntfs_vol *vol, const char *path, UINTN *out_size)
{
    if (!vol || !path || !out_size)
        return 0;

    *out_size = 0;

    INT64 mft_num = ntfs_resolve_path(vol, path);
    if (mft_num < 0)
        return 0;

    UINT8 *mft_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
    if (!mft_buf)
        return 0;

    if (ntfs_read_mft_record(vol, (UINT64)mft_num, mft_buf) != 0) {
        mem_free(mft_buf);
        return 0;
    }

    /* Verify it's a file, not a directory */
    UINT16 flags = rd16(mft_buf + 22);
    if (flags & NTFS_MFT_DIRECTORY) {
        mem_free(mft_buf);
        return 0;
    }

    /* Find $DATA attribute (unnamed) */
    UINT8 *data_attr = ntfs_find_attr(mft_buf, vol->mft_record_size,
                                      NTFS_AT_DATA, 0, 0);

    if (data_attr) {
        /* Read attribute data */
        UINT8 *data = 0;
        UINT64 size = 0;

        if (ntfs_read_attr_data(vol, data_attr, &data, &size) == 0) {
            *out_size = (UINTN)size;
            mem_free(mft_buf);
            return data;
        }
    }

    /* $DATA not found in base record — check for $ATTRIBUTE_LIST */
    UINT8 *al = ntfs_find_attr_any(mft_buf, vol->mft_record_size,
                                   NTFS_AT_ATTRIBUTE_LIST);
    if (al) {
        UINT8 *data = 0;
        UINT64 size = 0;

        if (ntfs_read_file_data_via_attrlist(vol, mft_buf,
                                             &data, &size) == 0) {
            *out_size = (UINTN)size;
            mem_free(mft_buf);
            return data;
        }
    }

    mem_free(mft_buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API: ntfs_volume_info                                        */
/* ------------------------------------------------------------------ */

int ntfs_volume_info(struct ntfs_vol *vol, UINT64 *total_bytes,
                     UINT64 *free_bytes)
{
    if (!vol)
        return -1;

    if (total_bytes)
        *total_bytes = vol->total_sectors * vol->bytes_per_sector;

    if (free_bytes) {
        /* To get free space accurately, we'd need to read and parse the
           $Bitmap (MFT record 6), which tracks every cluster's allocation
           status.  For a read-only driver this is optional but useful.

           We attempt to read $Bitmap and count free clusters. */
        *free_bytes = 0;

        UINT8 *mft_buf = (UINT8 *)mem_alloc(vol->mft_record_size);
        if (mft_buf) {
            if (ntfs_read_mft_record(vol, 6, mft_buf) == 0) {
                UINT8 *bm_attr = ntfs_find_attr(mft_buf, vol->mft_record_size,
                                                NTFS_AT_DATA, 0, 0);
                if (bm_attr) {
                    UINT8 *bm_data = 0;
                    UINT64 bm_size = 0;

                    /* For very large volumes, the bitmap could be huge
                       (e.g., 1TB / 4096 clusters / 8 bits = 32MB).
                       We'll read it in chunks to avoid excessive memory. */
                    if (!bm_attr[8]) {
                        /* Resident bitmap (very small volume) */
                        if (ntfs_read_attr_data(vol, bm_attr,
                                                &bm_data, &bm_size) == 0) {
                            UINT64 free_clusters = 0;
                            UINT64 max_bit = vol->total_clusters;
                            UINT64 bit = 0;

                            for (UINT64 i = 0; i < bm_size && bit < max_bit; i++) {
                                UINT8 byte = bm_data[i];
                                for (int b = 0; b < 8 && bit < max_bit; b++, bit++) {
                                    if (!(byte & (1 << b)))
                                        free_clusters++;
                                }
                            }

                            *free_bytes = free_clusters * vol->bytes_per_cluster;
                            mem_free(bm_data);
                        }
                    } else {
                        /* Non-resident bitmap — read in 64KB chunks */
                        UINT16 runs_off = rd16(bm_attr + 32);
                        UINT64 real_size = rd64(bm_attr + 48);
                        UINT32 ba_len = rd32(bm_attr + 4);

                        if (runs_off < ba_len) {
                            struct ntfs_extent *ext = (struct ntfs_extent *)
                                mem_alloc(NTFS_MAX_RUNS * sizeof(struct ntfs_extent));
                            if (ext) {
                                int ec = ntfs_parse_data_runs(
                                    bm_attr + runs_off, ba_len - runs_off,
                                    ext, NTFS_MAX_RUNS);

                                if (ec > 0) {
                                    UINT64 free_clusters = 0;
                                    UINT64 max_bit = vol->total_clusters;
                                    UINT64 bit = 0;

                                    /* Read chunks from extents */
                                    UINT32 chunk_size = 65536;
                                    UINT8 *chunk = (UINT8 *)mem_alloc(chunk_size);

                                    if (chunk) {
                                        UINT64 offset = 0;
                                        UINT32 bpc = vol->bytes_per_cluster;

                                        for (int e = 0; e < ec && bit < max_bit; e++) {
                                            UINT64 lcn = ext[e].lcn;
                                            UINT64 run_bytes = ext[e].length * bpc;

                                            if (lcn == 0) {
                                                /* Sparse - treat as all free */
                                                UINT64 bits_in_run = run_bytes * 8;
                                                if (bit + bits_in_run > max_bit)
                                                    bits_in_run = max_bit - bit;
                                                free_clusters += bits_in_run;
                                                bit += bits_in_run;
                                                offset += run_bytes;
                                                continue;
                                            }

                                            UINT64 run_remaining = run_bytes;
                                            UINT64 disk_byte = lcn * bpc;

                                            while (run_remaining > 0 && bit < max_bit) {
                                                UINT32 to_read = (run_remaining < chunk_size) ?
                                                    (UINT32)run_remaining : chunk_size;

                                                if (ntfs_read_bytes(vol, disk_byte,
                                                                    to_read, chunk) != 0)
                                                    break;

                                                for (UINT32 i = 0; i < to_read && bit < max_bit; i++) {
                                                    UINT8 byte = chunk[i];
                                                    for (int b = 0; b < 8 && bit < max_bit; b++, bit++) {
                                                        if (!(byte & (1 << b)))
                                                            free_clusters++;
                                                    }
                                                }

                                                disk_byte += to_read;
                                                run_remaining -= to_read;
                                                offset += to_read;
                                            }
                                        }

                                        mem_free(chunk);
                                    }

                                    *free_bytes = free_clusters * vol->bytes_per_cluster;
                                }

                                mem_free(ext);
                            }
                        }
                    }
                }
            }
            mem_free(mft_buf);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API: ntfs_file_size                                          */
/* ------------------------------------------------------------------ */

UINT64 ntfs_file_size(struct ntfs_vol *vol, const char *path)
{
    if (!vol || !path)
        return 0;

    INT64 mft_num = ntfs_resolve_path(vol, path);
    if (mft_num < 0)
        return 0;

    return ntfs_get_file_size(vol, (UINT64)mft_num);
}

/* ------------------------------------------------------------------ */
/* Public API: ntfs_exists                                             */
/* ------------------------------------------------------------------ */

int ntfs_exists(struct ntfs_vol *vol, const char *path)
{
    if (!vol || !path)
        return 0;

    INT64 mft_num = ntfs_resolve_path(vol, path);
    return (mft_num >= 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Public API: ntfs_get_label                                          */
/* ------------------------------------------------------------------ */

const char *ntfs_get_label(struct ntfs_vol *vol)
{
    if (!vol)
        return "";
    return vol->label;
}
