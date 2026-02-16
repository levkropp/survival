---
layout: default
title: "Chapter 25: Reading and Writing exFAT"
parent: "Phase 5.5: exFAT & NTFS"
nav_order: 1
---

# Chapter 25: Reading and Writing exFAT

## The 32-Gigabyte Wall

Plug a camera's SD card into the workstation and nothing happens. Open the file browser and the card appears as a red `[DISK]` entry -- a raw block device with no filesystem. The same thing happens with most USB drives larger than 32 GB. You can format them to FAT32 (Chapter 24), but then their existing contents are gone. Hundreds of photos, documents, whatever was on the drive -- erased because the workstation cannot read the filesystem they came with.

The problem is FAT32. Microsoft's original FAT32 specification limits volumes to 32 GB when formatted through Windows. Most operating systems default to exFAT for anything larger. Every camera, every phone, every factory-formatted USB drive above 32 GB ships with exFAT. The SDXC standard mandates it. And UEFI firmware knows nothing about exFAT -- the UEFI specification only requires FAT12, FAT16, and FAT32 support through the SimpleFileSystem protocol.

This is the 32-gigabyte wall. On one side, the small world of FAT32 volumes that UEFI can read. On the other side, every modern storage device with real data on it. Our workstation sits on the FAT32 side, blind to the other.

We need to tear down this wall. Not by reformatting drives -- that destroys data. By reading exFAT natively. We need a driver that can parse the exFAT on-disk structures, walk directory trees, read files, and write new ones. The driver must work without any operating system support, without libc, without even UEFI filesystem protocols. Just raw block I/O: give me sector N, here are the bytes.

What we are building in this chapter is substantial: a complete exFAT filesystem driver in about 2,000 lines of C. It supports mounting, directory listing, file reading, file writing, directory creation, rename, and delete. On top of that driver, we build a volume abstraction layer that lets the existing file browser and editor work transparently with exFAT volumes, exactly as they work with FAT32. By the end, you will plug in any exFAT USB drive and browse its contents as naturally as the boot volume.

The chapter has three layers. First, the volume abstraction in `fs.h` and `fs.c` that dispatches filesystem operations to the right backend. Second, the exFAT driver itself in `exfat.h` and `exfat.c`. Third, the UI integration in `browse.c` and `edit.c` that makes custom volumes visible and usable.

## The Volume Abstraction

Until now, every filesystem operation in the workstation went through UEFI's SimpleFileSystem protocol. The functions in `fs.c` -- `fs_readdir`, `fs_readfile`, `fs_writefile` -- all called `s_root->Open()`, `file->Read()`, and similar UEFI file protocol methods. This worked because every volume we touched was FAT32, and UEFI knows how to handle FAT32.

With exFAT (and later NTFS in Chapter 26), we need a dispatch layer. When the user browses a FAT32 volume, calls go through UEFI as before. When browsing an exFAT volume, calls go to our exFAT driver instead. The dispatch must be invisible to the rest of the codebase -- `browse.c` calls `fs_readdir()` and does not care which backend handles it.

### New Types in fs.h

The volume abstraction starts with an enum and a descriptor struct. Here is the addition to `fs.h`:

```c
/* Volume type for dispatch */
enum fs_vol_type { FS_VOL_SFS, FS_VOL_EXFAT, FS_VOL_NTFS };

/* Custom volume descriptor (exFAT/NTFS found on BlockIO handles) */
struct fs_custom_volume {
    EFI_HANDLE      handle;
    enum fs_vol_type type;
    char            label[48];
    UINT64          size_bytes;
};
```

The `fs_vol_type` enum has three cases. `FS_VOL_SFS` is the default -- UEFI SimpleFileSystem, meaning FAT32. `FS_VOL_EXFAT` and `FS_VOL_NTFS` are the two custom backends we will implement. The enum lives in `fs.h` so that browse.c can query the current volume type for UI purposes (showing "[exFAT]" vs "[NTFS]" tags, choosing colors, enabling or disabling write operations).

The `fs_custom_volume` struct describes a discovered exFAT or NTFS volume. It carries the UEFI handle (needed to get the BlockIO protocol), the volume type, a human-readable label, and the total size in bytes. The file browser uses these descriptors to show custom volume entries at the root level, right alongside USB and DISK entries.

Five new function declarations complete the custom volume API:

```c
/* Mount a custom (non-SFS) volume via BlockIO handle.
   Returns 0 on success, -1 on error. */
int fs_set_custom_volume(enum fs_vol_type type, EFI_HANDLE handle);

/* Returns 1 if current volume is read-only (NTFS), 0 otherwise */
int fs_is_read_only(void);

/* Query current volume type */
enum fs_vol_type fs_get_vol_type(void);

/* Enumerate exFAT/NTFS volumes on all BlockIO handles.
   Returns count found (up to max). */
int fs_enumerate_custom_volumes(struct fs_custom_volume *vols, int max);

/* Check if a handle's block device has a valid FAT32 boot sector */
int fs_has_valid_fat32(EFI_HANDLE handle);
```

### Custom Volume State in fs.c

The dispatch layer needs to track which backend is active. At the top of `fs.c`, alongside the existing `s_root` and `s_boot_root` variables, we add:

```c
/* Custom volume state */
static enum fs_vol_type s_vol_type = FS_VOL_SFS;
static struct exfat_vol *s_exfat = NULL;
static struct ntfs_vol *s_ntfs = NULL;
static EFI_HANDLE s_custom_handle = NULL;  /* BlockIO handle for custom vol */
```

The `s_vol_type` variable controls dispatch. When it is `FS_VOL_SFS` (the default), all filesystem calls go through UEFI. When it is `FS_VOL_EXFAT`, they go to the exFAT driver. When `FS_VOL_NTFS`, to the NTFS driver. The `s_exfat` and `s_ntfs` pointers hold the mounted volume handles returned by the respective drivers. Only one custom volume can be active at a time.

### BlockIO Callback Wrappers

The exFAT and NTFS drivers are designed to be portable -- they do not call UEFI protocols directly. Instead, they accept callback functions for reading and writing blocks. This separation means the drivers could theoretically run on Linux, on a microcontroller, anywhere that can provide block I/O.

In `fs.c`, we bridge from UEFI's BlockIO protocol to the callback interface:

```c
struct bio_ctx {
    EFI_BLOCK_IO *bio;
    UINT32 media_id;
};

static struct bio_ctx s_bio_ctx;

static int bio_read_cb(void *ctx, UINT64 lba, UINT32 count, void *buf) {
    struct bio_ctx *bc = (struct bio_ctx *)ctx;
    UINTN size = (UINTN)count * (UINTN)bc->bio->Media->BlockSize;
    EFI_STATUS st = bc->bio->ReadBlocks(bc->bio, bc->media_id,
                                         (EFI_LBA)lba, size, buf);
    return EFI_ERROR(st) ? -1 : 0;
}

static int bio_write_cb(void *ctx, UINT64 lba, UINT32 count, const void *buf) {
    struct bio_ctx *bc = (struct bio_ctx *)ctx;
    UINTN size = (UINTN)count * (UINTN)bc->bio->Media->BlockSize;
    EFI_STATUS st = bc->bio->WriteBlocks(bc->bio, bc->media_id,
                                          (EFI_LBA)lba, size, (void *)buf);
    if (EFI_ERROR(st)) return -1;
    bc->bio->FlushBlocks(bc->bio);
    return 0;
}
```

The `bio_ctx` structure captures the UEFI BlockIO protocol pointer and the media ID (which the protocol requires for every call). The read callback translates from the driver's "give me N blocks at LBA X" request into a UEFI `ReadBlocks` call. The write callback does the same for writes, and adds a `FlushBlocks` call to ensure data reaches the physical media. Both callbacks return 0 on success, -1 on error -- a simple C convention rather than UEFI's `EFI_STATUS`.

The write callback casts away `const` on the buffer pointer. UEFI's `WriteBlocks` takes a `void *` rather than `const void *`, a minor API infelicity. The data is not modified; the cast is safe.

### Path Conversion

UEFI uses UTF-16 (CHAR16) paths with backslash separators: `L"\\EFI\\BOOT\\hello.txt"`. Our exFAT driver uses ASCII paths with forward-slash separators: `"/EFI/BOOT/hello.txt"`. A conversion helper bridges the two:

```c
/* Convert CHAR16 path to ASCII path with '/' separators */
static void path_to_ascii(const CHAR16 *src, char *dst, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        char c = (char)(src[i] & 0x7F);
        dst[i] = (c == '\\') ? '/' : c;
        i++;
    }
    dst[i] = '\0';
}
```

This is lossy -- non-ASCII characters become garbage -- but our entire UI is ASCII, so this is fine. The backslash-to-forward-slash conversion matters because exFAT stores filenames in UTF-16 but our driver normalizes paths to ASCII with forward slashes, matching Unix conventions.

### Unmounting

Before mounting a new custom volume, any existing one must be torn down:

```c
static void unmount_custom(void) {
    if (s_exfat) {
        exfat_unmount(s_exfat);
        s_exfat = NULL;
    }
    if (s_ntfs) {
        ntfs_unmount(s_ntfs);
        s_ntfs = NULL;
    }
    s_vol_type = FS_VOL_SFS;
    s_custom_handle = NULL;
}
```

This function is called both when mounting a new custom volume (to clean up any previous one) and when returning to the boot volume. It flushes all dirty data (the unmount functions handle that internally), frees allocated memory, and resets the dispatch state to `FS_VOL_SFS`.

### The Dispatch Pattern

Every existing `fs_*` function gains a dispatch preamble. Here is `fs_readdir` as the canonical example:

```c
int fs_readdir(const CHAR16 *path, struct fs_entry *entries, int max_entries) {
    /* Dispatch to custom driver */
    if (s_vol_type == FS_VOL_EXFAT && s_exfat) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return exfat_readdir(s_exfat, apath, entries, max_entries);
    }
    if (s_vol_type == FS_VOL_NTFS && s_ntfs) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return ntfs_readdir(s_ntfs, apath, entries, max_entries);
    }

    /* SFS path — original UEFI implementation follows */
    ...
}
```

The pattern is the same everywhere: check `s_vol_type`, convert the path, call the custom driver, return. If neither custom backend is active, fall through to the original UEFI code. This pattern appears in `fs_readfile`, `fs_writefile`, `fs_volume_info`, `fs_file_size`, `fs_exists`, `fs_rename`, and `fs_mkdir`.

For write operations on read-only volumes, the dispatch is even simpler -- just refuse:

```c
EFI_STATUS fs_writefile(const CHAR16 *path, const void *data, UINTN size) {
    if (s_vol_type == FS_VOL_NTFS)
        return EFI_WRITE_PROTECTED;
    if (s_vol_type == FS_VOL_EXFAT && s_exfat) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return exfat_writefile(s_exfat, apath, data, size) == 0
               ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
    /* SFS path follows... */
}
```

NTFS is always read-only (we do not implement NTFS write support). exFAT is fully read-write. The dispatch converts between the exFAT driver's int return convention (0 = success) and UEFI's `EFI_STATUS`.

### Mounting a Custom Volume

When the user selects an exFAT or NTFS entry in the file browser, the browser calls `fs_set_custom_volume`:

```c
int fs_set_custom_volume(enum fs_vol_type type, EFI_HANDLE handle) {
    /* Unmount any previous custom volume */
    unmount_custom();

    /* Get BlockIO from the handle */
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO *bio = NULL;
    EFI_STATUS st = g_boot.bs->HandleProtocol(
        handle, &bio_guid, (void **)&bio);
    if (EFI_ERROR(st) || !bio || !bio->Media)
        return -1;

    /* Set up BlockIO context for callbacks */
    s_bio_ctx.bio = bio;
    s_bio_ctx.media_id = bio->Media->MediaId;
    UINT32 block_size = bio->Media->BlockSize;

    if (type == FS_VOL_EXFAT) {
        s_exfat = exfat_mount(bio_read_cb, bio_write_cb,
                               &s_bio_ctx, block_size);
        if (!s_exfat) return -1;
        s_vol_type = FS_VOL_EXFAT;
    } else if (type == FS_VOL_NTFS) {
        s_ntfs = ntfs_mount(bio_read_cb, &s_bio_ctx, block_size);
        if (!s_ntfs) return -1;
        s_vol_type = FS_VOL_NTFS;
    } else {
        return -1;
    }

    s_custom_handle = handle;
    return 0;
}
```

The function first tears down any existing custom mount. It then obtains the BlockIO protocol from the given UEFI handle -- this is the raw block device interface that lets us read and write sectors. The `s_bio_ctx` static is populated with the protocol pointer and media ID, then passed as the context pointer to the driver's mount function.

Note that `exfat_mount` receives both read and write callbacks, while `ntfs_mount` only gets a read callback. NTFS is read-only by design.

### Query Functions

Two small functions let the rest of the codebase query the current volume state:

```c
int fs_is_read_only(void) {
    return (s_vol_type == FS_VOL_NTFS) ? 1 : 0;
}

enum fs_vol_type fs_get_vol_type(void) {
    return s_vol_type;
}
```

The browser uses `fs_is_read_only()` to disable write operations (F4:New, F8:Paste, F9:Rename) and show appropriate status messages. The editor uses it to block F2:Save and display a `[READ-ONLY]` indicator. `fs_get_vol_type()` is used by `draw_path()` to show the correct `[exFAT]` or `[NTFS]` tag.

### FAT32 Validation

When enumerating volumes, we need to distinguish between handles that have a real FAT32 filesystem (handled by UEFI's SFS) and handles where the SFS protocol is stale (because we overwrote the filesystem with an ISO image or format operation). This helper reads sector 0 and checks for FAT32 signatures:

```c
int fs_has_valid_fat32(EFI_HANDLE handle) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO *bio = NULL;
    EFI_STATUS st = g_boot.bs->HandleProtocol(
        handle, &bio_guid, (VOID **)&bio);
    if (EFI_ERROR(st) || !bio || !bio->Media)
        return 0;

    UINT32 bs = bio->Media->BlockSize;
    if (bs < 512) return 0;

    unsigned char *sec = mem_alloc(bs);
    if (!sec) return 0;

    st = bio->ReadBlocks(bio, bio->Media->MediaId, 0, bs, sec);
    if (EFI_ERROR(st)) {
        mem_free(sec);
        return 0;
    }

    /* Check boot signature 0x55AA at offset 510 */
    int valid = (sec[510] == 0x55 && sec[511] == 0xAA);

    /* Check FAT32 filesystem type string at offset 82 */
    if (valid) {
        valid = (sec[82] == 'F' && sec[83] == 'A' && sec[84] == 'T'
                 && sec[85] == '3' && sec[86] == '2');
    }

    mem_free(sec);
    return valid;
}
```

The check is simple: read the boot sector, verify the 0xAA55 boot signature at offset 510, and look for the "FAT32" string at offset 82 in the BPB. Both conditions must be true. A drive that was FAT32 but got overwritten with an ISO image will fail this check -- the boot signature or filesystem type string will be gone. This prevents the browser from showing a stale `[USB]` entry for a drive that no longer has a valid FAT32 filesystem.

### Enumerating Custom Volumes

The most complex function in the volume abstraction scans all BlockIO handles for exFAT and NTFS signatures:

```c
int fs_enumerate_custom_volumes(struct fs_custom_volume *vols, int max) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_STATUS status;
    UINTN handle_count = 0;
    EFI_HANDLE *handles = NULL;
    int count = 0;

    status = g_boot.bs->LocateHandleBuffer(
        ByProtocol, &bio_guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || !handles)
        return 0;

    for (UINTN i = 0; i < handle_count && count < max; i++) {
        /* Skip boot device */
        if (handles[i] == s_boot_device)
            continue;

        /* Skip handles that have SFS with valid FAT32 */
        void *sfs = NULL;
        status = g_boot.bs->HandleProtocol(
            handles[i], &sfs_guid, &sfs);
        if (!EFI_ERROR(status) && sfs && fs_has_valid_fat32(handles[i]))
            continue;

        /* Get BlockIO */
        EFI_BLOCK_IO *bio = NULL;
        status = g_boot.bs->HandleProtocol(
            handles[i], &bio_guid, (void **)&bio);
        if (EFI_ERROR(status) || !bio || !bio->Media)
            continue;
        if (!bio->Media->MediaPresent)
            continue;

        /* Read sector 0 to check filesystem signature */
        UINT32 bs = bio->Media->BlockSize;
        if (bs < 512) continue;

        unsigned char *sec = mem_alloc(bs);
        if (!sec) continue;

        status = bio->ReadBlocks(bio, bio->Media->MediaId, 0, bs, sec);
        if (EFI_ERROR(status)) {
            mem_free(sec);
            continue;
        }

        enum fs_vol_type type;
        int found = 0;

        /* Check for exFAT: "EXFAT   " at offset 3 */
        if (sec[3] == 'E' && sec[4] == 'X' && sec[5] == 'F' &&
            sec[6] == 'A' && sec[7] == 'T' && sec[8] == ' ' &&
            sec[9] == ' ' && sec[10] == ' ') {
            type = FS_VOL_EXFAT;
            found = 1;
        }
        /* Check for NTFS: "NTFS    " at offset 3 */
        else if (sec[3] == 'N' && sec[4] == 'T' && sec[5] == 'F' &&
                 sec[6] == 'S' && sec[7] == ' ' && sec[8] == ' ' &&
                 sec[9] == ' ' && sec[10] == ' ') {
            type = FS_VOL_NTFS;
            found = 1;
        }

        mem_free(sec);

        if (!found)
            continue;

        struct fs_custom_volume *v = &vols[count];
        v->handle = handles[i];
        v->type = type;
        v->size_bytes = (UINT64)(bio->Media->LastBlock + 1) *
                        (UINT64)bio->Media->BlockSize;

        /* Try to get label by temporarily mounting */
        int pos = 0;
        const char *type_name = (type == FS_VOL_EXFAT) ? "exFAT" : "NTFS";
        while (*type_name && pos < 30)
            v->label[pos++] = *type_name++;

        /* Attempt quick mount to get real label */
        struct bio_ctx tmp_ctx;
        tmp_ctx.bio = bio;
        tmp_ctx.media_id = bio->Media->MediaId;

        if (type == FS_VOL_EXFAT) {
            struct exfat_vol *ev = exfat_mount(bio_read_cb, bio_write_cb,
                                                &tmp_ctx, bs);
            if (ev) {
                const char *lbl = exfat_get_label(ev);
                if (lbl && lbl[0]) {
                    pos = 0;
                    while (*lbl && pos < 30)
                        v->label[pos++] = *lbl++;
                }
                exfat_unmount(ev);
            }
        } else {
            struct ntfs_vol *nv = ntfs_mount(bio_read_cb, &tmp_ctx, bs);
            if (nv) {
                const char *lbl = ntfs_get_label(nv);
                if (lbl && lbl[0]) {
                    pos = 0;
                    while (*lbl && pos < 30)
                        v->label[pos++] = *lbl++;
                }
                ntfs_unmount(nv);
            }
        }

        v->label[pos] = '\0';
        fs_format_label_size(v->size_bytes, v->label, &pos);
        count++;
    }

    g_boot.bs->FreePool(handles);
    return count;
}
```

This function is called every time the file browser refreshes the root directory. It uses `LocateHandleBuffer` to find every handle that supports the BlockIO protocol -- this includes hard drives, USB sticks, SD cards, and partitions. For each handle, it applies several filters:

First, it skips the boot device. You do not want your boot SD card showing up as a mountable exFAT volume. Second, it skips handles that have a valid FAT32 SimpleFileSystem -- those are already handled by the `[USB]` entries. Third, it checks that media is actually present (for card readers that may be empty). Fourth, it reads sector 0 and checks byte offsets 3-10 for the `"EXFAT   "` or `"NTFS    "` signature string.

The label discovery is interesting. The function temporarily mounts the volume just to read its label, then immediately unmounts it. This is not elegant, but it works. The alternative would be to parse the boot sector for the label directly, but exFAT stores its volume label in a directory entry in the root directory, not in the boot sector. A full mount-read-unmount cycle is the simplest way to get it.

After getting the label (or falling back to "exFAT"/"NTFS" as a default), `fs_format_label_size` appends the volume size in a human-readable format like "(64 GB)". The result is a descriptor like `{handle: 0x..., type: FS_VOL_EXFAT, label: "SANDISK (64 GB)", size_bytes: 64424509440}`.

## The exFAT Header

The public interface for the exFAT driver is declared in `exfat.h`:

```c
/*
 * exfat.h — exFAT filesystem driver (read/write)
 *
 * Portable: uses callback-based block I/O, no UEFI dependency.
 */
#ifndef EXFAT_H
#define EXFAT_H

#include "boot.h"
#include "fs.h"

/* Block I/O callbacks */
typedef int (*exfat_block_read_fn)(void *ctx, UINT64 lba, UINT32 count, void *buf);
typedef int (*exfat_block_write_fn)(void *ctx, UINT64 lba, UINT32 count, const void *buf);

/* Opaque volume handle */
struct exfat_vol;

/* Mount an exFAT volume. Returns NULL on error.
   block_size is the underlying device block size (typically 512). */
struct exfat_vol *exfat_mount(exfat_block_read_fn read_fn,
                               exfat_block_write_fn write_fn,
                               void *ctx, UINT32 block_size);

/* Unmount and free all resources */
void exfat_unmount(struct exfat_vol *vol);

/* Read directory contents. path is ASCII with '/' separators.
   "/" for root. Returns entry count, or -1 on error. */
int exfat_readdir(struct exfat_vol *vol, const char *path,
                  struct fs_entry *entries, int max_entries);

/* Read entire file into newly allocated buffer. Returns NULL on error. */
void *exfat_readfile(struct exfat_vol *vol, const char *path, UINTN *out_size);

/* Write data to a file (create or replace). Returns 0 on success. */
int exfat_writefile(struct exfat_vol *vol, const char *path,
                    const void *data, UINTN size);

/* Create a directory. Returns 0 on success. */
int exfat_mkdir(struct exfat_vol *vol, const char *path);

/* Rename a file/dir. new_name is just the filename. Returns 0 on success. */
int exfat_rename(struct exfat_vol *vol, const char *path, const char *new_name);

/* Delete a file. Returns 0 on success. */
int exfat_delete(struct exfat_vol *vol, const char *path);

/* Get volume info. Returns 0 on success. */
int exfat_volume_info(struct exfat_vol *vol, UINT64 *total_bytes, UINT64 *free_bytes);

/* Get file size. Returns 0 if not found. */
UINT64 exfat_file_size(struct exfat_vol *vol, const char *path);

/* Check if path exists. Returns 1 if yes. */
int exfat_exists(struct exfat_vol *vol, const char *path);

/* Get volume label (ASCII). Returns empty string if none. */
const char *exfat_get_label(struct exfat_vol *vol);

#endif /* EXFAT_H */
```

Two callback typedefs define the block I/O interface. Both take an opaque context pointer (so the driver does not need to know about UEFI), an LBA (logical block address), a block count, and a buffer. The read callback fills the buffer; the write callback sends buffer contents to disk. Both return 0 on success.

The `struct exfat_vol` is declared but not defined -- it is an opaque handle. Callers get a pointer from `exfat_mount` and pass it to every subsequent call. They never access its fields directly. This encapsulation means the internal data structures can change without affecting the public API.

The twelve public functions cover the complete lifecycle: mount and unmount for setup and teardown; readdir and readfile for reading; writefile, mkdir, rename, and delete for writing; volume_info, file_size, exists, and get_label for metadata queries. Every function takes the volume handle as its first argument. Paths are ASCII with forward-slash separators.

The API deliberately mirrors the `fs_*` functions in `fs.h`. This makes the dispatch layer in `fs.c` trivial -- each `fs_*` function just converts the path and calls the corresponding `exfat_*` function.

## exFAT On Disk: Structures

The exFAT driver begins with the on-disk format. Like all filesystem drivers, we must understand the exact byte layout before we can write a single line of logic. exFAT uses a clean, modern design -- no legacy BPB fields from the DOS era, no ambiguous field overloading. Everything is in a well-defined binary format.

All on-disk structures use `#pragma pack(1)` to prevent the compiler from inserting alignment padding. Without this, a struct with a `UINT8` followed by a `UINT32` would have 3 bytes of padding on ARM64, and our `mem_copy` from disk data would read garbage. The `#pragma pack()` at the end restores normal alignment for runtime structures.

### Constants

```c
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
```

The FAT constants should be familiar from Chapter 24. `EXFAT_EOC` (End Of Chain) marks the last cluster in a chain. `EXFAT_BAD` marks a bad cluster. `EXFAT_FREE` means unallocated. The cache size of 8 sectors is a compromise between memory usage and performance -- each cache entry holds one sector (typically 512 bytes), so the total cache is 4 KB.

Directory entry type codes have a specific structure. Bit 7 (0x80) is the "InUse" flag. A type of 0x05 means the same entry as 0x85 but marked as deleted. This is how exFAT deletes entries without physically removing them -- just clear bit 7. The remaining bits identify the entry type: 0x01 for bitmap, 0x02 for upcase table, 0x03 for volume label, 0x05 for file, 0x40 for stream extension, 0x41 for file name extension. With the InUse bit set, these become the 0x81, 0x82, 0x83, 0x85, 0xC0, and 0xC1 values we define.

The stream extension flags are important for performance. `STREAM_NO_FAT_CHAIN` (bit 1) indicates that a file's clusters are contiguous on disk. When this flag is set, the driver does not need to follow the FAT chain -- it just reads sequential clusters starting from the first one. This is a significant optimization for large files.

### Boot Sector

```c
#pragma pack(1)

struct exfat_boot_sector {
    UINT8  jump_boot[3];         /* 0:   EB 76 90 */
    UINT8  fs_name[8];           /* 3:   "EXFAT   " */
    UINT8  must_be_zero[53];     /* 11:  zeros */
    UINT64 partition_offset;     /* 64:  sectors */
    UINT64 volume_length;        /* 72:  sectors */
    UINT32 fat_offset;           /* 80:  sectors from vol start */
    UINT32 fat_length;           /* 84:  sectors */
    UINT32 cluster_heap_offset;  /* 88:  sectors from vol start */
    UINT32 cluster_count;        /* 92:  total data clusters */
    UINT32 root_cluster;         /* 96:  first cluster of root dir */
    UINT32 volume_serial;        /* 100: serial number */
    UINT16 fs_revision;          /* 104: expect 0x0100 */
    UINT16 volume_flags;         /* 106: */
    UINT8  bytes_per_sector_shift;    /* 108: log2 */
    UINT8  sectors_per_cluster_shift; /* 109: log2 */
    UINT8  number_of_fats;       /* 110: usually 1 */
    UINT8  drive_select;         /* 111: */
    UINT8  percent_in_use;       /* 112: */
    UINT8  reserved[7];          /* 113: */
    UINT8  boot_code[390];       /* 120: */
    UINT16 boot_signature;       /* 510: 0xAA55 */
};
```

The boot sector is the first 512 bytes of the volume. Unlike FAT32's BPB (BIOS Parameter Block), which carried forward decades of DOS compatibility fields, exFAT's boot sector is clean. The `must_be_zero` region at offset 11 occupies the space where FAT32 would have its BPB -- exFAT explicitly zeros it out so that FAT drivers do not mistakenly try to mount an exFAT volume.

The `fs_name` field at offset 3 contains the ASCII string "EXFAT   " (padded with spaces). This is the signature we check during volume enumeration in `fs_enumerate_custom_volumes`.

Sector and cluster sizes are stored as power-of-two shifts rather than direct values. A `bytes_per_sector_shift` of 9 means 512-byte sectors (2^9 = 512). A `sectors_per_cluster_shift` of 3 means 8 sectors per cluster (2^3 = 8), giving 4 KB clusters. This encoding is compact and prevents invalid non-power-of-two values.

The `fat_offset` and `cluster_heap_offset` tell us where the FAT and data regions begin, in sectors from the start of the volume. The `root_cluster` is the first cluster of the root directory. The `cluster_count` is the total number of data clusters (starting from cluster 2, as in FAT).

### Directory Entries

exFAT directories consist of 32-byte entries, just like FAT. But unlike FAT's single-entry-per-file design, exFAT uses "entry sets" -- a group of related entries that together describe one file. A file is described by a File Directory Entry (0x85), followed by a Stream Extension Entry (0xC0), followed by one or more File Name Extension Entries (0xC1).

The generic entry structure lets us peek at the type byte:

```c
/* Generic 32-byte directory entry (for reading entry type) */
struct exfat_dentry {
    UINT8  type;
    UINT8  data[31];
};
```

The File Directory Entry carries metadata about the file:

```c
/* File Directory Entry (type 0x85) */
struct exfat_file_dentry {
    UINT8  type;                 /* 0x85 */
    UINT8  secondary_count;      /* count of stream + name entries */
    UINT16 set_checksum;
    UINT16 file_attributes;
    UINT16 reserved1;
    UINT32 create_timestamp;
    UINT32 modify_timestamp;
    UINT32 access_timestamp;
    UINT8  create_10ms;
    UINT8  modify_10ms;
    UINT8  create_tz_offset;
    UINT8  modify_tz_offset;
    UINT8  access_tz_offset;
    UINT8  reserved2[7];
};
```

The `secondary_count` tells us how many entries follow this one in the set. For a file named "hello.txt" (9 characters), this would be 2: one stream entry plus one name entry (which holds up to 15 characters). A file with a 30-character name would have `secondary_count` of 3: one stream plus two name entries.

The `set_checksum` is a 16-bit checksum over the entire entry set. It must be recomputed whenever any entry in the set changes (including rename). The file attributes use the same bit definitions as FAT: 0x10 for directory, 0x20 for archive, 0x01 for read-only.

Timestamps use a packed format: bits 31-25 encode the year (offset from 1980), bits 24-21 the month, bits 20-16 the day, bits 15-11 the hour, bits 10-5 the minute, and bits 4-0 the seconds divided by 2. The `create_10ms` and `modify_10ms` fields add 10-millisecond precision. The timezone offset fields encode UTC offset in 15-minute increments. Our driver does not use real timestamps -- we write a fixed date of January 1, 2026 for all new files, since we have no real-time clock.

The Stream Extension Entry holds the file's location and size:

```c
/* Stream Extension Entry (type 0xC0) */
struct exfat_stream_dentry {
    UINT8  type;                 /* 0xC0 */
    UINT8  flags;                /* bit 0: alloc possible, bit 1: no FAT chain */
    UINT8  reserved1;
    UINT8  name_length;          /* in Unicode chars */
    UINT16 name_hash;
    UINT16 reserved2;
    UINT64 valid_data_length;
    UINT32 reserved3;
    UINT32 first_cluster;
    UINT64 data_length;
};
```

The `flags` field is critical. Bit 0 (`STREAM_ALLOC_POSSIBLE`) indicates that the stream has an allocation (i.e., it occupies clusters). Bit 1 (`STREAM_NO_FAT_CHAIN`) is the contiguous allocation flag mentioned earlier. When set, the file's clusters are sequential starting from `first_cluster`, and the FAT does not need to be consulted.

The `name_hash` is a 16-bit hash of the up-cased filename. It allows fast filename comparison during directory searches -- if the hash does not match, the full name comparison can be skipped. The `name_length` is the total number of UTF-16 characters in the filename.

Two size fields exist: `data_length` is the allocated size, and `valid_data_length` is how much of that allocation contains valid data. For most files they are equal. A sparse or zero-filled file might have `valid_data_length` less than `data_length`.

File Name Extension Entries carry the actual filename in UTF-16:

```c
/* File Name Extension Entry (type 0xC1) */
struct exfat_name_dentry {
    UINT8  type;                 /* 0xC1 */
    UINT8  flags;
    UINT16 name[15];             /* UTF-16LE, 15 chars per entry */
};
```

Each name entry holds up to 15 UTF-16 characters. A filename of 30 characters needs two name entries. The maximum exFAT filename is 255 characters, requiring 17 name entries. That is a maximum entry set of 19 entries (1 file + 1 stream + 17 name) = 608 bytes.

The Allocation Bitmap Entry tells us where the cluster bitmap is stored:

```c
/* Allocation Bitmap Entry (type 0x81) */
struct exfat_bitmap_dentry {
    UINT8  type;                 /* 0x81 */
    UINT8  bitmap_flags;         /* bit 0: 0=first bitmap, 1=second */
    UINT8  reserved[18];
    UINT32 first_cluster;
    UINT64 data_length;
};
```

exFAT stores the allocation bitmap as a file in the root directory, pointed to by this entry. Bit 0 of `bitmap_flags` distinguishes the first bitmap from the optional second (for TexFAT, which we do not support). The `data_length` tells us how many bytes the bitmap occupies.

The Volume Label Entry stores the volume name:

```c
/* Volume Label Entry (type 0x83) */
struct exfat_label_dentry {
    UINT8  type;                 /* 0x83 */
    UINT8  char_count;           /* number of UTF-16 chars (max 11) */
    UINT16 label[11];            /* UTF-16LE volume label */
    UINT8  reserved[8];
};

#pragma pack()
```

The label is stored inline in the directory entry -- up to 11 UTF-16 characters, enough for "SANDISK USB" or similar. The `char_count` field tells us how many of the 11 characters are actually part of the label.

### Runtime Structures

After the packed on-disk structures, we define unpacked runtime structures for the sector cache and volume handle:

```c
struct exfat_cache_entry {
    UINT64 sector;
    UINT8  *data;
    int    valid;
    int    dirty;
};
```

Each cache entry tracks which sector it holds, a pointer to the sector data buffer, whether the entry is valid (contains real data), and whether it is dirty (modified but not yet written back). Eight of these entries form the sector cache.

```c
struct exfat_vol {
    exfat_block_read_fn  read_fn;
    exfat_block_write_fn write_fn;
    void                *ctx;

    UINT32 dev_block_size;
    UINT32 bytes_per_sector;
    UINT32 sectors_per_cluster;
    UINT32 cluster_count;
    UINT32 fat_offset;
    UINT32 fat_length;
    UINT32 cluster_heap_offset;
    UINT32 root_cluster;
    UINT64 volume_length;

    /* Allocation bitmap */
    UINT8  *bitmap;
    UINT32 bitmap_size;          /* in bytes */
    UINT32 bitmap_cluster;
    int    bitmap_no_fat_chain;

    /* Volume label (ASCII) */
    char   label[48];

    /* Sector cache */
    struct exfat_cache_entry cache[EXFAT_CACHE_SIZE];
    UINT32 cache_clock;          /* simple clock for LRU eviction */
};
```

This is the complete state for a mounted exFAT volume. The first three fields are the I/O callbacks and context pointer, passed in by the caller. The next group of fields is populated from the boot sector during mount. The bitmap section holds the in-memory copy of the cluster allocation bitmap -- loaded entirely into RAM at mount time. The label is stored as ASCII for convenience.

The `dev_block_size` deserves explanation. The underlying storage device (e.g., a USB stick) might have a different block size than exFAT's logical sector size. A USB device might report 512-byte blocks, while exFAT might use 4096-byte sectors (bytes_per_sector_shift=12). The cache and I/O functions must translate between exFAT sector numbers and device block numbers using the ratio `bytes_per_sector / dev_block_size`.

## Sector Cache

Reading raw sectors from a USB device is slow -- each `ReadBlocks` call is a USB transaction. For directory traversal, where we read many small 32-byte entries from the same sector, reading the sector once and accessing it multiple times is essential. The sector cache provides this.

### Initialization and Teardown

```c
static void cache_init(struct exfat_vol *vol)
{
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        vol->cache[i].data = (UINT8 *)mem_alloc(vol->bytes_per_sector);
        vol->cache[i].valid = 0;
        vol->cache[i].dirty = 0;
        vol->cache[i].sector = 0;
    }
    vol->cache_clock = 0;
}
```

At initialization, we allocate a data buffer for each of the 8 cache slots. Each buffer is one exFAT sector (typically 512 bytes, could be up to 4096). All slots start as invalid. The clock counter starts at 0 -- it is used for round-robin eviction.

```c
static void cache_free(struct exfat_vol *vol)
{
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        if (vol->cache[i].data) {
            mem_free(vol->cache[i].data);
            vol->cache[i].data = 0;
        }
    }
}
```

Teardown frees all data buffers. This is called during `exfat_unmount` after all dirty data has been flushed.

### Flushing Dirty Entries

```c
static int cache_flush_entry(struct exfat_vol *vol, int idx)
{
    struct exfat_cache_entry *ce = &vol->cache[idx];
    if (!ce->valid || !ce->dirty)
        return 0;

    UINT32 ratio = vol->bytes_per_sector / vol->dev_block_size;
    UINT64 dev_lba = ce->sector * ratio;

    if (vol->write_fn(vol->ctx, dev_lba, ratio, ce->data) != 0)
        return -1;

    ce->dirty = 0;
    return 0;
}
```

Flushing a cache entry writes its data back to the underlying device. The key calculation is the sector-to-block translation: if exFAT sectors are 4096 bytes and device blocks are 512 bytes, the ratio is 8, and each exFAT sector occupies 8 device blocks. We multiply the exFAT sector number by this ratio to get the device LBA, and write `ratio` blocks.

After a successful write, the dirty flag is cleared but the entry remains valid -- the data is still in cache for future reads.

```c
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
```

Flushing all entries is called after any write operation to ensure data reaches the disk. It continues even if one entry fails, reporting the overall failure status. This is important for data integrity -- we want to write as much as possible, even if one sector has an error.

### Finding a Cache Slot

```c
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
```

When we need to load a new sector into the cache, we first look for an empty (invalid) slot. If all slots are occupied, we evict using a simple clock algorithm -- round-robin through the slots. If the evicted slot is dirty, it gets flushed first.

This is not LRU (Least Recently Used) -- it is simpler. A true LRU cache would track access timestamps and evict the least recently accessed entry. For 8 slots, the difference is negligible, and the clock algorithm is trivial to implement with no extra bookkeeping.

### Reading Through the Cache

```c
static UINT8 *cache_read(struct exfat_vol *vol, UINT64 sector)
{
    /* Check if already cached */
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        if (vol->cache[i].valid && vol->cache[i].sector == sector)
            return vol->cache[i].data;
    }

    /* Need to read from disk */
    int idx = cache_find_slot(vol);
    struct exfat_cache_entry *ce = &vol->cache[idx];

    UINT32 ratio = vol->bytes_per_sector / vol->dev_block_size;
    UINT64 dev_lba = sector * ratio;

    if (vol->read_fn(vol->ctx, dev_lba, ratio, ce->data) != 0)
        return 0;

    ce->sector = sector;
    ce->valid = 1;
    ce->dirty = 0;
    return ce->data;
}
```

This is the most-called function in the entire driver. It returns a pointer to the cached contents of a given sector. If the sector is already in cache, it returns immediately. If not, it finds a slot (potentially evicting another sector), reads the data from disk, and returns the buffer pointer.

The returned pointer is valid until the next cache operation that might evict this slot. Callers must use the data immediately or copy it out. In practice, our code always processes the sector data before making another cache call, so this is safe.

### Marking Dirty and Invalidating

```c
static void cache_mark_dirty(struct exfat_vol *vol, UINT64 sector)
{
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        if (vol->cache[i].valid && vol->cache[i].sector == sector) {
            vol->cache[i].dirty = 1;
            return;
        }
    }
}
```

After modifying a cached sector's data (through the pointer returned by `cache_read`), the caller marks it dirty. The next flush will write it back. This is a write-back cache -- writes go to memory first and reach disk later. The alternative (write-through) would write immediately, but that would double the I/O for operations like building directory entry sets that modify multiple entries in the same sector.

```c
static void cache_invalidate(struct exfat_vol *vol, UINT64 sector)
{
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++) {
        if (vol->cache[i].valid && vol->cache[i].sector == sector) {
            if (vol->cache[i].dirty)
                cache_flush_entry(vol, i);
            vol->cache[i].valid = 0;
        }
    }
}
```

Invalidation removes a sector from the cache. If the sector is dirty, it gets flushed first -- we never discard modified data. This is used when we know the on-disk contents have changed outside the cache (e.g., after a raw sector write).

```c
static void cache_invalidate_all(struct exfat_vol *vol)
{
    cache_flush_all(vol);
    for (int i = 0; i < EXFAT_CACHE_SIZE; i++)
        vol->cache[i].valid = 0;
}
```

A full invalidation flushes everything and marks all slots invalid. This is a nuclear option, used when we need to ensure the cache is completely fresh.

### Raw Sector I/O

For bulk data transfers (reading or writing entire files), the cache is counterproductive -- we would thrash it by loading sector after sector, evicting useful cached data. Instead, we provide raw I/O functions that bypass the cache:

```c
static int read_sectors_raw(struct exfat_vol *vol, UINT64 exfat_sector,
                            UINT32 count, void *buf)
{
    UINT32 ratio = vol->bytes_per_sector / vol->dev_block_size;
    UINT64 dev_lba = exfat_sector * ratio;
    return vol->read_fn(vol->ctx, dev_lba, count * ratio, buf);
}

static int write_sectors_raw(struct exfat_vol *vol, UINT64 exfat_sector,
                             UINT32 count, const void *buf)
{
    UINT32 ratio = vol->bytes_per_sector / vol->dev_block_size;
    UINT64 dev_lba = exfat_sector * ratio;
    return vol->write_fn(vol->ctx, dev_lba, count * ratio, buf);
}
```

These functions perform the same sector-to-block translation as the cache functions, but read or write directly to the caller's buffer without touching the cache. They are used by `read_data`, `write_data`, `bitmap_load`, and `bitmap_flush` for multi-sector transfers where caching would be wasteful.

## Cluster Addressing and FAT

Two tiny functions convert between cluster numbers and sector addresses:

```c
static UINT64 cluster_to_sector(struct exfat_vol *vol, UINT32 cluster)
{
    return (UINT64)vol->cluster_heap_offset +
           (UINT64)(cluster - 2) * (UINT64)vol->sectors_per_cluster;
}

static UINT32 cluster_size(struct exfat_vol *vol)
{
    return vol->sectors_per_cluster * vol->bytes_per_sector;
}
```

Clusters are numbered starting from 2 (clusters 0 and 1 are reserved, a convention inherited from FAT). To find the first sector of cluster N, we subtract 2, multiply by sectors-per-cluster, and add the cluster heap offset. The cluster heap offset is where data clusters begin on the volume.

The `cluster_size` function returns the cluster size in bytes. For a volume with 512-byte sectors and 8 sectors per cluster, this is 4096 bytes. This value is used constantly throughout the driver for calculating how many clusters a file needs, how much data to read or write per cluster, and so on.

### FAT Operations

The File Allocation Table maps each cluster to the next cluster in its chain. Each entry is a 32-bit unsigned integer:

```c
static UINT32 fat_get(struct exfat_vol *vol, UINT32 cluster)
{
    if (cluster < 2 || cluster >= vol->cluster_count + 2)
        return EXFAT_EOC;

    UINT64 byte_off = (UINT64)cluster * 4;
    UINT64 sector = (UINT64)vol->fat_offset + byte_off / vol->bytes_per_sector;
    UINT32 offset = (UINT32)(byte_off % vol->bytes_per_sector);

    UINT8 *buf = cache_read(vol, sector);
    if (!buf)
        return EXFAT_EOC;

    UINT32 val;
    mem_copy(&val, buf + offset, 4);
    return val;
}
```

To read a FAT entry, we compute the byte offset (cluster number times 4 bytes per entry), find which sector of the FAT contains that offset, and read it through the cache. We use `mem_copy` instead of a direct pointer cast to avoid alignment issues on ARM64 -- the offset within the sector might not be 4-byte aligned, and an unaligned 32-bit read would cause a fault.

If the cluster number is out of range, we return `EXFAT_EOC` to prevent out-of-bounds reads. This is a safety measure -- a corrupted FAT entry pointing to cluster 0xDEADBEEF should not crash the driver.

```c
static int fat_set(struct exfat_vol *vol, UINT32 cluster, UINT32 value)
{
    if (cluster < 2 || cluster >= vol->cluster_count + 2)
        return -1;

    UINT64 byte_off = (UINT64)cluster * 4;
    UINT64 sector = (UINT64)vol->fat_offset + byte_off / vol->bytes_per_sector;
    UINT32 offset = (UINT32)(byte_off % vol->bytes_per_sector);

    UINT8 *buf = cache_read(vol, sector);
    if (!buf)
        return -1;

    mem_copy(buf + offset, &value, 4);
    cache_mark_dirty(vol, sector);
    return 0;
}
```

Writing a FAT entry is the reverse: read the sector through the cache, modify the 4 bytes at the computed offset, and mark the sector dirty. The write-back cache means this is fast -- multiple FAT updates in the same sector only cause one disk write when the cache is flushed.

## Allocation Bitmap

exFAT tracks cluster allocation with a bitmap -- one bit per cluster. Bit 0 of byte 0 corresponds to cluster 2 (the first data cluster), bit 1 to cluster 3, and so on. A set bit means the cluster is allocated; a clear bit means it is free.

Unlike FAT32, which uses the FAT itself for allocation tracking (a zero entry means free), exFAT has a dedicated bitmap. This is cleaner and faster -- checking whether a cluster is free is a single bit test instead of a 4-byte read.

### Reading and Writing Bits

```c
static int bitmap_get(struct exfat_vol *vol, UINT32 cluster)
{
    if (cluster < 2)
        return 0;
    UINT32 idx = cluster - 2;
    UINT32 byte_idx = idx / 8;
    UINT8  bit_idx  = (UINT8)(idx % 8);
    if (byte_idx >= vol->bitmap_size)
        return 0;
    return (vol->bitmap[byte_idx] >> bit_idx) & 1;
}
```

To check if a cluster is allocated, we subtract 2 from the cluster number (since the bitmap starts at cluster 2), divide by 8 to get the byte index, and mod by 8 to get the bit position within that byte. A right-shift and mask extracts the bit. The bounds check against `bitmap_size` prevents buffer overruns from corrupted cluster numbers.

```c
static void bitmap_set(struct exfat_vol *vol, UINT32 cluster, int used)
{
    if (cluster < 2)
        return;
    UINT32 idx = cluster - 2;
    UINT32 byte_idx = idx / 8;
    UINT8  bit_idx  = (UINT8)(idx % 8);
    if (byte_idx >= vol->bitmap_size)
        return;
    if (used)
        vol->bitmap[byte_idx] |= (UINT8)(1 << bit_idx);
    else
        vol->bitmap[byte_idx] &= (UINT8)~(1 << bit_idx);
}
```

Setting or clearing a bit uses the same index calculation. For setting, we OR in the bit. For clearing, we AND with the inverted bit. Both operations are on the in-memory bitmap -- the changes do not reach disk until `bitmap_flush` is called.

### Loading the Bitmap from Disk

```c
static int bitmap_load(struct exfat_vol *vol)
{
    UINT32 bps = vol->bytes_per_sector;
    UINT32 spc = vol->sectors_per_cluster;
    UINT32 clust_size = bps * spc;
    UINT32 total_bytes = vol->bitmap_size;
    UINT32 offset = 0;
    UINT32 cluster = vol->bitmap_cluster;

    while (offset < total_bytes && cluster >= 2 && cluster != EXFAT_EOC) {
        UINT64 sec = cluster_to_sector(vol, cluster);
        UINT32 chunk = total_bytes - offset;
        if (chunk > clust_size)
            chunk = clust_size;

        /* Read full sectors */
        UINT32 full_secs = chunk / bps;
        if (full_secs > 0) {
            if (read_sectors_raw(vol, sec, full_secs, vol->bitmap + offset) != 0)
                return -1;
        }
        /* Partial last sector */
        UINT32 partial = chunk % bps;
        if (partial > 0) {
            UINT8 *tmp = cache_read(vol, sec + full_secs);
            if (!tmp)
                return -1;
            mem_copy(vol->bitmap + offset + full_secs * bps, tmp, partial);
        }

        offset += clust_size;
        if (vol->bitmap_no_fat_chain)
            cluster++;
        else
            cluster = fat_get(vol, cluster);
    }

    return 0;
}
```

The bitmap is stored as a file on the volume, occupying one or more clusters. Loading it is essentially a file read: follow the cluster chain, read each cluster's data into the in-memory bitmap buffer. Full sectors are read with `read_sectors_raw` for efficiency. The last partial sector (if the bitmap does not end on a sector boundary) uses the cache.

The `bitmap_no_fat_chain` flag handles contiguous bitmaps. When set, clusters are sequential (cluster N+1 follows cluster N), so we just increment instead of consulting the FAT. Most exFAT volumes have contiguous bitmaps.

### Writing the Bitmap to Disk

```c
static int bitmap_flush(struct exfat_vol *vol)
{
    UINT32 bps = vol->bytes_per_sector;
    UINT32 spc = vol->sectors_per_cluster;
    UINT32 clust_size = bps * spc;
    UINT32 total_bytes = vol->bitmap_size;
    UINT32 offset = 0;
    UINT32 cluster = vol->bitmap_cluster;

    while (offset < total_bytes && cluster >= 2 && cluster != EXFAT_EOC) {
        UINT64 sec = cluster_to_sector(vol, cluster);
        UINT32 chunk = total_bytes - offset;
        if (chunk > clust_size)
            chunk = clust_size;

        /* Write full sectors */
        UINT32 full_secs = chunk / bps;
        if (full_secs > 0) {
            if (write_sectors_raw(vol, sec, full_secs, vol->bitmap + offset) != 0)
                return -1;
        }
        /* Partial last sector */
        UINT32 partial = chunk % bps;
        if (partial > 0) {
            UINT8 *tmp = cache_read(vol, sec + full_secs);
            if (!tmp)
                return -1;
            mem_copy(tmp, vol->bitmap + offset + full_secs * bps, partial);
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
```

Flushing is the mirror of loading: walk the cluster chain, write each chunk of the bitmap back to its cluster. Full sectors go through `write_sectors_raw`. Partial sectors use the cache (read-modify-write) to avoid corrupting the bytes after the bitmap data in the last sector.

The final `cache_flush_all` ensures that any cached partial-sector writes actually reach the device. Without this, a power loss after `bitmap_flush` could leave the partial sector in cache but not on disk, creating an inconsistency between the bitmap and the FAT.

### Allocation and Deallocation

```c
static UINT32 alloc_cluster(struct exfat_vol *vol)
{
    for (UINT32 i = 0; i < vol->cluster_count; i++) {
        UINT32 cl = i + 2;
        if (!bitmap_get(vol, cl)) {
            bitmap_set(vol, cl, 1);
            fat_set(vol, cl, EXFAT_EOC);
            return cl;
        }
    }
    return 0;
}
```

Allocating a cluster scans the bitmap for the first free bit. This is O(N) in the worst case, which is fine for our purposes -- we are not writing gigabytes of data. When a free cluster is found, we set its bitmap bit and mark it as end-of-chain in the FAT. Return 0 means allocation failed (disk full).

```c
static UINT32 alloc_cluster_chain(struct exfat_vol *vol, UINT32 prev)
{
    UINT32 cl = alloc_cluster(vol);
    if (cl == 0)
        return 0;
    if (prev >= 2)
        fat_set(vol, prev, cl);
    return cl;
}
```

This wrapper allocates a cluster and chains it to a previous cluster. The first call in a chain passes `prev=0` (no previous), and the new cluster's FAT entry is already set to `EXFAT_EOC` by `alloc_cluster`. Subsequent calls pass the previous cluster, and `fat_set` links them together: `FAT[prev] = new_cluster`.

```c
static void free_chain(struct exfat_vol *vol, UINT32 start, int no_fat_chain,
                       UINT64 data_length)
{
    UINT32 cluster = start;
    UINT32 clsz = cluster_size(vol);
    UINT64 remaining = data_length;

    while (cluster >= 2 && cluster != EXFAT_EOC && cluster != EXFAT_BAD) {
        bitmap_set(vol, cluster, 0);
        UINT32 next;
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
```

Freeing a chain walks from the start cluster, clearing each cluster's bitmap bit. For FAT-chained files, it also zeros the FAT entry. For contiguous files (`no_fat_chain` set), it follows sequential clusters and uses `data_length` to know when to stop.

The `data_length` parameter is essential for contiguous files. Without it, we would not know how many sequential clusters belong to the file -- there is no EOC marker in the FAT. The stream extension's `data_length` tells us exactly how many bytes (and therefore how many clusters) the file occupies.

```c
static int follow_chain(struct exfat_vol *vol, UINT32 start, int no_fat_chain,
                        UINT64 data_length, UINT32 *out, int max)
{
    UINT32 cluster = start;
    UINT32 clsz = cluster_size(vol);
    UINT64 remaining = data_length;
    int n = 0;

    while (cluster >= 2 && cluster != EXFAT_EOC && cluster != EXFAT_BAD
           && n < max && remaining > 0) {
        if (out)
            out[n] = cluster;
        n++;
        if (no_fat_chain) {
            if (remaining <= clsz)
                break;
            remaining -= clsz;
            cluster++;
        } else {
            cluster = fat_get(vol, cluster);
        }
    }
    return n;
}
```

This utility collects all cluster numbers in a chain into an array. It handles both FAT-chained and contiguous modes. The `max` parameter prevents buffer overflow. The function returns the count of clusters found. It is not currently used by the public API functions directly, but exists as a general-purpose chain-walking utility.

## UTF-16 and Checksums

exFAT stores filenames in UTF-16LE. Our workstation is ASCII-only. We need conversion functions in both directions, plus hash and checksum computations required by the exFAT specification.

### Case Conversion

```c
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
```

Case-insensitive comparison is needed because exFAT filenames are case-insensitive (like FAT and NTFS). The `to_lower` function handles ASCII letters only -- we do not support Unicode case folding. The `ascii_icmp` function compares two strings character by character after lowering, returning 0 for equality and a positive or negative value for ordering. The cast through `unsigned char` ensures proper comparison of characters above 127.

### String Conversion

```c
static void utf16_to_ascii(const UINT16 *src, int nchars, char *dst, int max)
{
    int i, j;
    for (i = 0, j = 0; i < nchars && j < max - 1; i++) {
        UINT16 ch;
        mem_copy(&ch, &src[i], 2);
        if (ch == 0)
            break;
        if (ch < 128)
            dst[j++] = (char)ch;
        else
            dst[j++] = '?';
    }
    dst[j] = '\0';
}
```

Converting from UTF-16 to ASCII is lossy -- any character outside the ASCII range (0-127) becomes a question mark. This is acceptable for our use case: most filenames on USB drives use ASCII characters. The `mem_copy` for each character avoids alignment issues on ARM64, where the UINT16 array from a directory entry might not be 2-byte aligned.

```c
static int ascii_to_utf16(const char *src, UINT16 *dst, int max_chars)
{
    int i;
    for (i = 0; i < max_chars - 1 && src[i]; i++) {
        UINT16 ch = (UINT16)(unsigned char)src[i];
        mem_copy(&dst[i], &ch, 2);
    }
    UINT16 zero = 0;
    mem_copy(&dst[i], &zero, 2);
    return i;
}
```

The reverse conversion is lossless for ASCII input: each ASCII character becomes a UTF-16 code unit with the high byte zero. The function returns the number of characters written (excluding the null terminator). Again, `mem_copy` avoids alignment issues.

### Name Hash

```c
static UINT16 exfat_name_hash(const UINT16 *name, int length)
{
    UINT16 hash = 0;
    for (int i = 0; i < length; i++) {
        UINT16 ch;
        mem_copy(&ch, &name[i], 2);
        /* Up-case for hashing (ASCII only) */
        if (ch >= 'a' && ch <= 'z')
            ch = (UINT16)(ch - 'a' + 'A');
        UINT8 *bytes = (UINT8 *)&ch;
        hash = (UINT16)(((hash << 15) | (hash >> 1)) + bytes[0]);
        hash = (UINT16)(((hash << 15) | (hash >> 1)) + bytes[1]);
    }
    return hash;
}
```

The name hash is a 16-bit checksum of the up-cased filename, computed per the exFAT specification. It uses a rotate-right-by-1 algorithm, processing each UTF-16 character as two bytes (low byte first, then high byte). The up-casing ensures that the hash is case-insensitive, matching exFAT's case-insensitive name comparison semantics.

This hash is stored in the stream extension entry and used for fast directory lookups. When searching for a filename, the driver can first compare hashes -- if they do not match, the full name comparison is skipped. In our implementation, we do not use this optimization (we always compare names directly), but we must compute correct hashes when creating new entries so that other exFAT implementations can use them.

### Entry Set Checksum

```c
static UINT16 entry_set_checksum(const UINT8 *entries, int count)
{
    UINT16 checksum = 0;
    int total_bytes = count * 32;
    for (int i = 0; i < total_bytes; i++) {
        /* Skip the checksum field itself (bytes 2-3 of first entry) */
        if (i == 2 || i == 3)
            continue;
        checksum = (UINT16)(((checksum << 15) | (checksum >> 1)) + entries[i]);
    }
    return checksum;
}
```

Every file entry set has a 16-bit checksum stored in the file directory entry's `set_checksum` field. The checksum covers all bytes of all entries in the set, except for bytes 2 and 3 of the first entry (which are the checksum field itself -- a chicken-and-egg problem). The algorithm is the same rotate-right-by-1 as the name hash.

We compute this checksum when creating new entry sets (`build_entry_set`) and must do it last, after all other fields are filled in. The checksum is then stored at bytes 2-3 of the first entry. Other exFAT implementations verify this checksum when reading directory entries, so getting it wrong would make our files appear corrupted.

## Directory Traversal

Directories in exFAT are chains of clusters containing 32-byte entries. Walking a directory means reading entries sequentially across sectors and clusters, following the FAT chain (or incrementing cluster numbers for contiguous directories). The `dir_iter` structure encapsulates this traversal:

```c
struct dir_iter {
    struct exfat_vol *vol;
    UINT32 first_cluster;
    int    no_fat_chain;
    UINT64 data_length;

    /* Current position */
    UINT32 cur_cluster;
    UINT32 sector_in_cluster;   /* 0 .. sectors_per_cluster-1 */
    UINT32 entry_in_sector;     /* 0 .. entries_per_sector-1 */
    UINT64 byte_offset;         /* total bytes walked */
    UINT8  *sector_buf;         /* cached from sector cache */
    UINT64 cur_sector;          /* absolute sector number */
};
```

The iterator tracks position at three levels of granularity: which cluster we are in (`cur_cluster`), which sector within that cluster (`sector_in_cluster`), and which 32-byte entry within that sector (`entry_in_sector`). The `byte_offset` is the total number of bytes walked from the start, used to check against `data_length` for directories with a known size.

The `sector_buf` pointer holds the cached sector data. It comes from `cache_read` and is valid until the next cache operation. The `cur_sector` is the absolute sector number on the volume.

### Initialization

```c
static int dir_iter_init(struct dir_iter *it, struct exfat_vol *vol,
                         UINT32 cluster, int no_fat_chain, UINT64 data_length)
{
    it->vol = vol;
    it->first_cluster = cluster;
    it->no_fat_chain = no_fat_chain;
    it->data_length = data_length;
    it->cur_cluster = cluster;
    it->sector_in_cluster = 0;
    it->entry_in_sector = 0;
    it->byte_offset = 0;
    it->sector_buf = 0;
    it->cur_sector = 0;

    if (cluster < 2)
        return -1;

    it->cur_sector = cluster_to_sector(vol, cluster);
    it->sector_buf = cache_read(vol, it->cur_sector);
    if (!it->sector_buf)
        return -1;

    return 0;
}
```

Initialization sets all position fields to zero (beginning of the directory), computes the first sector from the cluster number, and reads that sector into the cache. If the cluster number is invalid (less than 2) or the read fails, initialization returns -1.

The `data_length` parameter is typically 0 for directory traversal, meaning "follow the chain until EOC." For files with a known length, it limits how far the iterator advances. When `data_length` is 0, the `byte_offset >= data_length` check in `dir_iter_get` is effectively disabled.

### Getting the Current Entry

```c
static struct exfat_dentry *dir_iter_get(struct dir_iter *it)
{
    if (!it->sector_buf)
        return 0;
    if (it->data_length > 0 && it->byte_offset >= it->data_length)
        return 0;

    UINT32 off = it->entry_in_sector * 32;
    return (struct exfat_dentry *)(it->sector_buf + off);
}
```

This returns a pointer to the current 32-byte directory entry. The entry is inside the cached sector buffer, so it is valid until the next cache or iterator operation. Callers should copy the entry data (using `mem_copy`) before advancing the iterator.

### Position Queries

```c
static UINT64 dir_iter_sector(struct dir_iter *it)
{
    return it->cur_sector;
}

static UINT32 dir_iter_offset_in_sector(struct dir_iter *it)
{
    return it->entry_in_sector * 32;
}
```

These two functions return the absolute sector number and byte offset within that sector for the current entry. They are used by `parse_entry_set` to record the on-disk location of a file's entry set -- needed later for rename and delete operations, which must know exactly where on disk to modify the entry.

### Advancing to the Next Entry

```c
static int dir_iter_next(struct dir_iter *it)
{
    struct exfat_vol *vol = it->vol;
    UINT32 entries_per_sector = vol->bytes_per_sector / 32;

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
                it->sector_buf = 0;
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
```

Advancing is a three-level increment. First, we increment the entry index within the current sector. If it overflows (reaches `entries_per_sector`, typically 16 for 512-byte sectors), we reset it to 0 and increment the sector index within the current cluster. If that overflows (reaches `sectors_per_cluster`), we move to the next cluster -- either by incrementing (contiguous) or by following the FAT chain.

When we cross a sector boundary, we read the new sector through the cache. When we reach the end of the chain (EOC or bad cluster), we set `sector_buf` to null and return -1 to signal end-of-directory.

The function returns 0 on success (more entries available) and -1 at the end of the directory. This return value is checked after every `dir_iter_next` call in the directory traversal loops.

## Entry Set Parsing

A file in exFAT is not described by a single directory entry. It is described by an "entry set" -- a group of consecutive 32-byte entries. The entry set always starts with a File Directory Entry (0x85), followed by a Stream Extension Entry (0xC0), followed by one or more File Name Extension Entries (0xC1). The `secondary_count` field in the file entry tells us exactly how many entries follow.

### The Parsed Entry Info

```c
struct exfat_entry_info {
    /* From the file entry (0x85) */
    UINT16 attributes;
    UINT32 create_ts;
    UINT32 modify_ts;

    /* From the stream entry (0xC0) */
    UINT32 first_cluster;
    UINT64 data_length;
    UINT64 valid_data_length;
    UINT8  stream_flags;
    UINT8  name_length;
    UINT16 name_hash;

    /* Assembled name (ASCII) */
    char   name[FS_MAX_NAME];

    /* Location of the file entry on disk (for rename/delete) */
    UINT64 file_entry_sector;
    UINT32 file_entry_offset;
    UINT8  secondary_count;
};
```

This structure collects all the useful information from an entry set into a single flat record. The attributes and timestamps come from the file entry. The cluster location, size, and flags come from the stream entry. The name is assembled from the name entries and converted to ASCII.

Crucially, the last three fields record the physical location of the entry set on disk: which sector, what offset within that sector, and how many secondary entries. These are needed by rename and delete operations, which must go back to these exact bytes and modify them (clearing the InUse bit or writing a new entry set).

### The Parser

```c
static int parse_entry_set(struct dir_iter *it, struct exfat_entry_info *info)
{
    struct exfat_dentry *de = dir_iter_get(it);
    if (!de || de->type != ENTRY_FILE)
        return -1;

    struct exfat_file_dentry fd;
    mem_copy(&fd, de, 32);

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
    mem_copy(&sd, de, 32);

    info->first_cluster = sd.first_cluster;
    info->data_length = sd.data_length;
    info->valid_data_length = sd.valid_data_length;
    info->stream_flags = sd.flags;
    info->name_length = sd.name_length;
    info->name_hash = sd.name_hash;

    /* Read file name from subsequent 0xC1 entries */
    UINT16 name_buf[256];
    int name_pos = 0;
    int name_entries = fd.secondary_count - 1; /* minus the stream entry */

    for (int i = 0; i < name_entries; i++) {
        if (dir_iter_next(it) != 0)
            break;
        de = dir_iter_get(it);
        if (!de || de->type != ENTRY_NAME)
            break;

        struct exfat_name_dentry nd;
        mem_copy(&nd, de, 32);

        int chars_to_copy = 15;
        if (name_pos + chars_to_copy > (int)sd.name_length)
            chars_to_copy = (int)sd.name_length - name_pos;
        if (chars_to_copy > 0 && name_pos + chars_to_copy < 256) {
            mem_copy(&name_buf[name_pos], nd.name, (UINTN)chars_to_copy * 2);
            name_pos += chars_to_copy;
        }
    }

    utf16_to_ascii(name_buf, name_pos, info->name, FS_MAX_NAME);
    return 0;
}
```

The parser is the heart of directory reading. It starts at a File Directory Entry, validates it, then advances through the entry set reading each piece.

The first step copies the file entry into a local struct (using `mem_copy` to avoid alignment issues) and records the entry's disk location. The `secondary_count` check ensures the entry set is well-formed -- a file must have at least a stream entry and one name entry.

The second step advances to the stream extension and extracts the file's data cluster, size, and flags. This is where we learn where the file's actual data lives on disk.

The third step loops through the name entries, collecting UTF-16 characters into a buffer. Each name entry holds up to 15 characters, so a 30-character filename spans two name entries. The `chars_to_copy` calculation ensures we do not read past the declared name length (from the stream entry). After collecting all UTF-16 characters, we convert to ASCII.

After `parse_entry_set` returns, the directory iterator has been advanced past the entire entry set. The caller can immediately call `dir_iter_next` to continue to the next entry in the directory. This is important for the calling pattern in `find_in_dir` and `exfat_readdir`, where we iterate through the directory looking at one entry set at a time.

## Path Resolution

Navigating from a path string like "/photos/2024/vacation.jpg" to the actual file requires walking the directory tree, starting at the root and resolving each path component.

### Finding an Entry in a Directory

```c
static int find_in_dir(struct exfat_vol *vol, UINT32 dir_cluster,
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

        UINT8 type = de->type;

        if (type == ENTRY_EOD)
            break;

        if (type == ENTRY_FILE) {
            struct exfat_entry_info ei;
            if (parse_entry_set(&it, &ei) == 0) {
                if (ascii_icmp(ei.name, name) == 0) {
                    if (info)
                        mem_copy(info, &ei, sizeof(ei));
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
```

This function searches a single directory for a named entry. It initializes an iterator on the directory's cluster, then loops through all entries. Non-file entries (bitmap, upcase table, volume label) are skipped. For each file entry, it parses the complete entry set and compares the name case-insensitively. If found, it copies the entry info and returns 0. If the end of directory (EOD) is reached without finding the name, it returns -1.

The iterator management is subtle. After `parse_entry_set` advances past the entry set, we call `dir_iter_next` one more time to move to the entry after the set. This is because `parse_entry_set` leaves the iterator pointing at the last entry of the set (the last name entry), not past it.

### Resolving a Full Path

```c
static int resolve_path(struct exfat_vol *vol, const char *path,
                        struct exfat_entry_info *info)
{
    if (!path || !path[0])
        return -1;

    /* Handle root */
    if (path[0] == '/' && path[1] == '\0') {
        if (info) {
            mem_set(info, 0, sizeof(*info));
            info->first_cluster = vol->root_cluster;
            info->attributes = ATTR_DIRECTORY;
            info->name[0] = '/';
            info->name[1] = '\0';
        }
        return 0;
    }

    UINT32 cur_cluster = vol->root_cluster;
    const char *p = path;

    /* Skip leading slash */
    if (*p == '/')
        p++;

    while (*p) {
        /* Extract next component */
        char component[FS_MAX_NAME];
        int len = 0;
        while (*p && *p != '/' && len < FS_MAX_NAME - 1)
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
                mem_copy(info, &ei, sizeof(ei));
            return 0;
        }
    }

    return -1;
}
```

Path resolution splits the path into components separated by forward slashes and looks up each component in the current directory. Starting from the root cluster, each successful lookup yields either an intermediate directory (whose cluster becomes the new search target) or the final file/directory (whose info is returned to the caller).

The root path "/" is handled as a special case -- there is no directory entry for the root directory itself. We synthesize an `exfat_entry_info` with the root cluster number and directory attributes.

### Resolving the Parent

```c
static UINT32 resolve_parent(struct exfat_vol *vol, const char *path,
                              char *name_out, int name_max)
{
    if (!path || !path[0])
        return 0;

    /* Find last slash */
    const char *last_slash = 0;
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
    mem_copy(parent_path, path, (UINTN)plen);
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
```

Many operations (create, rename, delete) need to know the parent directory cluster and the filename component. Given "/photos/vacation.jpg", this function resolves "/photos" to get the parent directory's cluster, and copies "vacation.jpg" to `name_out`.

For files in the root directory ("/readme.txt" or just "readme.txt"), the parent is the root cluster. For deeper paths, we split at the last slash, resolve the parent path, verify it is a directory, and return its first cluster.

The return value of 0 indicates failure (since cluster numbers start at 2, 0 is never a valid cluster). The caller checks for this and reports an error.

## Reading and Writing Data

With cluster addressing, FAT operations, and path resolution in place, we can now read and write actual file data.

### Reading Data

```c
static int read_data(struct exfat_vol *vol, UINT32 first_cluster,
                     int no_fat_chain, UINT64 length, void *buf)
{
    UINT32 clsz = cluster_size(vol);
    UINT8 *dst = (UINT8 *)buf;
    UINT64 remaining = length;
    UINT32 cluster = first_cluster;

    while (remaining > 0 && cluster >= 2 && cluster != EXFAT_EOC) {
        UINT64 sec = cluster_to_sector(vol, cluster);
        UINT32 chunk = (remaining > clsz) ? clsz : (UINT32)remaining;
        UINT32 full_secs = chunk / vol->bytes_per_sector;
        UINT32 partial = chunk % vol->bytes_per_sector;

        if (full_secs > 0) {
            if (read_sectors_raw(vol, sec, full_secs, dst) != 0)
                return -1;
            dst += full_secs * vol->bytes_per_sector;
        }

        if (partial > 0) {
            UINT8 *tmp = cache_read(vol, sec + full_secs);
            if (!tmp)
                return -1;
            mem_copy(dst, tmp, partial);
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
```

Reading follows the cluster chain, copying data into the caller's buffer one cluster at a time. For each cluster, full sectors are read directly with `read_sectors_raw` (bypassing the cache for efficiency), while the partial last sector (if the file does not end on a sector boundary) is read through the cache.

The `chunk` calculation determines how many bytes to read from this cluster. For all clusters except the last, it is the full cluster size. For the last cluster, it is whatever remains.

After reading a cluster, we follow the chain: either incrementing for contiguous files or consulting the FAT for chained files. If the chain ends before we have read all requested bytes (`remaining > 0`), something is wrong -- the cluster chain is shorter than the declared file size. We return -1 to signal the error.

### Writing Data

```c
static int write_data(struct exfat_vol *vol, const void *data, UINT64 size,
                      UINT32 *out_first)
{
    UINT32 clsz = cluster_size(vol);
    UINT32 clusters_needed = (size > 0)
        ? (UINT32)((size + clsz - 1) / clsz) : 0;

    if (clusters_needed == 0) {
        *out_first = 0;
        return 0;
    }

    const UINT8 *src = (const UINT8 *)data;
    UINT64 remaining = size;
    UINT32 first = 0, prev = 0;

    for (UINT32 i = 0; i < clusters_needed; i++) {
        UINT32 cl = alloc_cluster_chain(vol, prev);
        if (cl == 0)
            return -1;
        if (i == 0)
            first = cl;

        UINT64 sec = cluster_to_sector(vol, cl);
        UINT32 chunk = (remaining > clsz) ? clsz : (UINT32)remaining;
        UINT32 full_secs = chunk / vol->bytes_per_sector;
        UINT32 partial = chunk % vol->bytes_per_sector;

        if (full_secs > 0) {
            if (write_sectors_raw(vol, sec, full_secs, src) != 0)
                return -1;
            src += full_secs * vol->bytes_per_sector;
        }

        if (partial > 0) {
            /* Read-modify-write the last partial sector */
            UINT8 *tmp = cache_read(vol, sec + full_secs);
            if (!tmp)
                return -1;
            mem_set(tmp, 0, vol->bytes_per_sector);
            mem_copy(tmp, src, partial);
            cache_mark_dirty(vol, sec + full_secs);
            src += partial;
        }

        /* Zero remaining sectors in the last cluster */
        if (i == clusters_needed - 1) {
            UINT32 used_secs = (chunk + vol->bytes_per_sector - 1) /
                               vol->bytes_per_sector;
            for (UINT32 s = used_secs; s < vol->sectors_per_cluster; s++) {
                UINT8 *tmp = cache_read(vol, sec + s);
                if (tmp) {
                    mem_set(tmp, 0, vol->bytes_per_sector);
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
```

Writing is the complement of reading, with the added complexity of cluster allocation. We first compute how many clusters are needed (ceiling division of size by cluster size). For each cluster, we allocate it and chain it to the previous one, then write the data.

Full sectors are written directly with `write_sectors_raw`. For the partial last sector, we zero the entire sector first, then copy the partial data. This ensures no stale data leaks from a previous file. The zeroing of remaining sectors in the last cluster serves the same purpose -- we do not want old data visible if the cluster was previously used by a larger file.

The function returns the first cluster number through `out_first`. The caller needs this to populate the stream extension entry.

## Directory Entry Creation

Creating a new file or directory requires three steps: build the entry set (file + stream + name entries), find free space in the parent directory, and write the entries.

### Building the Entry Set

```c
static int build_entry_set(UINT8 *buf, const char *name, UINT16 attributes,
                           UINT32 first_cluster, UINT64 data_length)
{
    int name_len = (int)str_len((const CHAR8 *)name);
    int name_entries = (name_len + 14) / 15;  /* ceiling division */
    int total_entries = 1 + 1 + name_entries;  /* file + stream + names */

    mem_set(buf, 0, (UINTN)total_entries * 32);

    /* Build UTF-16 name */
    UINT16 name_utf16[256];
    mem_set(name_utf16, 0, sizeof(name_utf16));
    for (int i = 0; i < name_len && i < 255; i++) {
        UINT16 ch = (UINT16)(unsigned char)name[i];
        mem_copy(&name_utf16[i], &ch, 2);
    }

    UINT16 hash = exfat_name_hash(name_utf16, name_len);

    /* File Directory Entry (0x85) */
    struct exfat_file_dentry *fd = (struct exfat_file_dentry *)buf;
    fd->type = ENTRY_FILE;
    fd->secondary_count = (UINT8)(total_entries - 1);
    fd->file_attributes = attributes;
    /* Timestamps: encode a fixed date (2026-01-01 00:00:00)
     * exFAT timestamp: bits 31-25=year(since 1980), 24-21=month, 20-16=day,
     *                  15-11=hour, 10-5=minute, 4-0=seconds/2 */
    UINT32 ts = ((2026 - 1980) << 25) | (1 << 21) | (1 << 16);
    fd->create_timestamp = ts;
    fd->modify_timestamp = ts;
    fd->access_timestamp = ts;

    /* Stream Extension (0xC0) */
    struct exfat_stream_dentry *sd = (struct exfat_stream_dentry *)(buf + 32);
    sd->type = ENTRY_STREAM;
    sd->flags = STREAM_ALLOC_POSSIBLE;  /* uses FAT chain */
    sd->name_length = (UINT8)name_len;
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
            mem_copy(nd->name, &name_utf16[start], (UINTN)chars * 2);
        /* Remaining chars are zero (already cleared by mem_set) */
    }

    /* Compute and set checksum */
    UINT16 checksum = entry_set_checksum(buf, total_entries);
    mem_copy(buf + 2, &checksum, 2);

    return total_entries;
}
```

The function constructs a complete entry set in a caller-provided buffer. The caller must provide a buffer large enough for `(3 + name_len/15) * 32` bytes. In practice, we use a 640-byte buffer (20 entries), which handles filenames up to 270 characters.

The process is straightforward. First, compute how many name entries are needed: ceiling division of the name length by 15. The total entry count is 1 (file) + 1 (stream) + name_entries. Zero the entire buffer.

Build the UTF-16 version of the name for hashing. Compute the name hash.

Fill in the file entry: type 0x85, secondary count, attributes, and timestamps. We use a fixed timestamp because the workstation has no real-time clock. January 1, 2026 is encoded as `(46 << 25) | (1 << 21) | (1 << 16)` where 46 = 2026 - 1980.

Fill in the stream extension: type 0xC0, flags indicating FAT-chained allocation, name length, hash, first cluster, and data length. Both `data_length` and `valid_data_length` are set to the same value.

Fill in the name entries: type 0xC1, with 15 UTF-16 characters each. The last entry may have fewer than 15 characters; the rest are already zero.

Finally, compute the checksum over the entire entry set and store it in bytes 2-3 of the first entry.

### Finding Free Directory Space

```c
static int find_free_dir_slot(struct exfat_vol *vol, UINT32 dir_cluster,
                              int count, UINT64 *out_sector, UINT32 *out_offset)
{
    struct dir_iter it;
    if (dir_iter_init(&it, vol, dir_cluster, 0, 0) != 0)
        return -1;

    /* Track consecutive free entries */
    int free_run = 0;
    UINT64 run_start_sector = 0;
    UINT32 run_start_offset = 0;

    UINT32 last_cluster = dir_cluster;

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
                /* Can we fit more entries after this EOD? */
                /* Check remaining capacity in the cluster chain */
                UINT32 bps = vol->bytes_per_sector;
                UINT32 entries_per_sector = bps / 32;
                /* Estimate: all remaining entries in all remaining sectors
                 * of this cluster and chain are available */
                /* For simplicity, if we haven't found enough,
                 * try advancing and counting */
                while (free_run < count) {
                    if (dir_iter_next(&it) != 0) {
                        /* Need to allocate a new cluster */
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

        last_cluster = it.cur_cluster;
        if (dir_iter_next(&it) != 0)
            break;
    }

alloc_new:
    /* Allocate a new cluster for the directory */
    {
        /* Find the last cluster in the chain */
        UINT32 cl = dir_cluster;
        UINT32 prev = cl;
        while (cl >= 2 && cl != EXFAT_EOC) {
            prev = cl;
            cl = fat_get(vol, cl);
        }

        UINT32 new_cl = alloc_cluster_chain(vol, prev);
        if (new_cl == 0)
            return -1;

        /* Zero the new cluster */
        UINT64 sec = cluster_to_sector(vol, new_cl);
        for (UINT32 s = 0; s < vol->sectors_per_cluster; s++) {
            UINT8 *buf = cache_read(vol, sec + s);
            if (!buf)
                return -1;
            mem_set(buf, 0, vol->bytes_per_sector);
            cache_mark_dirty(vol, sec + s);
        }
        cache_flush_all(vol);

        /* The free run might span from existing data into the new cluster.
         * If we had some free entries already, re-use them.
         * Otherwise, the new cluster starts at offset 0. */
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
```

Finding a slot for a new entry set is complex because the entries must be contiguous -- you cannot split a file's entry set across non-adjacent locations. The function walks the directory looking for a run of consecutive free entries (entries with the InUse bit clear or EOD entries) that is at least `count` entries long.

The algorithm tracks the length and starting position of the current run of free entries. When an in-use entry is encountered, the run resets to zero. When an EOD entry is found, everything after it is implicitly free, so the function continues advancing the iterator to count available slots.

If the directory runs out of space (the iterator reaches the end of the cluster chain without finding enough free slots), the function falls through to `alloc_new`, which allocates a new cluster, appends it to the directory's chain, zeros it out, and reports the slot position.

The zeroing of the new cluster is essential -- it fills all entries with 0x00 (ENTRY_EOD), making them available for use and properly terminating the directory.

### Writing the Entry Set

```c
static int write_entry_set(struct exfat_vol *vol, UINT64 start_sector,
                           UINT32 start_offset, const UINT8 *entries,
                           int count)
{
    UINT64 sector = start_sector;
    UINT32 offset = start_offset;
    const UINT8 *src = entries;

    for (int i = 0; i < count; i++) {
        UINT8 *buf = cache_read(vol, sector);
        if (!buf)
            return -1;

        mem_copy(buf + offset, src, 32);
        cache_mark_dirty(vol, sector);
        src += 32;
        offset += 32;

        if (offset >= vol->bytes_per_sector) {
            offset = 0;
            sector++;
            /* Note: this simple increment works within a cluster.
             * Cross-cluster boundaries need more logic, but for
             * reasonable entry sets (< 1 sector), this is fine. */
        }
    }

    return cache_flush_all(vol);
}
```

Writing entry set data to the directory is straightforward: read each target sector through the cache, copy 32 bytes of entry data, mark dirty, advance. When the offset crosses a sector boundary, we increment the sector number.

The comment acknowledges a limitation: this simple sector increment does not handle cross-cluster boundaries. An entry set that spans from the last sector of one cluster into the first sector of the next cluster would need FAT chain following to find the next sector. In practice, entry sets are small (typically 3-5 entries, 96-160 bytes), and `find_free_dir_slot` ensures the run starts in a position with enough room. The limitation is documented rather than worked around.

### Adding to a Directory

```c
static int add_entry_to_dir(struct exfat_vol *vol, UINT32 dir_cluster,
                            const UINT8 *entry_set, int entry_count)
{
    UINT64 slot_sector;
    UINT32 slot_offset;

    if (find_free_dir_slot(vol, dir_cluster, entry_count,
                           &slot_sector, &slot_offset) != 0)
        return -1;

    return write_entry_set(vol, slot_sector, slot_offset,
                           entry_set, entry_count);
}
```

This is the high-level function that combines slot finding and entry writing. It is called by `exfat_writefile`, `exfat_mkdir`, and `exfat_rename` whenever they need to add a new file or directory to a parent directory.

## The Public API

With all the infrastructure in place -- cache, FAT, bitmap, directory traversal, path resolution, data I/O, and entry creation -- we can now implement the public API functions that the volume abstraction layer calls.

### Mounting

```c
struct exfat_vol *exfat_mount(exfat_block_read_fn read_fn,
                               exfat_block_write_fn write_fn,
                               void *ctx, UINT32 block_size)
{
    if (!read_fn || block_size == 0)
        return 0;

    /* Read the boot sector. It starts at LBA 0 of the volume.
     * We need at least 512 bytes. Allocate a buffer aligned to block_size. */
    UINT8 *boot_buf = (UINT8 *)mem_alloc(4096);
    if (!boot_buf)
        return 0;

    /* Read enough blocks to cover 512 bytes */
    UINT32 blocks_needed = (512 + block_size - 1) / block_size;
    if (blocks_needed == 0)
        blocks_needed = 1;

    if (read_fn(ctx, 0, blocks_needed, boot_buf) != 0) {
        mem_free(boot_buf);
        return 0;
    }

    /* Validate the boot sector */
    struct exfat_boot_sector bs;
    mem_copy(&bs, boot_buf, sizeof(bs));
    mem_free(boot_buf);

    /* Check filesystem name */
    static const UINT8 exfat_sig[8] = { 'E','X','F','A','T',' ',' ',' ' };
    int sig_ok = 1;
    for (int i = 0; i < 8; i++) {
        if (bs.fs_name[i] != exfat_sig[i]) {
            sig_ok = 0;
            break;
        }
    }
    if (!sig_ok)
        return 0;

    /* Check boot signature */
    if (bs.boot_signature != 0xAA55)
        return 0;

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
            return 0;
    }

    /* Validate shifts */
    if (bs.bytes_per_sector_shift < 9 || bs.bytes_per_sector_shift > 12)
        return 0;
    if (bs.sectors_per_cluster_shift > 25)
        return 0;

    /* Allocate volume structure */
    struct exfat_vol *vol = (struct exfat_vol *)mem_alloc(sizeof(struct exfat_vol));
    if (!vol)
        return 0;
    mem_set(vol, 0, sizeof(*vol));

    vol->read_fn = read_fn;
    vol->write_fn = write_fn;
    vol->ctx = ctx;
    vol->dev_block_size = block_size;
    vol->bytes_per_sector = (UINT32)1 << bs.bytes_per_sector_shift;
    vol->sectors_per_cluster = (UINT32)1 << bs.sectors_per_cluster_shift;
    vol->cluster_count = bs.cluster_count;
    vol->fat_offset = bs.fat_offset;
    vol->fat_length = bs.fat_length;
    vol->cluster_heap_offset = bs.cluster_heap_offset;
    vol->root_cluster = bs.root_cluster;
    vol->volume_length = bs.volume_length;

    /* Initialize sector cache */
    cache_init(vol);

    /* Load metadata (bitmap + volume label) */
    if (load_metadata(vol) != 0) {
        cache_free(vol);
        mem_free(vol);
        return 0;
    }

    return vol;
}
```

Mount is the entry point for the entire driver. It reads and validates the boot sector, allocates the volume structure, initializes the cache, and loads metadata (the allocation bitmap and volume label).

The validation is thorough. We check the "EXFAT   " signature, the 0xAA55 boot marker, the MustBeZero region (which distinguishes exFAT from FAT), and the sector/cluster shift values (sector shifts of 9-12 correspond to 512-4096 byte sectors; cluster shift up to 25 is the specification limit).

The shift-to-size conversion uses bit shifting: `1 << 9 = 512`, `1 << 12 = 4096`. This is more efficient than a power function and leverages the fact that the on-disk format already stores these as shift values.

The `load_metadata` function, called at the end of mount, scans the root directory for two critical entries:

```c
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

        UINT8 type = de->type;

        if (type == ENTRY_EOD)
            break;

        if (type == ENTRY_BITMAP) {
            struct exfat_bitmap_dentry bd;
            mem_copy(&bd, de, 32);

            /* Only use the first bitmap (bitmap_flags bit 0 = 0) */
            if ((bd.bitmap_flags & 1) == 0) {
                vol->bitmap_cluster = bd.first_cluster;
                vol->bitmap_size = (UINT32)bd.data_length;
                /* Check if contiguous (we assume it always uses FAT for safety,
                 * but the bitmap entry doesn't have a NoFatChain flag —
                 * the bitmap is always at a known cluster and usually contiguous) */
                vol->bitmap_no_fat_chain = 0;
                found_bitmap = 1;
            }
        } else if (type == ENTRY_VLABEL) {
            struct exfat_label_dentry ld;
            mem_copy(&ld, de, 32);
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
    vol->bitmap = (UINT8 *)mem_alloc((UINTN)vol->bitmap_size);
    if (!vol->bitmap)
        return -1;
    mem_set(vol->bitmap, 0, vol->bitmap_size);

    if (bitmap_load(vol) != 0) {
        mem_free(vol->bitmap);
        vol->bitmap = 0;
        return -1;
    }

    return 0;
}
```

This function iterates through the root directory looking for the allocation bitmap entry (0x81) and the volume label entry (0x83). When it finds the bitmap entry, it records the cluster location and size. When it finds the label, it converts it from UTF-16 to ASCII.

The bitmap is mandatory -- without it, we cannot track free space. If the bitmap entry is not found, the mount fails. The label is optional -- a volume without a label simply has an empty string.

After locating the bitmap, we allocate a memory buffer for it and load the bitmap data from disk using `bitmap_load`. The bitmap stays in memory for the entire mount duration, so allocation and deallocation operations are fast (just bit operations on the in-memory buffer). Changes are flushed to disk by `bitmap_flush`.

The driver also contains a local `sort_entries` function for sorting directory listings:

```c
static void sort_entries(struct fs_entry *entries, int count)
{
    /* Simple insertion sort: directories first, then alphabetical */
    for (int i = 1; i < count; i++) {
        struct fs_entry tmp;
        mem_copy(&tmp, &entries[i], sizeof(tmp));
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
            mem_copy(&entries[j + 1], &entries[j], sizeof(struct fs_entry));
            j--;
        }
        mem_copy(&entries[j + 1], &tmp, sizeof(struct fs_entry));
    }
}
```

This is the same sorting logic used in `fs.c` -- directories first, then alphabetical within each group. The exFAT driver has its own copy because it needs to sort its own readdir results before returning them to the caller. An insertion sort is fine for our maximum of 256 entries per directory.

### Unmounting

```c
void exfat_unmount(struct exfat_vol *vol)
{
    if (!vol)
        return;

    /* Flush everything */
    bitmap_flush(vol);
    cache_flush_all(vol);

    /* Free resources */
    if (vol->bitmap)
        mem_free(vol->bitmap);
    cache_free(vol);
    mem_free(vol);
}
```

Unmounting is clean: flush the bitmap to disk, flush any dirty cache entries, free the bitmap buffer, free the cache buffers, and free the volume structure. After this, the volume pointer is invalid.

The flush ordering matters. The bitmap must be flushed before the cache because `bitmap_flush` may use the cache for partial-sector writes. Flushing the cache afterward ensures those partial-sector writes reach the device.

### Reading a Directory

```c
int exfat_readdir(struct exfat_vol *vol, const char *path,
                  struct fs_entry *entries, int max_entries)
{
    if (!vol || !path || !entries || max_entries <= 0)
        return -1;

    /* Resolve the path to a directory */
    struct exfat_entry_info dir_info;
    if (resolve_path(vol, path, &dir_info) != 0)
        return -1;

    if (!(dir_info.attributes & ATTR_DIRECTORY))
        return -1;

    UINT32 dir_cluster = dir_info.first_cluster;
    int count = 0;

    struct dir_iter it;
    if (dir_iter_init(&it, vol, dir_cluster, 0, 0) != 0)
        return -1;

    for (;;) {
        struct exfat_dentry *de = dir_iter_get(&it);
        if (!de)
            break;

        UINT8 type = de->type;

        if (type == ENTRY_EOD)
            break;

        if (type == ENTRY_FILE) {
            struct exfat_entry_info ei;
            if (parse_entry_set(&it, &ei) == 0) {
                if (count < max_entries) {
                    str_copy(entries[count].name, ei.name, FS_MAX_NAME);
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
```

Reading a directory resolves the path, verifies it is a directory, then iterates through all entries. For each file entry set, it parses the entry, copies the name, size, and directory flag into the caller's `fs_entry` array, and advances. Non-file entries (bitmap, upcase, label) are silently skipped. After collecting all entries, the array is sorted and the count returned.

The function returns -1 on error and the entry count on success. The caller (typically `fs_readdir` in `fs.c`, which in turn is called by `browse.c`) provides a pre-allocated array and maximum count.

### Reading a File

```c
void *exfat_readfile(struct exfat_vol *vol, const char *path, UINTN *out_size)
{
    if (!vol || !path || !out_size)
        return 0;

    struct exfat_entry_info info;
    if (resolve_path(vol, path, &info) != 0)
        return 0;

    if (info.attributes & ATTR_DIRECTORY)
        return 0;

    UINT64 size = info.data_length;
    if (size == 0) {
        /* Empty file: return a 1-byte buffer with NUL */
        UINT8 *buf = (UINT8 *)mem_alloc(1);
        if (buf)
            buf[0] = 0;
        *out_size = 0;
        return buf;
    }

    UINT8 *buf = (UINT8 *)mem_alloc((UINTN)size);
    if (!buf)
        return 0;

    int no_fat = (info.stream_flags & STREAM_NO_FAT_CHAIN) ? 1 : 0;

    if (read_data(vol, info.first_cluster, no_fat, size, buf) != 0) {
        mem_free(buf);
        return 0;
    }

    *out_size = (UINTN)size;
    return buf;
}
```

Reading a file resolves the path, allocates a buffer for the entire file contents, and reads the data through `read_data`. The function handles the NoFatChain flag -- if the file is stored contiguously, we pass `no_fat=1` to `read_data`, which then increments cluster numbers instead of consulting the FAT.

Empty files get special treatment: a 1-byte buffer with a NUL terminator, and `out_size` set to 0. This prevents callers from receiving a NULL pointer for a valid empty file. The caller must `mem_free` the returned buffer.

### Writing a File

```c
int exfat_writefile(struct exfat_vol *vol, const char *path,
                    const void *data, UINTN size)
{
    if (!vol || !path)
        return -1;
    if (!vol->write_fn)
        return -1;

    /* Resolve parent directory */
    char filename[FS_MAX_NAME];
    UINT32 parent_cluster = resolve_parent(vol, path, filename, FS_MAX_NAME);
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
        UINT64 sec = existing.file_entry_sector;
        UINT32 off = existing.file_entry_offset;
        int total = 1 + existing.secondary_count;

        for (int i = 0; i < total; i++) {
            UINT8 *buf = cache_read(vol, sec);
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
    UINT32 first_cluster = 0;
    if (size > 0) {
        if (write_data(vol, data, (UINT64)size, &first_cluster) != 0)
            return -1;
    }

    /* Build entry set */
    UINT8 entry_buf[32 * 20]; /* max ~18 name entries + file + stream */
    mem_set(entry_buf, 0, sizeof(entry_buf));
    int entry_count = build_entry_set(entry_buf, filename, ATTR_ARCHIVE,
                                      first_cluster, (UINT64)size);
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
```

Writing a file is the most complex public function. It uses a delete-and-recreate strategy rather than in-place modification. If the file already exists, we first free its data clusters and mark its directory entries as deleted (by clearing the InUse bit -- bit 7 -- of each entry's type byte). Then we write the new data to freshly allocated clusters, build a new entry set, and add it to the parent directory.

This approach is simpler than in-place modification (which would need to handle growing and shrinking cluster chains, updating size fields, recalculating checksums on existing entries). The cost is fragmentation -- each rewrite of a file may scatter its clusters more. For our use case (editing small text files), this is irrelevant.

The InUse bit clearing is a neat exFAT feature. Each entry type has bit 7 set when active (0x85 = in-use file, 0xC0 = in-use stream). Clearing bit 7 turns them into 0x05 and 0x40 respectively -- "deleted" entries that exFAT implementations skip during directory traversal. The space can later be reclaimed by new entries.

### Creating a Directory

```c
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
    UINT32 cur_cluster = vol->root_cluster;
    const char *p = path;

    if (*p == '/')
        p++;

    while (*p) {
        char component[FS_MAX_NAME];
        int len = 0;
        while (*p && *p != '/' && len < FS_MAX_NAME - 1)
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
        UINT32 new_cluster = alloc_cluster(vol);
        if (new_cluster == 0)
            return -1;

        /* Zero the cluster (empty directory) */
        UINT64 sec = cluster_to_sector(vol, new_cluster);
        for (UINT32 s = 0; s < vol->sectors_per_cluster; s++) {
            UINT8 *buf = cache_read(vol, sec + s);
            if (!buf)
                return -1;
            mem_set(buf, 0, vol->bytes_per_sector);
            cache_mark_dirty(vol, sec + s);
        }
        cache_flush_all(vol);

        /* Build entry set for the new directory */
        UINT8 entry_buf[32 * 20];
        mem_set(entry_buf, 0, sizeof(entry_buf));
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
```

Creating a directory is like a recursive mkdir -- it walks the path component by component, creating each missing directory along the way. For each component, it first checks if the directory already exists. If it does, it moves to that directory. If it does not, it allocates a cluster, zeros it out (making it an empty directory with all EOD entries), builds an entry set with `ATTR_DIRECTORY`, and adds it to the parent.

Note that the `data_length` is 0 for the entry set. This is correct for exFAT directories -- their size is determined by the cluster chain, not by a length field. A directory simply occupies as many clusters as it needs, with the chain terminated by EOC.

The function returns 0 if the directory already exists, 0 if it was successfully created, and -1 on error.

### Renaming

```c
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
    char old_name[FS_MAX_NAME];
    UINT32 parent_cluster = resolve_parent(vol, path, old_name, FS_MAX_NAME);
    if (parent_cluster == 0)
        return -1;

    /* Check that the new name doesn't already exist */
    struct exfat_entry_info conflict;
    if (find_in_dir(vol, parent_cluster, new_name, &conflict) == 0)
        return -1;  /* Name already taken */

    /* Strategy: delete old entry, create new one with same data */
    UINT32 first_cluster = info.first_cluster;
    UINT64 data_length = info.data_length;
    UINT16 attributes = info.attributes;

    /* Mark old directory entries as deleted */
    UINT64 sec = info.file_entry_sector;
    UINT32 off = info.file_entry_offset;
    int total = 1 + info.secondary_count;

    for (int i = 0; i < total; i++) {
        UINT8 *buf = cache_read(vol, sec);
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
    UINT8 entry_buf[32 * 20];
    mem_set(entry_buf, 0, sizeof(entry_buf));
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
```

Rename uses the same delete-and-recreate strategy as writefile. The old entry set is marked as deleted (InUse bit cleared), and a new entry set with the new name is created. The data clusters are not touched -- only the directory metadata changes. The `first_cluster`, `data_length`, and `attributes` are preserved from the old entry and used in the new one.

This approach handles name length changes gracefully. If the new name is longer, the new entry set needs more name entries and may not fit in the same location. If shorter, the old location would have wasted space. By deleting and recreating, we always find the best available slot.

The function first checks that the new name does not already exist in the same directory, preventing duplicate entries.

### Deleting

```c
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
    UINT64 sec = info.file_entry_sector;
    UINT32 off = info.file_entry_offset;
    int total = 1 + info.secondary_count;

    for (int i = 0; i < total; i++) {
        UINT8 *buf = cache_read(vol, sec);
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
```

Deleting a file or directory has three steps: check that directories are empty (to prevent orphaned subtrees), free the data clusters, and mark the directory entries as deleted.

The emptiness check iterates through the directory looking for any ENTRY_FILE (0x85) entries. If one is found, the directory is not empty and the delete is refused. This prevents the user from deleting a directory that still contains files, which would leave those files' clusters allocated but unreachable -- a disk space leak.

After freeing the clusters (which clears bitmap bits and FAT entries), the directory entries are marked deleted with the InUse bit trick. Finally, the bitmap is flushed to ensure the freed clusters are properly recorded on disk.

### Volume Information

```c
int exfat_volume_info(struct exfat_vol *vol, UINT64 *total_bytes,
                      UINT64 *free_bytes)
{
    if (!vol)
        return -1;

    UINT32 clsz = cluster_size(vol);

    if (total_bytes)
        *total_bytes = (UINT64)vol->cluster_count * (UINT64)clsz;

    if (free_bytes) {
        UINT32 free_clusters = 0;
        for (UINT32 i = 0; i < vol->cluster_count; i++) {
            if (!bitmap_get(vol, i + 2))
                free_clusters++;
        }
        *free_bytes = (UINT64)free_clusters * (UINT64)clsz;
    }

    return 0;
}
```

Volume info computes total size from the cluster count and cluster size, and free space by counting unset bits in the allocation bitmap. The bitmap scan is O(N) in the number of clusters, but since the bitmap is in memory, this is fast even for large volumes.

### Utility Functions

```c
UINT64 exfat_file_size(struct exfat_vol *vol, const char *path)
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
```

File size simply resolves the path and returns the data length from the stream extension. Returns 0 for directories or non-existent files.

```c
int exfat_exists(struct exfat_vol *vol, const char *path)
{
    if (!vol || !path)
        return 0;

    struct exfat_entry_info info;
    return (resolve_path(vol, path, &info) == 0) ? 1 : 0;
}
```

Existence check resolves the path and returns 1 if successful, 0 if the file is not found. This is used by the `fs_exists` dispatch layer.

```c
const char *exfat_get_label(struct exfat_vol *vol)
{
    if (!vol)
        return "";
    return vol->label;
}
```

Label access returns the ASCII volume label loaded during mount. Returns an empty string if the volume has no label or the volume pointer is null.

## Browse and Editor Integration

The exFAT driver and volume abstraction are invisible infrastructure. The user sees the results through two places: the file browser (`browse.c`) and the text editor (`edit.c`). Both need changes to discover, display, and interact with custom volumes.

### Browser State

At the top of `browse.c`, alongside the existing USB volume state, we add custom volume tracking:

```c
/* Custom volume state (exFAT/NTFS) */
static struct fs_custom_volume s_custom_vols[8];
static int s_custom_count;
static int s_custom_start_idx;
static int s_on_custom;           /* browsing exFAT or NTFS volume? */
```

The `s_custom_vols` array holds the descriptors returned by `fs_enumerate_custom_volumes`. The `s_custom_count` is how many were found. The `s_custom_start_idx` records where in the entry list custom volume entries begin (after the real files and USB entries). The `s_on_custom` flag tracks whether we are currently browsing inside a custom volume.

### Path Display

When browsing a custom volume, the path bar at the top of the screen shows a volume type tag:

```c
if (s_on_custom) {
    enum fs_vol_type vt = fs_get_vol_type();
    const char *tag = (vt == FS_VOL_NTFS) ? "[NTFS] " :
                      (vt == FS_VOL_EXFAT) ? "[exFAT] " : "[USB] ";
    int k = 0;
    while (tag[k] && i < (int)g_boot.cols)
        line[i++] = tag[k++];
}
```

This goes inside `draw_path()`, right before the path string is appended. The user sees "[exFAT] /" or "[NTFS] /photos" instead of just "/" or "/photos". This makes it immediately clear which volume type is active.

### Status Bar

The status bar at the bottom shows different key options depending on the context:

```c
int on_custom_entry = (!s_on_usb && !s_on_custom && s_custom_count > 0
                       && s_cursor >= s_custom_start_idx
                       && s_cursor < s_custom_start_idx + s_custom_count);

if (on_custom_entry) {
    msg = " ENTER:Open                                        BS:Back ESC:Exit";
} else if (s_on_custom && fs_is_read_only()) {
    if (on_iso)
        msg = " ENTER:Open F3:Copy F10:WriteISO               BS:Back";
    else
        msg = " ENTER:Open F3:Copy                            BS:Back";
} else if (s_on_custom) {
    if (on_iso)
        msg = " ENTER:Open F3:Copy F10:WriteISO               BS:Back";
    else
        msg = " ENTER:Open F3:Copy                            BS:Back";
}
```

When the cursor is on a custom volume entry (not yet mounted), only ENTER is available. When browsing inside a read-only volume (NTFS), write operations are omitted from the status bar. When browsing a writable custom volume (exFAT), the same read operations are shown.

### Entry Coloring

Custom volume entries get distinctive colors in the file listing:

```c
int is_custom_entry = (!s_on_usb && !s_on_custom && s_custom_count > 0
                       && entry_idx >= s_custom_start_idx
                       && entry_idx < s_custom_start_idx + s_custom_count);

if (is_custom_entry) {
    /* Color by volume type: orange for exFAT, magenta for NTFS */
    int ci = entry_idx - s_custom_start_idx;
    fg = (s_custom_vols[ci].type == FS_VOL_NTFS) ? COLOR_MAGENTA : COLOR_ORANGE;
}
```

exFAT volumes appear in orange text, matching the existing USB volume color. NTFS volumes appear in magenta. This gives the user an immediate visual cue about the volume type. The regular `[USB]` FAT32 entries continue to be orange. `[DISK]` entries remain red.

### Directory Loading

When loading the root directory of the boot volume, the browser enumerates custom volumes and appends them as entries:

```c
/* Enumerate exFAT/NTFS volumes */
s_custom_count = 0;
s_custom_start_idx = s_count;
s_custom_count = fs_enumerate_custom_volumes(s_custom_vols, 8);

for (int i = 0; i < s_custom_count && s_count < MAX_ENTRIES; i++) {
    struct fs_entry *e = &s_entries[s_count];
    int pos = 0;
    const char *tag = (s_custom_vols[i].type == FS_VOL_NTFS)
                      ? "[NTFS] " : "[exFAT] ";
    while (*tag && pos < FS_MAX_NAME - 2)
        e->name[pos++] = *tag++;
    int j = 0;
    while (s_custom_vols[i].label[j] && pos < FS_MAX_NAME - 1)
        e->name[pos++] = s_custom_vols[i].label[j++];
    e->name[pos] = '\0';
    e->size = s_custom_vols[i].size_bytes;
    e->is_dir = 1;
    s_count++;
}
```

The entries appear after the USB volume entries and before the raw disk entries. Each entry shows a tag (`[exFAT]` or `[NTFS]`) followed by the volume label and size. The `is_dir` flag is set to 1 so they appear in the directory section of the sorted listing.

When filtering raw disk devices, the browser also skips devices already listed as custom volumes to avoid duplicates:

```c
/* Skip devices already listed as custom volumes */
int skip = 0;
for (int ci = 0; ci < s_custom_count; ci++) {
    if (s_custom_vols[ci].handle == all_devs[i].handle) {
        skip = 1;
        break;
    }
}
if (skip) continue;
```

Without this check, an exFAT USB drive would appear both as an `[exFAT]` entry (from custom volume enumeration) and as a `[DISK]` entry (from raw block device enumeration).

### Entering a Custom Volume

When the user presses ENTER on a custom volume entry, the browser mounts it:

```c
} else if (!s_on_usb && !s_on_custom && s_custom_count > 0
           && s_cursor >= s_custom_start_idx
           && s_cursor < s_custom_start_idx + s_custom_count) {
    /* Entering an exFAT/NTFS volume */
    int ci = s_cursor - s_custom_start_idx;
    if (fs_set_custom_volume(s_custom_vols[ci].type,
                              s_custom_vols[ci].handle) == 0) {
        s_on_custom = 1;
        path_set_root();
        load_dir();
        draw_all();
    } else {
        draw_status_msg(" Failed to mount volume");
    }
}
```

The cursor index is translated to a custom volume index, then `fs_set_custom_volume` is called with the volume type and handle. If mounting succeeds, we set `s_on_custom`, reset the path to root, and reload the directory listing. If mounting fails, an error message is displayed.

### Leaving a Custom Volume

Pressing Backspace or Escape at the root of a custom volume returns to the boot volume:

```c
case KEY_BS:
    if (s_on_custom && path_is_root()) {
        /* Leave custom volume, return to boot root */
        fs_restore_boot_volume();
        s_on_custom = 0;
        path_set_root();
        load_dir();
        draw_all();
    }
    ...
```

`fs_restore_boot_volume()` calls `unmount_custom()` internally, which flushes and frees the exFAT or NTFS volume, then restores `s_vol_type` to `FS_VOL_SFS` and `s_root` to the boot volume root handle. The same handling is duplicated for KEY_ESC.

### Write Operation Guards

Write operations (F4:New, F8:Paste, F9:Rename) check `fs_is_read_only()` before proceeding:

```c
case KEY_F4:
    if (fs_is_read_only()) {
        draw_status_msg(" Volume is read-only");
        break;
    }
    prompt_new_file();
    load_dir();
    draw_all();
    break;

case KEY_F8:
    if (fs_is_read_only()) {
        draw_status_msg(" Volume is read-only");
        break;
    }
    if (do_paste() == 0) {
        load_dir();
        draw_all();
    }
    break;

case KEY_F9:
    if (fs_is_read_only()) {
        draw_status_msg(" Volume is read-only");
        break;
    }
    do_rename();
    load_dir();
    draw_all();
    break;
```

These guards prevent writes to NTFS volumes. For exFAT volumes, `fs_is_read_only()` returns 0, so write operations proceed normally through the dispatch layer.

### Editor Integration

The editor needs three changes for custom volume support.

First, the title bar shows a `[READ-ONLY]` indicator when the current volume is read-only:

```c
if (fs_is_read_only() && i + 12 < (int)g_boot.cols) {
    const char *ro = " [READ-ONLY]";
    int k = 0;
    while (ro[k] && i < (int)g_boot.cols)
        line[i++] = ro[k++];
}
```

This appears after the filename (and the modification asterisk, if any). The user sees "readme.txt [READ-ONLY]" in the title bar.

Second, the status bar omits write-related keys:

```c
if (fs_is_read_only())
    msg = " F3:Select  ESC:Exit  [READ-ONLY]";
else
    msg = " F2:Save  F3:Select  F5:Run  F6:Rebuild  ESC:Exit";
```

On a read-only volume, F2:Save, F5:Run, and F6:Rebuild are hidden from the status bar.

Third, F2:Save is blocked:

```c
case KEY_F2:
    if (fs_is_read_only()) {
        draw_info("Volume is read-only");
    } else {
        handle_save();
    }
    break;
```

If the user presses F2 on a read-only volume, they get an informational message instead of a save attempt.

Finally, the F6:Rebuild source list includes the new driver files:

```c
static const char *sources[] = {
    "/src/main.c", "/src/fb.c", "/src/kbd.c", "/src/mem.c",
    "/src/font.c", "/src/fs.c", "/src/browse.c", "/src/edit.c",
    "/src/shim.c", "/src/tcc.c",
    "/src/disk.c", "/src/fat32.c", "/src/iso.c",
    "/src/exfat.c", "/src/ntfs.c",
    NULL
};
```

Both `exfat.c` and `ntfs.c` are added to the self-hosting rebuild. When the user presses F6, TCC compiles all source files including the filesystem drivers, producing a new workstation binary that includes everything.

## What We Built

This chapter added about 2,000 lines of C to the workstation -- roughly 100 KB of source code. The exFAT driver alone is the largest single module in the project, bigger than the editor, the file browser, or the TCC integration layer. Yet it compiles to about 15 KB of machine code, well within our size budget. The total binary with both architectures remains comfortably under the 4 MB target.

Here is what we gained. Plug in any exFAT-formatted USB drive -- a 64 GB flash drive, a camera SD card, a phone's storage -- and the file browser shows it as an orange `[exFAT]` entry with its volume label and size. Press ENTER, and you are browsing its contents: directories, files, sizes, all rendered in the familiar file browser interface. Open a text file and it loads in the editor. Create a new file, save it, and it is written to the exFAT volume. The volume abstraction layer makes all of this transparent -- no special commands, no mode switches.

The architecture is layered cleanly. The exFAT driver knows nothing about UEFI -- it speaks only through block I/O callbacks. The volume abstraction in `fs.c` bridges between UEFI handles and driver callbacks. The browser and editor know nothing about exFAT internals -- they call the same `fs_readdir`, `fs_readfile`, and `fs_writefile` functions they always have. Each layer has a well-defined interface, and changes to one layer do not ripple through the others.

The design decisions are worth revisiting. We chose a write-back sector cache with 8 slots and clock eviction -- simple enough to implement in 100 lines, effective enough to prevent redundant USB transactions during directory traversal. We load the entire allocation bitmap into memory at mount time, making allocation checks instantaneous at the cost of a few kilobytes of RAM. We use a delete-and-recreate strategy for file writes and renames, sacrificing fragmentation for implementation simplicity. These are all pragmatic choices for a workstation that edits small text files, not a high-performance server filesystem.

We also built the volume abstraction to be extensible. Adding support for another filesystem -- say, ext4 or HFS+ -- would mean implementing a new driver with the same callback interface, adding a new value to `fs_vol_type`, and extending the dispatch blocks in `fs.c`. No changes to the browser or editor would be needed.

The 32-gigabyte wall is gone. In the next chapter, we will add NTFS read support, handling the other major filesystem found on USB drives. That driver is read-only -- NTFS write support would be an order of magnitude more complex -- but being able to read files from NTFS drives covers the most common use case: recovering data from Windows-formatted storage.
