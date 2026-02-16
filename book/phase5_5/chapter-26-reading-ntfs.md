---
layout: default
title: "Chapter 26: Reading NTFS"
parent: "Phase 5.5: exFAT & NTFS"
nav_order: 2
---

# Chapter 26: Reading NTFS

## The Drive You Cannot Read

Someone hands you a USB stick. They want to copy a file to your workstation -- a document, a photo, a piece of source code they have been working on. You plug it in. The file browser shows an entry: magenta, labeled `[NTFS]`. You press ENTER, navigate to the file, open it in the editor. It is read-only -- the title bar says `[READ-ONLY]` and F2:Save is gone from the status bar -- but you can see the contents. You can select text, copy it, paste it into a file on your own FAT32 volume. The data is off the drive.

Before this chapter, that scenario ended at "you plug it in." The workstation could detect an NTFS volume -- the boot sector identification from Chapter 25's volume abstraction layer already recognized the `NTFS    ` OEM signature. But it could not read a single byte of data from it. The volume appeared in the browser with a label and a size, and that was all. To get data off the drive, you would have to format it -- destroying whatever the person brought you.

Most USB drives that have touched a Windows machine are NTFS. Windows has formatted new USB drives as NTFS by default for years. A survival workstation that cannot read NTFS is a workstation that cannot accept data from the outside world. Fixing this is not a feature -- it is a necessity.

NTFS is enormous. The specification -- such as it exists, since Microsoft has never published a complete one -- describes a journaling filesystem with transactions, compression (LZNT1), encryption (EFS), sparse files, alternate data streams, reparse points, hard links, security descriptors with access control lists, and a change journal. The on-disk format is not a filesystem in the way FAT32 is a filesystem. It is a database. Every file and directory is a row in a table called the Master File Table. Every piece of metadata -- the filename, the timestamps, the security descriptor, the data itself -- is an attribute stored within that row. Directories are B+ tree indexes. Small files are stored inline within their MFT record. Large files are described by compressed run-length encodings that map virtual clusters to physical clusters on disk.

Writing to NTFS means updating the MFT, updating the journal, updating the B+ tree indexes, updating the bitmap, and doing all of this transactionally so that a power failure does not corrupt the volume. It is thousands of lines of code for a correct implementation. ntfs-3g, the open-source Linux driver, is roughly 40,000 lines. It is GPL-licensed, designed around FUSE, and assumes a full POSIX environment.

None of that works here. We have no libc, no POSIX, no FUSE, no GPL compatibility. And we do not need to write. The survival scenario is clear: someone hands you a drive, you need to get data off it. Read-only support is the right answer. It lets us skip the journal, skip transactions, skip the bitmap, skip compression, and focus entirely on the read path: given a path, find the MFT record, read the data.

The result is about 800 lines of actual logic (the rest is boilerplate, error handling, and helpers). It handles everything a typical NTFS volume contains -- files, directories, nested subdirectories, small resident files, large non-resident files, directories with thousands of entries -- and explicitly does not handle compression, encryption, sparse files, or alternate data streams. Those are documented limitations, and they cover edge cases that rarely matter for the "copy a file off a USB stick" scenario.

The driver follows the same callback-based pattern as the exFAT driver from Chapter 25. No UEFI dependency in the core logic. You provide a function pointer that reads blocks, and the driver does everything else. This makes it testable on a Linux host with a disk image, and it makes extraction into a standalone library trivial -- which we will do at the end of the chapter.

## The NTFS Header

The public interface is minimal. Eight functions, an opaque volume handle, and a single callback type:

```c
/*
 * ntfs.h -- NTFS filesystem driver (read-only)
 *
 * Portable: uses callback-based block I/O, no UEFI dependency.
 * Supports: file/dir read, path resolution, MFT parsing.
 * Does NOT support: compression, encryption, sparse files, ADS.
 */
#ifndef NTFS_H
#define NTFS_H

#include "boot.h"
#include "fs.h"

/* Block I/O callbacks */
typedef int (*ntfs_block_read_fn)(void *ctx, UINT64 lba, UINT32 count, void *buf);

/* Opaque volume handle */
struct ntfs_vol;

/* Mount an NTFS volume (read-only). Returns NULL on error.
   block_size is the underlying device block size (typically 512). */
struct ntfs_vol *ntfs_mount(ntfs_block_read_fn read_fn,
                             void *ctx, UINT32 block_size);

/* Unmount and free all resources */
void ntfs_unmount(struct ntfs_vol *vol);

/* Read directory contents. path is ASCII with '/' separators.
   "/" for root. Returns entry count, or -1 on error. */
int ntfs_readdir(struct ntfs_vol *vol, const char *path,
                 struct fs_entry *entries, int max_entries);

/* Read entire file into newly allocated buffer. Returns NULL on error. */
void *ntfs_readfile(struct ntfs_vol *vol, const char *path, UINTN *out_size);

/* Get volume info. Returns 0 on success. */
int ntfs_volume_info(struct ntfs_vol *vol, UINT64 *total_bytes, UINT64 *free_bytes);

/* Get file size. Returns 0 if not found. */
UINT64 ntfs_file_size(struct ntfs_vol *vol, const char *path);

/* Check if path exists. Returns 1 if yes. */
int ntfs_exists(struct ntfs_vol *vol, const char *path);

/* Get volume label (ASCII). Returns empty string if none. */
const char *ntfs_get_label(struct ntfs_vol *vol);

#endif /* NTFS_H */
```

The callback type `ntfs_block_read_fn` takes an opaque context pointer, a logical block address, a block count, and a buffer. It returns zero on success. This is the same pattern the exFAT driver uses -- `fs.c` wraps UEFI's BlockIO protocol behind this callback, and the driver never touches UEFI directly.

The eight public functions mirror the exFAT driver's API exactly. `ntfs_mount` and `ntfs_unmount` manage the volume lifetime. `ntfs_readdir` and `ntfs_readfile` are the core data access functions. `ntfs_volume_info`, `ntfs_file_size`, `ntfs_exists`, and `ntfs_get_label` provide metadata queries. The `struct fs_entry` return type for `ntfs_readdir` is the same type used by the FAT32 and exFAT backends, so the file browser needs no changes to display NTFS directory contents.

All paths are ASCII with forward-slash separators. The volume abstraction layer in `fs.c` converts UEFI's CHAR16 backslash paths to this format before calling into the driver. Inside the driver, path resolution splits on `/` and walks the directory tree component by component.

## NTFS On Disk: Structures

The implementation begins with the on-disk structure definitions. Every structure uses `#pragma pack(1)` to match the exact byte layout on disk -- no padding, no alignment holes. This matters on ARM64 where the compiler would otherwise insert padding to align 64-bit fields to 8-byte boundaries.

```c
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
```

The boot sector is a BIOS Parameter Block inherited from DOS, but most of the FAT-related fields are zeroed out. `reserved_sectors`, `number_of_fats`, `root_entries`, `fat_size` -- all zero. NTFS is not FAT. These fields exist only to prevent FAT drivers from misidentifying the volume.

The `oem_id` at offset 3 contains `NTFS    ` (four letters, four spaces). This is how volume detection works: the volume abstraction layer reads the first sector and checks bytes 3 through 10. The `bytes_per_sector` field at offset 11 is usually 512. `sectors_per_cluster` at offset 13 is typically 8, giving 4 KB clusters -- the standard for volumes up to about 16 TB.

The critical fields are at higher offsets. `total_sectors` at offset 40 gives the volume size. `mft_cluster` at offset 48 is the single most important value -- it tells us where the Master File Table begins. Without this, we cannot find any file or directory on the volume. `mft_mirr_cluster` at offset 56 points to a backup copy of the first few MFT records, used for disaster recovery.

The `clusters_per_mft_record` field at offset 64 uses a peculiar signed encoding. If positive, the MFT record size is that many clusters. If negative, the record size is `2^abs(value)` bytes. Nearly every NTFS volume uses -10, meaning `2^10 = 1024` byte MFT records regardless of cluster size. The same encoding applies to `clusters_per_index_block` at offset 68. This encoding exists because MFT records need to be a fixed size independent of cluster size. A volume with 64 KB clusters cannot express a 1024-byte record size as a fraction of a cluster, so the negative-means-power-of-two trick sidesteps this elegantly.

```c
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
```

Every MFT record starts with the four-byte signature `FILE`. The `usa_offset` and `usa_count` fields locate the Update Sequence Array used for sector-level integrity checking -- we will examine this mechanism in detail in the fixup section. The `lsn` is the log sequence number from the journal, which we ignore entirely since we do not replay the journal.

The `sequence_number` is incremented each time this MFT record is reused for a different file. Combined with the record number, it forms a unique 8-byte reference that can detect stale pointers. `hard_link_count` tracks how many directory entries point to this file. `first_attr_offset` tells us where the attribute chain begins within the record -- typically 56 bytes from the start, but it varies.

The `flags` field is a bitmask: `0x01` means the record is in use (not deleted), `0x02` means it is a directory. Both flags can be set simultaneously. `used_size` is how many bytes of the record are actually used by attributes, and `allocated_size` is the total record size (typically 1024). The `base_record` field is nonzero for extension records -- records that hold overflow attributes for a file whose attributes do not fit in a single 1024-byte record.

```c
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
```

Every attribute in an MFT record begins with this 16-byte header. The `type` field identifies the attribute: `0x10` for standard information, `0x30` for filename, `0x80` for data, `0x90` for index root, and so on. The `length` field is the total size of the attribute including the header, the name, and the value or data runs. Walking the attribute chain means starting at `first_attr_offset`, reading the type and length, advancing by length bytes, and stopping when the type is `0xFFFFFFFF` (the end marker).

The `non_resident` flag is the branching point for all attribute parsing. If zero, the attribute's data is stored inline -- the value sits right after the header within the MFT record. If one, the data is stored in clusters on disk, described by data runs embedded in the attribute. The `name_length` and `name_offset` fields describe an optional UTF-16 attribute name. Most attributes are unnamed, but directory indexes use the name `$I30` -- a fact that caused one of two bugs discovered during testing.

```c
/* Resident attribute (follows common header) */
struct ntfs_attr_resident {
    UINT32  value_length;
    UINT16  value_offset;
    UINT16  flags;
};
```

For resident attributes, the value descriptor follows immediately after the 16-byte common header. `value_length` at offset +16 (relative to the attribute start) is a 4-byte unsigned integer giving the size of the inline data. `value_offset` at offset +20 is a 2-byte offset from the attribute start to the actual value bytes. The `flags` at offset +22 are reserved.

This layout was the source of the second bug found during testing. The initial implementation read `value_offset` at offset +18, assuming `value_length` was 2 bytes. But `value_length` is 4 bytes (a `UINT32`), so `value_offset` is at +20, not +18. Getting this wrong by two bytes means reading value data from the wrong position -- which corrupts filenames, file sizes, and attribute data in ways that are intermittent and baffling, because the error depends on what happens to sit at the wrong offset in each particular MFT record.

```c
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
```

For non-resident attributes, the descriptor is larger. `starting_vcn` and `last_vcn` define the range of Virtual Cluster Numbers this attribute instance covers -- relevant when an attribute is split across multiple MFT records via the attribute list mechanism. `data_runs_offset` at offset +32 (relative to attribute start) points to the compressed data run encoding that maps virtual clusters to physical clusters on disk. `real_size` at offset +48 is the actual data size in bytes. `allocated_size` is the total space allocated (may be larger due to cluster rounding). `initialized_size` is how much has been written -- beyond this point, the data reads as zeros.

```c
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
```

The `$FILE_NAME` attribute (type `0x30`) contains the filename and basic metadata. The `parent_ref` is an 8-byte MFT reference where the low 6 bytes are the parent directory's MFT record number and the high 2 bytes are the sequence number. Four timestamps follow -- creation, modification, MFT modification, and last read -- each as a Windows FILETIME (100-nanosecond intervals since January 1, 1601). We do not display these timestamps in the browser, but they are present in every filename attribute.

The `flags` field at offset 56 within the filename body contains the file attribute flags. The critical one for us is `0x10000000`, which indicates a directory. The `name_length` at offset 64 is the filename length in UTF-16 characters, and `name_namespace` at offset 65 identifies which naming convention this entry uses: POSIX (0, case-sensitive), Win32 (1, the long name), DOS (2, the 8.3 short name), or Win32+DOS (3, a name that satisfies both conventions). The actual UTF-16 filename follows immediately at offset 66.

```c
/* INDEX_ROOT header (value of 0x90 attribute) */
struct ntfs_index_root {
    UINT32  attr_type;             /* 0x30 for filename index */
    UINT32  collation_rule;
    UINT32  index_block_size;
    UINT8   clusters_per_index_block;
    UINT8   padding[3];
    /* Index node header follows at offset 16 */
};
```

The `$INDEX_ROOT` attribute (type `0x90`) is the root of a directory's B+ tree index. The `attr_type` field identifies what kind of entries the index contains -- for directories, this is `0x30` (filename attributes). The `index_block_size` gives the size of each INDX block in the B+ tree's non-root nodes. The index node header follows at offset 16 within the attribute value.

```c
/* Index node header (appears in INDEX_ROOT and INDX blocks) */
struct ntfs_index_node_header {
    UINT32  entries_offset;        /* relative to start of this header */
    UINT32  total_size;
    UINT32  allocated_size;
    UINT32  flags;                 /* 0x01 = has sub-nodes */
};
```

The index node header appears in both `$INDEX_ROOT` values and INDX block buffers. `entries_offset` is the offset from the start of this header to the first index entry. `total_size` is the total size of all entries. The `flags` field's bit 0 indicates whether this node has sub-nodes in `$INDEX_ALLOCATION` -- if set, the directory has more entries than fit in the root node alone.

```c
/* INDX block header */
struct ntfs_indx_header {
    UINT8   signature[4];          /* "INDX" */
    UINT16  usa_offset;
    UINT16  usa_count;
    UINT64  lsn;
    UINT64  vcn;
    /* Index node header at offset 24 */
};
```

INDX blocks are the non-root nodes of the directory B+ tree. They live in clusters on disk, described by the data runs of the `$INDEX_ALLOCATION` attribute. Each block has its own `INDX` signature and its own Update Sequence Array -- just like MFT records, INDX blocks are multi-sector structures that require fixup processing. The `vcn` field identifies which virtual cluster within the index allocation this block represents. The index node header starts at offset 24.

```c
/* Index entry */
struct ntfs_index_entry_header {
    UINT64  mft_reference;         /* low 6 bytes = record num, high 2 = seq */
    UINT16  entry_length;
    UINT16  stream_length;         /* length of $FILE_NAME data */
    UINT32  flags;                 /* 0x01=has sub-node, 0x02=last entry */
};

#pragma pack()
```

Each index entry contains an MFT reference to the file or directory it describes, the total entry length, the length of the embedded `$FILE_NAME` data (the `stream_length`), and flags. The `NTFS_INDEX_ENTRY_LAST` flag (bit 1) marks the sentinel entry at the end of each node -- this entry has no filename data and serves only as a terminator. The `NTFS_INDEX_ENTRY_SUBNODE` flag (bit 0) indicates that a child node pointer follows the entry data, which we ignore since we are scanning all entries rather than searching the B+ tree in sorted order.

### Constants

The constants define attribute types, MFT flags, and operating limits:

```c
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
```

The attribute type constants correspond to well-known NTFS attribute types. We only use a subset: `0x10` (standard info -- not directly, but for completeness), `0x20` (attribute list), `0x30` (filename), `0x60` (volume name), `0x80` (data), `0x90` (index root), `0xA0` (index allocation), `0xB0` (bitmap for free-space calculation). The `0xFFFFFFFF` end marker terminates every attribute chain.

The well-known MFT records are fixed by the NTFS specification: record 0 is the MFT itself (self-referential), record 3 is the `$Volume` metadata file (which holds the volume label), and record 5 is the root directory. Record 6 is the `$Bitmap` file (used by `ntfs_volume_info` to count free clusters).

The operating limits are generous but bounded. `NTFS_MAX_RUNS` at 512 handles files fragmented into up to 512 extents. `NTFS_MAX_DIR_ENTRIES` at 1024 caps the deduplication array for a single directory listing. `NTFS_MAX_MFT_SIZE` at 4096 and `NTFS_MAX_INDX_SIZE` at 65536 are sanity bounds -- any record claiming to be larger than these is treated as corrupt.

### Volume State

The runtime state for a mounted NTFS volume:

```c
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
```

The `ntfs_cache_entry` struct is a single slot in the sector cache -- a sector number, a data buffer, and a validity flag. The cache is small (16 entries) because we only need it for the uncommon case where device blocks are larger than NTFS sectors.

The `ntfs_extent` struct represents a decoded data run -- a mapping from virtual cluster numbers (VCNs, positions within a file) to logical cluster numbers (LCNs, positions on the physical volume). Every non-resident attribute is described by a sequence of these extents.

The `ntfs_vol` struct holds everything needed to read from the volume. The `read_fn` and `ctx` fields are the callback and its context. The geometry fields (`bytes_per_sector`, `sectors_per_cluster`, `bytes_per_cluster`, `mft_record_size`, `index_block_size`) come from the boot sector. The `mft_cluster` field is where the MFT starts on disk.

The most important cached state is `mft_extents` and `mft_extent_count` -- the decoded data runs for the `$MFT` file itself. To read any MFT record by number, we need to know where that record lives on disk. The MFT's own data runs tell us. These are parsed once during mount and reused for every subsequent MFT read. The raw bytes are also saved in `mft_runs` in case we need to re-parse them.

### Forward Declarations

The forward declarations serve as a roadmap of the internal functions:

```c
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
```

The layering is visible in these declarations. At the bottom: sector and cluster I/O. Above that: fixup sequences and data run decoding. Above that: MFT record reading and attribute search. Above that: path resolution and name comparison. Each layer builds on the one below, and the public API functions at the top of the file call down through these layers to satisfy user requests.

## Utility Functions: ARM-Safe Reads

NTFS stores all multi-byte values in little-endian format. On x86_64 this is the native byte order, and you could read values with simple pointer casts. On ARM64 you cannot -- unaligned memory access causes a fault. Even on x86_64, reading a `UINT32` from a `UINT8*` pointer that is not 4-byte aligned is undefined behavior in C, even though the hardware tolerates it.

The solution is four small helper functions that use `mem_copy` (our shim's equivalent of `memcpy`) to read values safely from any byte offset:

```c
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
```

These functions appear everywhere in the driver. Every access to a packed structure field goes through `rd16`, `rd32`, or `rd64`. The `rd_s8` function exists specifically for the signed `clusters_per_mft_record` and `clusters_per_index_block` fields in the boot sector, where the sign bit determines the interpretation.

The performance cost is negligible. `mem_copy` of 2, 4, or 8 bytes compiles to a couple of load/store instructions. The alternative -- casting `UINT8*` to `UINT32*` and dereferencing -- would save one instruction per read but crash on ARM64 when the pointer is not naturally aligned. Since MFT record data is sliced and diced at arbitrary byte offsets, unaligned access is the common case, not the exception.

## Sector Cache

The sector cache exists for one specific scenario: when the underlying device's block size is larger than NTFS's logical sector size. This happens rarely -- most devices use 512-byte blocks matching NTFS's 512-byte sectors -- but when it does happen, we need to read a full device block and extract the NTFS sector from within it. The cache avoids re-reading the same device block multiple times.

```c
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
```

Initialization zeros the cache array and resets the clock pointer. Freeing walks every slot and releases any allocated buffers. The cache is embedded in the `ntfs_vol` struct (not separately allocated), so it lives as long as the volume is mounted.

```c
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

    /* Cache miss -- find slot (simple clock) */
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
```

The cache lookup is a linear scan of 16 entries -- fast enough for 16 slots. On a cache hit, we return the pointer directly; the caller reads from the cached buffer without copying. On a miss, we evict the slot at the current clock position and advance the clock. This is the simplest possible eviction policy: round-robin, no access tracking.

The LBA conversion handles three cases. When device blocks and NTFS sectors are the same size (the common case), the sector number is the LBA directly. When device blocks are smaller, we multiply. When device blocks are larger, we divide -- this is the case the cache actually helps with, because reading one device block gives us data for multiple NTFS sectors.

```c
/* Invalidate entire cache (e.g., if we switch volumes) */
static void ntfs_cache_invalidate(struct ntfs_vol *vol)
{
    for (int i = 0; i < NTFS_CACHE_SIZE; i++)
        vol->cache[i].valid = 0;
}
```

Cache invalidation marks all entries as invalid without freeing the buffers. The buffers are reused on the next cache miss. This function exists for the case where the underlying device changes -- unlikely for a mounted volume, but defensive coding costs nothing.

## Low-Level I/O

Three functions handle reading data from the volume at increasing levels of abstraction: sectors, clusters, and arbitrary byte ranges.

```c
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
```

The first case -- 1:1 mapping -- is a direct passthrough to the block read callback. No translation, no overhead. This is the path taken by virtually all real hardware where both the device and NTFS use 512-byte sectors.

The second case -- device blocks smaller than NTFS sectors -- multiplies up. If the device uses 256-byte blocks and NTFS uses 512-byte sectors, each NTFS sector is two device blocks. The math is straightforward: LBA is `sector * ratio`, count is `count * ratio`. The data arrives contiguously in the caller's buffer.

The third case -- device blocks larger than NTFS sectors -- is the interesting one. Here we must read entire device blocks through the cache and extract the relevant NTFS sector from within. Each sector is processed individually because consecutive NTFS sectors might span device block boundaries. This case is rare in practice but handling it correctly means the driver works with any device block size, not just the common 512 bytes.

```c
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
```

Cluster reads are a thin wrapper over sector reads. A cluster is just a group of consecutive sectors, so reading N clusters starting at cluster C means reading `N * sectors_per_cluster` sectors starting at sector `C * sectors_per_cluster`. This function exists for readability -- callers that think in clusters (data run processing, extent reading) do not need to do the multiplication themselves.

```c
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
```

`ntfs_read_bytes` handles the most general case: reading an arbitrary range of bytes from any offset on the volume. If the read happens to be sector-aligned (both the start offset and the length are multiples of the sector size), it falls through to `ntfs_read_sectors` directly. Otherwise, it reads one sector at a time into a temporary buffer and copies out just the needed bytes.

This function is used primarily by `ntfs_read_mft_record`, which needs to read a 1024-byte MFT record from an arbitrary byte offset within a cluster. The MFT record might start at any offset within a cluster (multiple records share a cluster), and it might span a cluster boundary. `ntfs_read_bytes` handles both cases transparently.

The temporary buffer is allocated and freed within the function call. In a high-performance driver this would be a concern, but for a read-only driver doing one-off reads of MFT records and index blocks, the overhead is negligible. The common path -- aligned reads -- avoids the allocation entirely.

## Fixup Sequences

Before you can trust anything in an MFT record, you must apply the fixup sequence. This is NTFS's sector-level integrity mechanism, and getting it wrong means reading garbage.

The idea is simple but clever. When NTFS writes a multi-sector structure to disk -- an MFT record, an INDX block -- it replaces the last two bytes of every sector with a signature value. The original values of those last-two-bytes are saved in an Update Sequence Array at the beginning of the record. If a write is interrupted mid-sector (a "torn write"), the signature at the end of the affected sector will not match, and the corruption is detectable on the next read.

To read an MFT record correctly, you must reverse this process: verify that every sector's last two bytes match the expected signature, then replace them with the saved original values from the USA. If any sector fails the signature check, the record is corrupt and must be rejected.

```c
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
```

The function starts by reading the USA offset and count from the record header. These are always at bytes 4-7, regardless of the record type -- both MFT records and INDX blocks place them in the same position. The `usa_count` includes the signature value itself, so a 1024-byte record with 512-byte sectors has `usa_count = 3`: one signature value and two replacement values (one per sector).

The validation checks are important. The count minus one must equal the number of sectors in the record. The USA must fit within the record bounds. These checks catch corrupt records early, before we try to use any of the data.

The core loop walks each sector in the record. For each sector, it reads the last two bytes and compares them to the expected signature value (`usv`). If they match, the sector was written completely -- the signature was placed there by NTFS during the write, confirming the sector was not torn. The function then replaces those two bytes with the original values from the USA, restoring the data to its true content.

If any sector's last two bytes do not match the signature, the record is corrupt. We return -1 and the caller handles the error -- typically by skipping that record or returning an error to the public API.

A concrete example: a 1024-byte MFT record with 512-byte sectors. The USA at, say, offset 48 contains three 16-bit values: `[0x1234, 0xABCD, 0xEF01]`. The value `0x1234` is the signature. Byte 510-511 of the record should contain `0x1234` -- if they do, we replace them with `0xABCD`. Byte 1022-1023 should also contain `0x1234` -- if they do, we replace them with `0xEF01`. Now the record's data is correct at every position.

The same mechanism applies to INDX blocks. A 4096-byte INDX block with 512-byte sectors has 8 sectors, so `usa_count = 9`: one signature and eight replacement values. Every multi-sector structure in NTFS uses fixup sequences. Forget to apply them, and bytes 510-511, 1022-1023, and so on in every record will contain the wrong values -- corrupting whatever attribute data happens to span those positions.

## Data Run Decoding

A non-resident attribute stores its data in clusters on disk, described by a sequence of "data runs." Each run specifies a cluster count and a cluster offset. The encoding is compact and variable-length -- a run can be as short as two bytes or as long as seventeen. This is the most interesting piece of the entire driver, and the most error-prone to implement.

A data run list is a sequence of entries terminated by a zero byte. Each entry starts with a header byte whose low nibble is the size (in bytes) of the cluster count field, and whose high nibble is the size of the cluster offset field:

```
header byte: [offset_size : 4 bits] [length_size : 4 bits]
```

After the header come `length_size` bytes encoding the cluster count as an unsigned little-endian integer, then `offset_size` bytes encoding the cluster offset as a signed little-endian integer. The offset is delta-encoded: it is relative to the starting LCN of the previous run. The first run's offset is relative to LCN 0 (the beginning of the volume).

For example, a header byte of `0x31` means: 1 byte for the cluster count, 3 bytes for the offset. A header of `0x11` means 1 byte each. A header of `0x43` means 3 bytes for count, 4 for offset.

```c
/*
 * Parse a data run list from raw bytes into an array of extents.
 * Returns number of extents parsed, or -1 on error.
 *
 * Data runs are a compressed encoding:
 *   byte 0: header -- low nibble = length_size, high nibble = offset_size
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
```

The function walks the data run list byte by byte. At each step, it reads the header byte, extracts the nibble sizes, validates them (length size must be 1-8, offset size must be 0-8), and checks that enough bytes remain in the buffer.

The length field is read as an unsigned integer: each byte contributes 8 bits of the value, in little-endian order. A 2-byte length field with bytes `[0x00, 0x01]` means 256 clusters (0x0100). A 1-byte field with value `0x0A` means 10 clusters. The length is never negative -- you cannot have a run of negative clusters.

The offset field is more complex because it is signed. The raw bytes are read as if unsigned, then sign-extended if the highest bit of the last byte is set. This is the critical subtlety. A 3-byte offset of `0xFFFE00` is not 16,776,704 clusters forward -- it is -512, meaning "512 clusters before the previous run's start." Without sign extension, every backward run would be misinterpreted as a massive forward jump, and the driver would read from the wrong part of the disk.

The sign extension works by checking the high bit of the most significant byte. If it is set, the value is negative, and we fill all higher bytes with `0xFF`. A 3-byte value `0xFE, 0xFF, 0xFF` has its high bit set (byte 2 is `0xFF`, and `0xFF & 0x80` is true), so bytes 3 through 7 are filled with `0xFF`, producing the 64-bit value `0xFFFFFFFFFFFFFF FE` which is -2 in two's complement.

The delta encoding is what makes data runs compact. Instead of storing the absolute cluster position for each run, NTFS stores the difference from the previous run's position. A file whose data is contiguous on disk needs only one run. A file split into two fragments needs two runs, where the second run's offset is the signed distance from the first fragment's start to the second fragment's start. Because most files are reasonably contiguous, these deltas tend to be small, fitting in 1-3 bytes rather than the 6-8 bytes an absolute position would require.

A special case: if `off_size` is zero, the run is sparse. No clusters are allocated on disk for this range; the data is logically zero-filled. We record it with LCN 0, and the read function (shown below) fills the output buffer with zeros for sparse extents. We do not fully support sparse files (compression, sparse allocation tracking), but we do handle the encoding correctly -- a sparse run is simply one where no disk read is needed.

The function also tracks virtual cluster numbers. Each run starts at the VCN where the previous run ended. The first run starts at VCN 0. If the first run is 100 clusters long, the second run starts at VCN 100. This sequential tracking allows callers to locate data by VCN -- given a byte offset within a file, divide by the cluster size to get the target VCN, then scan the extents to find which one contains that VCN.

```c
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
            /* Sparse run -- fill with zeros */
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
                /* Partial last cluster -- need temp buffer */
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
```

This function converts a list of extents into actual data. It walks each extent in order, reading the clusters it describes and copying the data into the output buffer. Sparse runs (LCN 0) are handled by zero-filling the corresponding region of the buffer.

The chunked reading is a practical optimization. Rather than reading one cluster at a time (which would produce thousands of individual block I/O calls for a large file), the function reads up to 64 clusters per call. This reduces call overhead significantly. The chunk size of 64 is a balance between reducing call count and avoiding excessive temporary buffer allocation.

The partial-cluster case at the end of a file requires special handling. If the file's `real_size` is not a multiple of the cluster size (the typical case -- most files do not end on a cluster boundary), the last read needs to fetch a full cluster from disk but copy only the valid portion into the output buffer. A temporary buffer is allocated for this case, the full cluster is read into it, and only the needed bytes are copied out.

## MFT Record Reading

With sector I/O, fixup sequences, and data run decoding in place, we can read any MFT record by number. This is the central operation of the entire driver -- every file access, every directory listing, every path resolution ultimately calls this function.

```c
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
                /* Record spans clusters -- read first part */
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
```

The function takes a record number and produces the complete, fixup-applied MFT record in the caller's buffer. The logic has three phases: locate the record on disk, read it, and validate it.

Location is the interesting part. The MFT is itself a file -- MFT record 0 -- and its data is described by data runs just like any other non-resident attribute. During mount, we decoded those data runs into the `mft_extents` array. To find record N, we calculate its byte offset within the MFT data (`record_num * rec_size`), convert to a cluster offset, and scan the extent array to find which extent contains that cluster.

Once we find the right extent, we calculate the physical cluster on disk: `ext_lcn + (cluster_offset - ext_vcn)`. The `ext_lcn` is where the extent starts on the physical volume, and we add the offset within the extent. Combined with the offset within the cluster (`off_in_cluster`), this gives us the exact byte position of the MFT record on disk.

The cluster-spanning case adds complexity. If `off_in_cluster + rec_size > bpc`, the record straddles two clusters. We read the first portion from the current cluster, then need to find the next cluster -- which might be in a different extent. The inner loop handles this by searching the extent array again for the next virtual cluster number. In practice, with 4 KB clusters and 1024-byte MFT records, four records fit per cluster, and spanning never occurs. But with 512-byte clusters or 4096-byte MFT records, spanning is common, and handling it correctly is essential.

After reading the raw bytes, we verify the `FILE` signature and apply the fixup sequence. If either check fails, the record is corrupt and we return -1. The caller sees a clean, fully validated MFT record with all sector-level fixups applied.

## Attribute Search

Once we have an MFT record in a buffer, we need to find specific attributes within it. The attribute chain is a sequence of variable-length entries starting at `first_attr_offset` and ending when the type field reads `0xFFFFFFFF`.

Three functions handle attribute search. `ntfs_find_attr` finds the first attribute matching a type and optional name. `ntfs_find_attr_next` continues searching after a previous match. `ntfs_find_attr_any` finds the first attribute of a given type regardless of its name.

```c
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
```

`ntfs_find_attr` is a convenience wrapper that delegates to `ntfs_find_attr_next` with `after = NULL`, meaning "start from the beginning."

```c
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
```

The search walks the attribute chain from the starting position. For each attribute, it reads the type and length, checks for the end marker, validates the length (must be at least 16 bytes and must not extend past the record's used size), and compares the type.

The name matching is where things get subtle. When `name` is NULL and `name_len` is zero, we are looking for an unnamed attribute -- and we only match attributes whose `name_length` is also zero. When `name` is provided, we compare the attribute's UTF-16 name character by character using `rd16` for alignment-safe reads. This distinction matters because `$INDEX_ROOT` and `$INDEX_ALLOCATION` in directories are named `$I30` -- they are not unnamed. Searching for an unnamed attribute of type `0x90` will find nothing in a directory, even though the attribute is right there.

```c
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
```

`ntfs_find_attr_any` is simpler: it matches on type alone, ignoring the attribute name entirely. This is the function used for finding `$INDEX_ROOT` and `$INDEX_ALLOCATION`, which are always named `$I30` in directories. Using `ntfs_find_attr` with `name=NULL` for these would fail, because that specifically looks for unnamed attributes.

This was one of the two bugs found during testing. The initial implementation used `ntfs_find_attr(mft_buf, size, NTFS_AT_INDEX_ROOT, NULL, 0)`, which looks for an unnamed `$INDEX_ROOT` attribute. But directory index attributes are always named `$I30`. Every directory came back empty until the search was changed to `ntfs_find_attr_any`, which ignores the name and matches any `$INDEX_ROOT` regardless of what it is called.

## Reading Attribute Data

Once we have found an attribute, we need to read its data. This function handles both resident and non-resident attributes transparently:

```c
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
```

For resident attributes, the data sits inline in the MFT record. We read `value_length` from offset +16 and `value_offset` from offset +20 (the correct offsets, after fixing the bug described earlier), allocate a buffer, and copy the data out. The +1 allocation and null termination are a convenience for callers that will interpret the data as strings (like volume names).

For non-resident attributes, we read the data runs offset from +32 and the real size from +48, parse the data runs into extents, allocate a buffer for the full data, and read from disk. The extent array is allocated at the maximum size (512 entries) and freed after use. The data buffer is allocated at the exact `real_size` plus one byte for null termination.

The function always allocates and returns a new buffer. The caller is responsible for freeing it with `mem_free`. This ownership model is simple and unambiguous -- every call that returns data also allocates the buffer for it.

```c
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
```

`ntfs_read_attr_data_into` is the pre-allocated variant. Instead of allocating a new buffer, it reads into a caller-provided buffer of a specified size. It reads at most `buf_size` bytes, clamping to `real_size` if the attribute is smaller. This function is used internally when we already have a buffer of known size -- for example, when reading bitmap data in chunks for the volume info calculation.

## Name Utilities

NTFS stores all filenames as UTF-16LE. Our workstation works in ASCII. Three utility functions bridge this gap, plus an insertion sort for directory listings.

```c
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
```

The UTF-16 to ASCII conversion is deliberately lossy. Characters in the printable ASCII range (0x20 through 0x7E) pass through directly. Null characters terminate the string. Everything else -- accented characters, CJK characters, emoji -- becomes `?`. This is acceptable for a survival workstation. The filenames are readable enough to identify files, and we avoid the complexity of full Unicode rendering.

Note the use of `rd16` to read each UTF-16 character. The source pointer comes from on-disk data that may not be 2-byte aligned (it sits at arbitrary offsets within MFT records), so a direct dereference of `src[i]` could fault on ARM64. Reading through `rd16` handles alignment safely.

```c
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
```

Case-insensitive comparison for ASCII strings. NTFS filenames are case-insensitive (in the Win32 namespace), so path resolution must compare case-insensitively. The function converts both characters to lowercase before comparing, using the simple rule that uppercase A-Z becomes lowercase a-z. This is only correct for ASCII, but since we have already converted all filenames to ASCII, it is sufficient.

```c
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
```

This function compares a UTF-16 name from disk directly against an ASCII path component. It is used during path resolution to find a specific filename in a directory index without first converting the entire directory to ASCII. The early length check avoids unnecessary work -- if the lengths differ, the names cannot match. Each UTF-16 character is converted to ASCII (non-ASCII characters cause an immediate mismatch), then both sides are lowercased and compared.

```c
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
```

The sort uses insertion sort with a two-level comparison: directories come before files, and within each group, entries are sorted alphabetically (case-insensitive). Insertion sort is O(n^2), but for directory listings capped at 1024 entries, it is fast enough and avoids the implementation complexity of quicksort or mergesort. The `mem_copy` calls (rather than struct assignment) ensure correct behavior on ARM64 where `struct fs_entry` may have alignment requirements.

## Directory State and Entry Parsing

NTFS directories present a unique challenge: each file can have multiple directory entries (one for the Win32 name, one for the DOS 8.3 name). Without deduplication, a directory listing would show every file twice. The `ntfs_dir_state` structure tracks which MFT records we have already seen and which namespace their names came from:

```c
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
```

The `entries` array and `max_entries` limit come from the caller. The `mft_refs` and `namespaces` arrays are allocated internally to track deduplication state. For each entry added, we record its MFT record number and the namespace of the name we stored. This allows us to replace DOS names with Win32 names when we encounter them later.

```c
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
       No -- we collect everything and dedup above handles it.
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
```

This function is the heart of the deduplication logic. It receives raw `$FILE_NAME` data from an index entry and decides whether to add it, skip it, or replace an existing entry.

First, it checks for the `.` and `..` entries that every directory contains. These are skipped -- the file browser handles parent navigation separately, and showing them in the listing would be confusing.

The MFT reference is masked to 48 bits (6 bytes) to extract the record number, discarding the 2-byte sequence number. The dedup check scans the existing entries for a matching record number. If found, the namespace determines the outcome: a new DOS name for an existing entry is discarded (we already have a better name). A new Win32 name replacing an existing DOS name triggers a replacement -- the entry's name, flags, and size are updated in place. Any other combination keeps the existing entry.

For new entries (no duplicate found), the function extracts the flags and file size from the `$FILE_NAME` data, converts the UTF-16 name to ASCII, and appends to the entry array. If the array is full, the entry is silently dropped. This is a practical limit, not a correctness issue -- directories with more than 1024 entries are rare on USB drives.

The two separate code paths -- dedup replacement and new entry addition -- both read the same fields from `fn_data`: flags at offset 56, real_size at offset 48, name_length at offset 64, and the name starting at offset 66. These offsets correspond to the `ntfs_filename` structure, but we read them by offset rather than through the struct to avoid alignment issues.

```c
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
```

This function walks a sequence of index entries and feeds each one to `ntfs_dir_add_entry`. The format is the same whether the entries come from an `$INDEX_ROOT` attribute or an INDX block: each entry starts with the 16-byte `ntfs_index_entry_header` followed by the embedded `$FILE_NAME` data.

The `NTFS_INDEX_ENTRY_LAST` flag marks the sentinel entry at the end of each index node. This entry has no `$FILE_NAME` data -- it exists only to mark the end of the list and optionally point to a child node (for B+ tree traversal). We stop parsing when we see it.

The `entry_len` check at the bottom prevents infinite loops: if an entry claims to have zero length, advancing by zero would loop forever. Malformed entries are handled by breaking out of the loop rather than returning an error -- a partially parsed directory is more useful than none at all.

```c
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
```

An INDX block is a self-contained index node that lives in clusters on disk. Before we can parse its entries, we must verify the `INDX` signature and apply the fixup sequence -- INDX blocks are multi-sector structures just like MFT records, and their last-two-bytes-per-sector have been replaced with the USA signature.

The index node header starts at offset 24 within the INDX block (after the block header with signature, USA info, LSN, and VCN). The `entries_offset` field within the node header is relative to the start of the node header itself, so the absolute offset of the first entry within the block buffer is `24 + entries_offset`. The `total_size` is clamped to the block bounds as a safety measure.

## Directory Reading

With all the parsing infrastructure in place, we can read an entire directory. Two functions handle this: `ntfs_read_dir_entries` for the normal case, and `ntfs_read_dir_entries_full` which falls back to the `$ATTRIBUTE_LIST` mechanism for large directories whose index data is split across multiple MFT records.

```c
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

    /* --- Parse INDEX_ROOT (0x90) -- named "$I30" --- */
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
```

This is one of the longest functions in the driver, and it handles two very different cases. A small directory -- one with a handful of entries -- stores everything in `$INDEX_ROOT`, which is always resident within the MFT record. A large directory overflows into `$INDEX_ALLOCATION`, which is a non-resident attribute containing INDX blocks on disk.

The function starts by reading the MFT record and verifying it is a directory (bit 1 of the flags). It then sets up the `ntfs_dir_state` with the dedup tracking arrays.

The `$INDEX_ROOT` parsing is straightforward. The attribute value contains an index root header (16 bytes) followed by an index node header (16 bytes) followed by index entries. The `entries_offset` in the node header tells us where the entries start relative to the node header, so the actual start within the attribute value is `16 + entries_offset`. The `node_flags` bit 0 tells us whether the directory also has `$INDEX_ALLOCATION` -- if not, the root node contains all entries and we are done.

The `$INDEX_ALLOCATION` parsing is more involved. The attribute is non-resident, so we must decode its data runs to find where the INDX blocks live on disk. We allocate a single INDX-block-sized buffer and reuse it for each block, reading one block at a time. For each extent, we calculate how many INDX blocks fit in the extent's clusters, compute the disk byte offset for each block, read it, and parse its entries through `ntfs_parse_indx_block`.

The byte offset calculation for each INDX block within an extent handles the case where the index block size differs from the cluster size. If index blocks are 4096 bytes and clusters are also 4096 bytes, each cluster is one INDX block and the calculation is trivial. If clusters are smaller (say, 512 bytes), multiple clusters make up one INDX block. If clusters are larger (say, 64 KB), multiple INDX blocks fit in one cluster. The `block_byte_off / bpc` and `block_byte_off % bpc` arithmetic handles all cases.

## Path Resolution and Name Lookup

Path resolution walks from the root directory (MFT record 5) through each path component, looking up each name in the current directory's index.

```c
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
                                                    /* DOS match -- save but
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
```

`ntfs_lookup_name` is the longest function in the driver because it inlines the index entry walking rather than delegating to `ntfs_parse_index_entries`. This is intentional: for a name lookup, we want to stop as soon as we find a match rather than collecting every entry. The dedup machinery is not needed because we are looking for exactly one name.

The search starts in `$INDEX_ROOT`, walking entries and comparing each filename against the target using `ntfs_name_icmp_utf16`. If a match is found in the Win32 or POSIX namespace, the function returns immediately. If a match is found only in the DOS namespace, the function records it but continues looking -- a Win32 name for the same file might appear later, and we prefer it.

If the root node has the large-index flag set, the function continues into `$INDEX_ALLOCATION`, reading INDX blocks one at a time and searching each one. The inline fixup verification (checking the `INDX` signature and applying fixup) allows the function to skip corrupt blocks rather than aborting the entire search. The name comparison logic is duplicated from the root node search.

The `goto done` pattern ensures that the MFT buffer is freed on every exit path. Without it, the deeply nested if/for/while blocks would require either a complex cleanup chain or redundant `mem_free` calls at every return point.

```c
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
```

Path resolution is clean and simple. Starting at MFT record 5 (the root directory), it splits the path on `/` separators, extracts each component into a local buffer, looks it up in the current directory, and uses the result as the starting point for the next component. If any component is not found, the entire resolution fails.

The root path `/` is handled as a special case, returning MFT record 5 directly without any lookup. Empty path components (from doubled slashes like `//`) are skipped. The trailing slash after the last component is also skipped, so `/dir/file` and `/dir/file/` resolve the same way.

## File Info Helpers

Two small functions provide metadata about files and directories without reading their data.

```c
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
```

`ntfs_is_directory` reads the MFT record and checks bit 1 of the flags field. This is used by the public API's `ntfs_readfile` function to verify that the target is a file, not a directory, before attempting to read its data.

```c
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
```

`ntfs_get_file_size` finds the unnamed `$DATA` attribute and reads its size. For non-resident data, the `real_size` at offset +48 gives the actual file size. For resident data (small files stored inline in the MFT record), the `value_length` at offset +16 is the size. Directories return 0 because they do not have a meaningful "file size."

Note that this function uses `ntfs_find_attr` (which matches unnamed attributes) rather than `ntfs_find_attr_any`. This is correct because the `$DATA` attribute for a file's primary data stream is always unnamed. Named `$DATA` attributes are alternate data streams, which we do not support.

```c
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
```

The volume label is stored in MFT record 3 (`$Volume`) as a `$VOLUME_NAME` attribute (type `0x60`). This attribute is always resident -- the label is just a short UTF-16 string. We read it during mount and cache it in the `vol->label` field so that `ntfs_get_label` can return it without re-reading the MFT.

The conversion from UTF-16 to ASCII uses `ntfs_utf16_to_ascii`, which replaces non-ASCII characters with `?`. Volume labels are typically plain ASCII anyway ("WINDOWS", "DATA", "BACKUP"), so the lossy conversion rarely matters.

## $ATTRIBUTE_LIST Support

Most files and directories fit their attributes comfortably within a single 1024-byte MFT record. But some do not. A file with a very long filename, many alternate data streams, or extensive security descriptors might overflow its MFT record. A directory with thousands of entries might need a `$INDEX_ALLOCATION` attribute whose data runs are too large to fit alongside the other attributes.

When this happens, NTFS creates extension MFT records. The base record (the one with the file's own record number) gets an `$ATTRIBUTE_LIST` attribute (type `0x20`) that catalogs every attribute belonging to this file and identifies which MFT record holds it. Some attributes stay in the base record; others are moved to extension records.

The `$ATTRIBUTE_LIST` is itself an attribute, and it can be either resident (for small lists) or non-resident (for large files with many split attributes). Each entry in the list has a fixed layout:

```
offset  0: UINT32  attribute type
offset  4: UINT16  record length
offset  6: UINT8   attribute name length
offset  7: UINT8   attribute name offset
offset  8: UINT64  starting VCN (for non-resident attributes)
offset 16: UINT64  MFT reference (record holding the attribute)
offset 24: UINT16  attribute ID
```

Two functions handle the attribute list: one for reading file data, one for reading directory index data.

```c
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
                        /* Resident $DATA in extension record --
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
```

This function handles the case where a file's `$DATA` attribute is split across multiple MFT records. It reads the `$ATTRIBUTE_LIST`, walks its entries looking for `$DATA` (type `0x80`) with no name (the primary data stream), reads each referenced extension MFT record, extracts the data runs from each `$DATA` attribute found, and collects all the extents into a single array.

The VCN adjustment is critical. Each extension record's `$DATA` attribute has a `starting_vcn` at offset +16 that tells us where its data runs begin within the overall file. The data runs within each attribute are relative to that starting VCN, but our global extent array needs absolute VCNs. Adding `starting_vcn` to each parsed extent's VCN gives us the correct global position.

After collecting all extents, we sort them by VCN using insertion sort. They should already be in order (the attribute list entries are sorted by starting VCN), but sorting defensively costs almost nothing and prevents subtle bugs from out-of-order attribute list entries.

Finally, we allocate a buffer for the full data size, read all extents into it, and return it to the caller. The `real_size` is taken as the maximum across all extension records' `$DATA` attributes -- each one reports the total file size, so they should all agree.

```c
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
```

This is the directory equivalent of `ntfs_read_file_data_via_attrlist`. Instead of collecting `$DATA` extents and reading file data, it collects `$INDEX_ALLOCATION` extents and reads INDX blocks. The pattern is the same: walk the attribute list, find entries of the target type, read extension MFT records, extract data runs, adjust VCNs, sort, and process.

The difference is in the processing step. For file data, we read everything into a single buffer. For directory data, we read one INDX block at a time and parse its entries through `ntfs_parse_indx_block`, which feeds them into the `ntfs_dir_state` with deduplication. This avoids allocating a buffer for the entire index allocation, which could be many megabytes for a directory with thousands of entries.

```c
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
```

`ntfs_read_dir_entries_full` is the entry point for all directory reads from the public API. It tries the simple path first -- `ntfs_read_dir_entries`, which handles directories whose attributes fit in a single MFT record. If that fails, it falls back to the attribute list path: re-reads the MFT record, parses `$INDEX_ROOT` from the base record, and delegates `$INDEX_ALLOCATION` reading to `ntfs_read_dir_via_attrlist`.

This two-pass approach means the simple case (the vast majority of directories) incurs no attribute list overhead. The complex case is handled transparently. The caller -- `ntfs_readdir` -- does not know or care which path was taken.

## The Public API

The eight public functions are the interface between the NTFS driver and the rest of the workstation. Each one validates its inputs, delegates to the internal functions, and presents results in the format the volume abstraction layer expects.

### ntfs_mount

```c
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
        /* Resident $MFT data -- extremely unlikely, but handle it */
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
```

`ntfs_mount` is the largest and most critical function in the driver. It bootstraps the entire volume from a raw block device. The process has several phases:

**Phase 1: Boot sector.** Read LBA 0, verify the `NTFS` OEM ID, parse the BPB fields. Calculate the cluster size, record sizes, and total volume size. The signed encoding for MFT record size and index block size is decoded here -- a value of -10 becomes `1 << 10 = 1024`.

**Phase 2: MFT record 0.** The boot sector tells us the MFT's starting cluster. We read that cluster, verify the `FILE` signature, and apply the fixup sequence. This gives us the raw MFT record for the `$MFT` file itself.

**Phase 3: $MFT data runs.** Within MFT record 0, we find the unnamed `$DATA` attribute. Its data runs describe where every MFT record on the volume is stored. We copy the raw run bytes and parse them into our extent array. This is the single most important piece of cached state -- without it, we cannot locate any other MFT record.

**Phase 4: $ATTRIBUTE_LIST handling.** On very large volumes, the `$MFT` file itself may be fragmented enough that its data runs do not fit in a single MFT record. If MFT record 0 contains an `$ATTRIBUTE_LIST`, we read it, find extension records that hold additional `$DATA` runs, parse those runs, merge them with the existing extents, and sort by VCN. This ensures we can locate MFT records in any part of the volume, even on heavily fragmented disks.

**Phase 5: Volume label.** Finally, we read MFT record 3 (`$Volume`) to extract the volume label, which appears in the file browser next to the `[NTFS]` tag.

Every phase can fail, and every failure path cleans up all previously allocated resources. The cleanup chain grows longer as we progress through the function -- by phase 4, a failure must free the extent array, the run bytes, the MFT record buffer, the sector cache, and the volume structure itself. This telescoping cleanup is ugly but necessary in C code without exceptions or RAII.

### ntfs_unmount

```c
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
```

Unmounting frees everything: the sector cache (including its buffers), the raw MFT run bytes, the decoded extent array, and the volume structure itself. The null checks on `mft_runs` and `mft_extents` are defensive -- they should always be set after a successful mount, but checking costs nothing.

### ntfs_readdir

```c
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
```

`ntfs_readdir` is clean: resolve the path to an MFT record number, read the directory entries with full attribute list support, sort the results (directories first, then alphabetical), and return the count. The `struct fs_entry` array is shared with the FAT32 and exFAT backends, so the file browser displays NTFS directories the same way it displays any other filesystem's directories.

### ntfs_readfile

```c
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

    /* $DATA not found in base record -- check for $ATTRIBUTE_LIST */
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
```

File reading resolves the path, verifies the target is a file (not a directory), and reads the `$DATA` attribute. The function tries two paths: first, look for `$DATA` directly in the base MFT record. If found, read it (handling both resident and non-resident cases). If not found, check for an `$ATTRIBUTE_LIST` and try reading through the extension records.

The two-path approach handles the normal case (data in the base record) without attribute list overhead, while still supporting the uncommon case (data split across extension records) when needed. Most files on a typical USB drive will take the first path. Only very large files or files with many attributes will need the attribute list fallback.

The function returns a newly allocated buffer that the caller must free. On error, it returns NULL and sets `*out_size` to 0.

### ntfs_volume_info

```c
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
                        /* Non-resident bitmap -- read in 64KB chunks */
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
```

Volume info is conceptually simple -- report total size and free space -- but the implementation is the most deeply nested function in the driver. Total size is easy: `total_sectors * bytes_per_sector`, both cached from the boot sector.

Free space requires reading the `$Bitmap` file (MFT record 6). This file has one bit per cluster on the volume: set means allocated, clear means free. On a 1 TB volume with 4 KB clusters, the bitmap is about 32 MB. We cannot load 32 MB into memory on a constrained UEFI system, so the non-resident case reads the bitmap in 64 KB chunks, scanning each chunk bit by bit to count free clusters.

The resident case (very small volumes where the bitmap fits in the MFT record) reads the entire bitmap at once and counts in a single pass. The non-resident case decodes the bitmap attribute's data runs, walks each extent, reads 64 KB chunks from disk, and counts free bits. Sparse runs in the bitmap (all-zero ranges, meaning all clusters free) are handled as a special case to avoid unnecessary disk reads.

The deeply nested structure is a consequence of defensive programming: every allocation might fail, every disk read might fail, and we need to continue functioning (returning 0 free bytes) even if the bitmap is unreadable. A production filesystem driver would factor this into helper functions, but for a read-only driver that calls `ntfs_volume_info` once per volume switch, the nesting is tolerable.

### ntfs_file_size, ntfs_exists, ntfs_get_label

```c
UINT64 ntfs_file_size(struct ntfs_vol *vol, const char *path)
{
    if (!vol || !path)
        return 0;

    INT64 mft_num = ntfs_resolve_path(vol, path);
    if (mft_num < 0)
        return 0;

    return ntfs_get_file_size(vol, (UINT64)mft_num);
}
```

`ntfs_file_size` resolves the path and delegates to the internal `ntfs_get_file_size` function. It returns 0 for directories, nonexistent paths, and errors -- the caller must use `ntfs_exists` separately if it needs to distinguish "file does not exist" from "file has zero size."

```c
int ntfs_exists(struct ntfs_vol *vol, const char *path)
{
    if (!vol || !path)
        return 0;

    INT64 mft_num = ntfs_resolve_path(vol, path);
    return (mft_num >= 0) ? 1 : 0;
}
```

`ntfs_exists` is the simplest public function: resolve the path and return 1 if it succeeds, 0 if it fails. Path resolution failure means the path does not exist (or some component along the way does not exist).

```c
const char *ntfs_get_label(struct ntfs_vol *vol)
{
    if (!vol)
        return "";
    return vol->label;
}
```

`ntfs_get_label` returns the cached volume label. The label was read during mount from MFT record 3's `$VOLUME_NAME` attribute and converted to ASCII. If no label was set, the field is an empty string.

## The Standalone Library

The NTFS reader was designed from the start to be separable. It uses callback-based block I/O -- you provide a function pointer that reads blocks from whatever storage device you have. There are no UEFI types in the core logic; the in-tree version uses `UINT64` and `mem_alloc` from the workstation's own headers, but the standalone version uses `uint64_t` and `malloc` from `<stdint.h>` and `<stdlib.h>`.

The extraction process is mechanical. Replace `UINT8` with `uint8_t`, `UINT16` with `uint16_t`, `UINT32` with `uint32_t`, `UINT64` with `uint64_t`, `INT8` with `int8_t`, `INT64` with `int64_t`, and `UINTN` with `size_t`. Replace `mem_alloc` with `malloc`, `mem_free` with `free`, `mem_copy` with `memcpy`, `mem_set` with `memset`, and `str_len` with `strlen`. Replace the `boot.h` and `fs.h` includes with `<stdint.h>`, `<stdlib.h>`, and `<string.h>`. Define `struct fs_entry` locally with a `name` field and `size`/`is_dir` fields.

The result is two files: `ntfs.h` and `ntfs.c`. No dependencies beyond the C standard library. The example program demonstrates the POSIX block I/O backend:

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "ntfs.h"

struct posix_ctx { int fd; };

static int posix_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    struct posix_ctx *p = ctx;
    off_t off = lba * 512;
    if (lseek(p->fd, off, SEEK_SET) < 0) return -1;
    size_t bytes = count * 512;
    if (read(p->fd, buf, bytes) != (ssize_t)bytes) return -1;
    return 0;
}

int main(int argc, char **argv) {
    struct posix_ctx ctx;
    ctx.fd = open(argv[1], O_RDONLY);
    struct ntfs_vol *vol = ntfs_mount(posix_read, &ctx, 512);
    /* ... use ntfs_readdir, ntfs_readfile, etc. ... */
    ntfs_unmount(vol);
    close(ctx.fd);
}
```

The block read callback wraps `lseek` and `read`. The context pointer carries the file descriptor. Mount, use, unmount -- the same sequence as the UEFI version, with a different I/O backend.

No MIT-licensed, standalone NTFS reader existed before this. ntfs-3g is GPL and massive. The Windows driver is closed-source. Various hobby implementations exist but are either incomplete, GPL, or tied to a specific OS. The survival workstation needed one, so we wrote one, and extracting it for reuse cost almost nothing -- the callback-based design meant the only changes were swapping type names and memory allocators.

The standalone library lives on GitHub as a public repository. Two files, MIT licensed, zero dependencies. To use it in another project -- a bootloader, a UEFI shell, an embedded system, a forensics tool -- you copy the two files and provide a block read callback.

## Integration: Browse and Edit

The volume abstraction layer from Chapter 25 handles most of the integration. `fs_readdir`, `fs_readfile`, and the other filesystem functions check `s_vol_type` and dispatch to `ntfs_readdir` or `ntfs_readfile` when the volume is NTFS. The browser treats NTFS volumes the same as exFAT -- navigate in, see files and directories, open files in the editor.

The difference is write protection. `fs_is_read_only` returns 1 for NTFS volumes. The editor checks this function and adjusts its behavior: the title bar shows `[READ-ONLY]` after the filename, the status bar replaces `F2:Save` with nothing, and the user can read, select, and copy text -- but not save. Similarly, `fs_writefile`, `fs_mkdir`, and `fs_rename` return `EFI_WRITE_PROTECTED` for NTFS volumes, which prevents the browser from offering paste, rename, or new-file operations.

In the browser itself, NTFS volumes appear as magenta `[NTFS]` entries alongside the orange `[exFAT]` entries and the standard orange `[USB]` (FAT32) entries. The color coding gives an instant visual signal: orange means full read-write access, magenta means read-only.

## What We Built

About 800 lines of core logic in `src/ntfs.c` (roughly 2,855 lines including all the boilerplate, error handling, and utility functions), plus a clean 8-function public API in `src/ntfs.h`. The driver reads files and directories from NTFS volumes, handles resident and non-resident data, parses small and large directories, resolves paths through the B+ tree index, applies fixup sequences to every multi-sector structure, decodes variable-length signed data runs, deduplicates Win32/DOS filename pairs, and reports volume label and capacity.

Two bugs were found and fixed during development, both in areas where the NTFS documentation is either ambiguous or missing. The first: resident attribute `value_offset` is at byte +20 of the attribute header, not +18. The `value_length` field is 4 bytes (`UINT32`), not 2, pushing the offset field two bytes later than a careless reading of the structure might suggest. The second: `$INDEX_ROOT` and `$INDEX_ALLOCATION` attributes are named `$I30` -- they are not unnamed. Using `ntfs_find_attr` with `name=NULL` to find them returns nothing, because that function specifically matches unnamed attributes. The fix was `ntfs_find_attr_any`, which ignores the name entirely.

The driver does not write. It does not handle compression, encryption, sparse files, or alternate data streams. It does not replay the journal. It is a reader, and only a reader, and that is exactly what a survival workstation needs.

Plug in a Windows user's USB drive. Browse into it. Open their files. Copy what you need. The data is off the drive. That is the entire point.
