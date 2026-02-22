---
layout: default
title: "Chapter 36: Reading FAT32"
parent: "Phase 2: The Survival Toolkit"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 2
---

# Chapter 36: Reading FAT32

## The Missing Half

Our FAT32 module can create filesystems and write files. It formatted the SD card, laid out GPT partitions, wrote the EFI bootloader, the kernel, the root filesystem. But it can't read any of it back.

This asymmetry was fine for a single-purpose flasher. The ESP32 wrote data; Linux read it. Traffic flowed one direction. But the file browser in Chapter 38 needs to list directories, and the text viewer in Chapter 39 needs to read file contents. We need the other half.

The good news: all the hard machinery already exists. `fat_get()` follows FAT chains. `cluster_to_lba()` converts cluster numbers to disk addresses. `read_sector()` loads 512-byte blocks. `find_in_dir()` walks directory entries with full LFN support. `name_to_83()` converts filenames to the 8.3 format. We wrote all of this for the write path. The read path is a thin layer on top.

## Initializing from an Existing Card

`fat32_format()` knows the filesystem geometry because it just created it — it computed `spc`, `fat_start`, `data_start`, and `total_clusters` from the partition size. But what about a card that was formatted elsewhere? A card the user prepared on their laptop, or one formatted by the flasher weeks ago?

`fat32_read_init()` recovers the geometry by parsing the BPB (BIOS Parameter Block) from sector 0 of the partition:

```c
int fat32_read_init(uint32_t partition_start_lba)
{
    s_fs.part_start = partition_start_lba;

    if (read_sector(partition_start_lba, s_buf) < 0)
        return -1;

    struct fat32_bpb *bpb = (struct fat32_bpb *)s_buf;
    if (bpb->signature != 0xAA55 || bpb->fat_size_32 == 0)
        return -1;

    s_fs.spc = bpb->sectors_per_cluster;
    s_fs.fat_sectors = bpb->fat_size_32;
    s_fs.fat_start = partition_start_lba + bpb->reserved_sectors;
    s_fs.data_start = s_fs.fat_start + s_fs.fat_sectors * bpb->num_fats;
    s_fs.total_clusters = (bpb->total_sectors_32 - bpb->reserved_sectors
                           - s_fs.fat_sectors * bpb->num_fats) / s_fs.spc;
    s_fs.next_free_cluster = s_fs.total_clusters + 2;
    return 0;
}
```

Two validation checks: the `0xAA55` boot signature must be present, and `fat_size_32` must be nonzero (zero would indicate FAT12/FAT16, which uses `fat_size_16` instead). Everything else is trusted — we're not a filesystem repair tool.

The last line is important: `next_free_cluster` is set past the end of the volume. This prevents `alloc_cluster()` from ever finding a "free" cluster during read operations. If a bug accidentally calls allocation code, it fails immediately instead of corrupting the card. The write path sets `next_free_cluster` to 3 during format; the read path walls it off.

## Walking Paths Without Creating Them

The write path has `walk_path()`, which resolves a path like `EFI/BOOT` by calling `ensure_dir()` on each component — creating missing directories along the way. That's exactly wrong for reading. Asking "what files are in `photos/vacation`?" shouldn't create a `photos` directory if it doesn't exist.

`find_path()` is the read-only equivalent. Same loop, same component splitting, but it calls `find_in_dir()` instead of `ensure_dir()`:

```c
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
```

If any component doesn't exist, `find_in_dir()` returns 0 and `find_path()` propagates the failure. No side effects, no disk writes.

## Listing Directories

Directory enumeration is a cursor-based API. You open a directory, read entries one at a time, and close when done:

```c
int handle = fat32_open_dir("/EFI/BOOT");
struct fat32_dir_info info;
while (fat32_read_dir(handle, &info) == 1) {
    printf("%s  %lu bytes  %s\n",
           info.is_dir ? "[DIR]" : "     ",
           (unsigned long)info.size,
           info.name);
}
fat32_close_dir(handle);
```

The cursor state lives in a static struct — one directory can be open at a time, matching our single-task UI:

```c
static struct {
    int active;
    uint32_t cluster;       /* current cluster being scanned */
    uint32_t sector;        /* sector offset within cluster */
    int entry;              /* entry index within sector */
    uint16_t lfn_buf[260];  /* accumulated LFN characters */
    int lfn_active;
} s_readdir;
```

`fat32_open_dir()` resolves the path to a starting cluster and initializes the cursor. Empty string or `"/"` gives you the root directory (cluster 2).

`fat32_read_dir()` is where the real work happens. It resumes from wherever the cursor left off and walks directory entries:

- `0x00` in the first byte: end of directory. Return 0.
- `0xE5`: deleted entry. Skip it, reset LFN state.
- `ATTR_LONG_NAME` (`0x0F`): this is an LFN fragment. Accumulate its characters into `lfn_buf`. LFN entries are stored in reverse order — the last fragment comes first, marked with bit 6 set. Each fragment holds 13 UCS-2 characters spread across three fields (offsets 1, 14, and 28 in the 32-byte entry).
- `ATTR_VOLUME_ID`: volume label. Skip it.
- `.` and `..`: parent navigation entries. Skip them — the file browser handles parent navigation with its own back button.
- Anything else: a real file or directory entry.

For real entries, the name comes from one of two sources. If LFN entries preceded this one, the long filename has been accumulating in `lfn_buf` — convert it from UCS-2 to ASCII and use that. If no LFN entries preceded it (the file has a pure 8.3 name), reconstruct the readable name from the 11-byte directory entry.

## Reversing 8.3 Names

`name_to_83()` converts `"readme.txt"` to `"README  TXT"` — uppercase, space-padded, no dot. `name_from_83()` reverses this:

```c
static void name_from_83(const uint8_t *raw, uint8_t nt_flags, char *out)
{
    int pos = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = (char)raw[i];
        if ((nt_flags & 0x08) && c >= 'A' && c <= 'Z') c += 32;
        out[pos++] = c;
    }
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
```

The `nt_reserved` byte in the directory entry holds case flags — a Microsoft extension that predates VFAT. Bit 3 (`0x08`) means the base name was originally all lowercase. Bit 4 (`0x10`) means the extension was all lowercase. Windows sets these flags when creating files like `readme.txt` — the 8.3 name stores `README  TXT` but the flags remember the original case. Without checking these flags, every filename would display in uppercase: `README.TXT` instead of `readme.txt`.

This matters for usability. A file browser showing `README.TXT`, `NOTES.TXT`, `PHOTO.BMP` looks hostile. The same files as `readme.txt`, `notes.txt`, `photo.bmp` look like they belong on a normal computer. Two bits of metadata, one line of code each, significant difference in readability.

## Reading Files

File reading follows the same cursor pattern as directory enumeration:

```c
int handle = fat32_file_open("/notes/todo.txt");
char buf[256];
int n;
while ((n = fat32_file_read(handle, buf, sizeof(buf))) > 0) {
    /* process n bytes in buf */
}
fat32_file_close(handle);
```

`fat32_file_open()` splits the path, resolves the directory with `find_path()`, finds the file entry with `find_in_dir()`, and captures the starting cluster and file size. The file size comes from the directory entry that `find_in_dir()` just loaded into `s_dir` — it's still sitting in the buffer, so we read it directly without another disk access.

`fat32_file_read()` streams data sector by sector. It loads a sector into `s_stream_buf`, copies bytes to the caller's buffer, and advances. When it exhausts a sector, it loads the next one. When it exhausts a cluster (after `spc` sectors), it follows the FAT chain via `fat_get()` to find the next cluster.

The read cursor tracks five things:

```c
static struct {
    int active;
    uint32_t current_cluster;
    uint32_t file_size;
    uint32_t position;
    uint32_t sector_in_cluster;
    uint32_t buf_pos;
} s_readfile;
```

`position` counts total bytes delivered to the caller. When `position` reaches `file_size`, the file is exhausted — return 0 for EOF. `buf_pos` starts at `SECTOR_SIZE` (512), which forces the first call to `fat32_file_read()` to load a sector before copying any data. This avoids special-casing the initial state.

The caller controls the read granularity. Read 1 byte at a time for character-by-character processing. Read 4 KB at a time for bulk transfers. The sector buffering is internal — the caller never sees it.

## Zero New Buffers

The read path allocates no new memory. It reuses the three existing 512-byte sector buffers:

- `s_buf`: FAT table reads via `fat_get()`, same as the write path
- `s_dir`: directory sector reads via `find_in_dir()` and `fat32_read_dir()`
- `s_stream_buf`: file data reads, reused from the streaming write API

`s_stream_buf` is the interesting one. During writes, it buffers outgoing data for `fat32_stream_write()`. During reads, it buffers incoming data for `fat32_file_read()`. These operations can't happen simultaneously — you're either writing a file or reading one — so the buffer serves both purposes safely. The code enforces this: `fat32_file_open()` checks `s_stream.active` and refuses to open if a streaming write is in progress.

1,536 bytes of static buffers serving both the write and read paths. On a system with 520 KB of RAM, every saved buffer matters less than it did on the 64 KB UEFI stack — but the discipline matters. Unnecessary allocation is unnecessary complexity.

## What We Built

Two files modified, zero files created:

```
File         Lines   Changes
───────────  ─────   ──────────────────────────────
fat32.h        80    +fat32_dir_info struct, +7 function declarations
fat32.c      1140    +name_from_83, +find_path, +read_init,
                      +dir enumeration, +file reading (~250 new lines)
```

The API surface is seven functions: one to initialize, three for directories, three for files. The implementation reuses every internal function and buffer from the write path. No new dependencies, no new memory, no build system changes.

The file browser in Chapter 38 will call `fat32_open_dir()` and `fat32_read_dir()` to show directory contents. The text viewer in Chapter 39 will call `fat32_file_open()` and `fat32_file_read()` to display file contents. The plumbing is in place. Now we need the UI.

---

**Next:** [Chapter 37: exFAT Support](chapter-37-exfat-support)
