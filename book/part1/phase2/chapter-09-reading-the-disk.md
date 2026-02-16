---
layout: default
title: "Chapter 9: Reading the Disk"
parent: "Phase 2: Filesystem & Editor"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 1
---

# Chapter 9: Reading the Disk

## From Pixels to Files

Phase 1 gave us a screen, a keyboard, and memory management. We can render text, accept keystrokes, and allocate buffers. But the workstation has nothing to show -- no files, no documents, no source code. The framebuffer and keyboard are I/O devices, but without access to the filesystem, we are just a fancy typewriter with no paper.

Our SD card is formatted as FAT32. It already contains our boot binary at `\EFI\BOOT\BOOTAA64.EFI`. It can also hold anything else we put on it -- survival documentation, source code, configuration files, the book you are reading right now. The UEFI firmware already knows how to read FAT32 -- every UEFI implementation must include a FAT driver to load boot binaries. We just need to ask it.

This chapter builds a filesystem abstraction layer: about 260 lines of code that hide the complexity of UEFI file access behind six clean functions. By the end, the rest of our code will call `fs_readdir()` and `fs_readfile()` and get back plain ASCII data, never touching UEFI protocols directly.

We will work through every function, every struct field, and every design decision. The filesystem layer is one of the most reused pieces of infrastructure in the entire workstation -- the file browser, the text editor, the compiler shim, and the banner display all depend on it. Getting it right now saves us pain later.

## The Protocol Chain

Getting from "we want to read a file" to actually reading it requires navigating a chain of three UEFI protocols. Each step discovers a protocol that leads to the next:

```
Our image handle
    -> Loaded Image Protocol -> DeviceHandle (the SD card partition)
        -> Simple File System Protocol -> OpenVolume -> Root directory handle
            -> Open, Read, Close (files and directories)
```

**Step 1: Where did I come from?** We have our `image_handle` from `efi_main` -- it is the handle for our running binary. We ask UEFI: "What device was I loaded from?" The Loaded Image Protocol answers this question. It contains a `DeviceHandle` field pointing to the partition we booted from.

**Step 2: Can I read files on this device?** We take that device handle and ask: "Does this device support file access?" The Simple File System Protocol says yes. It provides a single function, `OpenVolume`, which opens the root directory of the partition.

**Step 3: Navigate the filesystem.** From the root directory handle, we can open any file or directory by path using UEFI's file I/O methods: `Open`, `Read`, `GetInfo`, `Write`, and `Close`.

This three-step discovery is how UEFI decouples components. The firmware does not hand you a "filesystem object" at boot time. Instead, you chain through protocols, each one revealing the next. It is verbose, but it means any device that implements these protocols -- SD card, USB drive, NVMe -- works the same way.

Let us build this step by step.

## The Directory Entry

Before writing any filesystem code, we need to decide how we will represent files to the rest of our program. UEFI uses `CHAR16` (UTF-16) strings and variable-size structures. Our framebuffer renders 8-bit ASCII with an 8x16 bitmap font. We need a clean boundary between the two worlds.

Create `src/fs.h`:

```c
#ifndef FS_H
#define FS_H

#include "boot.h"

#define FS_MAX_NAME    128
#define FS_MAX_ENTRIES 256

/* A single directory entry (pre-converted to ASCII) */
struct fs_entry {
    char     name[FS_MAX_NAME];
    UINT64   size;
    UINT8    is_dir;
};
```

This is our directory entry structure. Three fields, each chosen deliberately:

- **`name`** is `char`, not `CHAR16`. We convert UEFI's UTF-16 filenames to ASCII at read time so the rest of our code never deals with wide strings. This conversion-at-the-boundary pattern is important. UEFI uses UTF-16 for all strings, but our UI code, our editor, and our compiler all use 8-bit ASCII. Rather than scatter `CHAR16`-to-`char` conversions throughout the browser and editor, we do it once when reading directory entries and never think about it again.

- **`size`** is `UINT64` -- the file size in bytes. UEFI reports file sizes as 64-bit values. For directories, this is typically 0. We store it directly without truncation because FAT32 supports files up to 4GB, and we want the full size for display in the file browser.

- **`is_dir`** is a `UINT8` boolean flag. One byte, not four -- struct packing matters when you have arrays of 256 of these.

`FS_MAX_NAME` at 128 bytes covers the vast majority of real-world filenames. FAT32 long filenames can theoretically reach 255 characters, but in practice survival documentation and source files have short names. `FS_MAX_ENTRIES` at 256 limits directory listings to a reasonable size for an embedded system -- 256 entries times the size of `fs_entry` is about 34KB of stack or heap, well within our budget.

## The Function Signatures

```c
/* Initialize filesystem -- opens the boot volume.
   Call after g_boot is set up. */
EFI_STATUS fs_init(void);

/* Read directory contents into caller-provided array.
   path is CHAR16, e.g. L"\\" for root or L"\\EFI\\BOOT".
   Returns number of entries, or -1 on error.
   Entries are sorted: directories first, then alphabetical. */
int fs_readdir(const CHAR16 *path, struct fs_entry *entries, int max_entries);

/* Read entire file into a newly allocated buffer.
   Caller must mem_free() the returned pointer.
   Sets *out_size to file size. Returns NULL on error. */
void *fs_readfile(const CHAR16 *path, UINTN *out_size);

/* Write data to a file, creating or replacing it. */
EFI_STATUS fs_writefile(const CHAR16 *path, const void *data, UINTN size);

/* Get volume size and free space in bytes.
   Returns 0 on success, -1 on error. */
int fs_volume_info(UINT64 *total_bytes, UINT64 *free_bytes);

/* Get file size in bytes. Returns 0 if file doesn't exist. */
UINT64 fs_file_size(const CHAR16 *path);

/* Check if a file or directory exists. Returns 1 if yes, 0 if no. */
int fs_exists(const CHAR16 *path);

/* Rename a file. new_name is just the filename, not a full path. */
EFI_STATUS fs_rename(const CHAR16 *path, const CHAR16 *new_name);

#endif /* FS_H */
```

Eight functions. The first three do the heavy lifting -- initialize, list a directory, and read a file. `fs_writefile` and `fs_rename` handle the write path (we implement them in Chapters 11 and 10 respectively, when we need them). The remaining three are convenience wrappers that reuse the same UEFI protocol calls to answer common questions.

`fs_readdir` takes a `CHAR16` path because UEFI requires it. Paths use backslash separators: `L"\\"` for the root, `L"\\EFI\\BOOT"` for nested directories. It returns the entry count, or -1 on error. The caller provides the array and its maximum size -- we do not allocate internally.

`fs_readfile` returns a dynamically allocated buffer that the caller must `mem_free()`. The alternative -- making the caller pass a pre-sized buffer -- would require knowing the file size in advance, which means two calls instead of one. The returned-buffer pattern is simpler at the call site.

`fs_volume_info`, `fs_file_size`, and `fs_exists` round out the read-side API. They answer the questions every file manager needs: "How much space is left?", "How big is this file?", and "Does this path exist?" We implement them after the core three.

## Character Conversion

Now the implementation. Create `src/fs.c`:

```c
#include "fs.h"
#include "mem.h"

/* Root directory handle for the boot volume */
static EFI_FILE_HANDLE s_root;
static EFI_FILE_HANDLE s_boot_root;  /* saved boot volume root */
```

Two module-level statics. `s_root` is the root directory handle that every filesystem operation uses. Once `fs_init` opens it, all subsequent calls -- `fs_readdir`, `fs_readfile`, and every other `fs_*` function -- navigate from this root. `s_boot_root` saves a copy so we can restore it later if we ever switch to a different volume.

First, a helper to convert UEFI filenames to ASCII:

```c
/* Convert CHAR16 string to ASCII (truncates to 7-bit) */
static void char16_to_ascii(const CHAR16 *src, char *dst, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}
```

We truncate each 16-bit character to 7 bits with `& 0x7F`. This is deliberate lossy conversion. FAT32 filenames are almost always ASCII-compatible -- the FAT specification was designed in an era when filenames were 8.3 uppercase ASCII. Long filename support (VFAT) allows Unicode, but our files will have plain English names like `README.TXT`, `main.c`, and `chapter-09.md`.

Characters outside the ASCII range (accented letters, CJK characters) become garbage under this conversion, but that is acceptable. We are building a survival workstation, not a Unicode text processor. The simplicity of single-byte strings throughout our codebase -- the editor, the browser, the compiler -- is worth the tradeoff.

The function writes at most `max - 1` characters and always null-terminates. This prevents buffer overflows when filenames are longer than `FS_MAX_NAME`.

## Sorting

We want directory listings sorted -- directories first, then files alphabetically within each group. FAT32 is case-insensitive, so our comparison must be too:

```c
/* Case-insensitive ASCII comparison for sorting */
static int name_cmp(const char *a, const char *b) {
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

We lowercase by adding 32 -- the distance between `'A'` (65) and `'a'` (97) in ASCII. We cannot call `tolower()` from the C library because we do not have one. Everything in this codebase is built from scratch.

The cast to `(UINT8)` before the subtraction ensures unsigned comparison. Without it, characters above 127 would be treated as negative values, producing wrong sort order for any non-ASCII bytes that survived our conversion.

Now the sort itself:

```c
/* Sort entries: directories first, then alphabetical */
static void sort_entries(struct fs_entry *entries, int count) {
    /* Simple insertion sort -- fine for <= 256 entries */
    for (int i = 1; i < count; i++) {
        struct fs_entry tmp;
        mem_copy(&tmp, &entries[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0) {
            int swap = 0;
            if (tmp.is_dir && !entries[j].is_dir) {
                swap = 1;
            } else if (tmp.is_dir == entries[j].is_dir) {
                if (name_cmp(tmp.name, entries[j].name) < 0)
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

Insertion sort with a two-level ordering. The `swap` logic implements the priority: if the current element is a directory and the compared element is not, it sorts earlier. If both are the same type (both directories or both files), alphabetical order breaks the tie.

Insertion sort is O(n^2), which sounds bad, but for n <= 256 it is faster than any O(n log n) algorithm due to tiny constant factor and excellent cache behavior. The entire array fits in L1 cache. Quicksort's recursive overhead and cache-unfriendly access patterns make it slower at this scale.

We use `mem_copy` for struct moves instead of direct assignment (`tmp = entries[i]`). GCC under `-ffreestanding` sometimes generates hidden `memcpy` calls for struct copies, and we do not have a `memcpy` linked into our binary at this point -- that comes later with the shim layer in Chapter 13. Using our own `mem_copy` avoids the dependency.

## Opening the Boot Volume

Now the initialization function -- the protocol chain from earlier, implemented:

```c
EFI_STATUS fs_init(void) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;

    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
```

We store the GUIDs as local variables because `HandleProtocol` takes their addresses. GUIDs in UEFI are 128-bit identifiers that uniquely name each protocol. `EFI_LOADED_IMAGE_PROTOCOL_GUID` and `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID` are defined by gnu-efi's headers as constant structs.

```c
    /* Get the loaded image protocol to find our boot device */
    status = g_boot.bs->HandleProtocol(
        g_boot.image_handle, &li_guid, (void **)&loaded_image);
    if (EFI_ERROR(status))
        return status;
```

**Step 1.** Ask UEFI for the Loaded Image protocol associated with our image handle. `HandleProtocol` is the fundamental UEFI operation: given a handle and a protocol GUID, return a pointer to that protocol's interface structure. The `EFI_LOADED_IMAGE` structure contains information about the currently running binary -- where it was loaded from, its size, its command line. We care about one field: `DeviceHandle`, which identifies the partition containing our boot binary.

```c
    /* Get the filesystem protocol from the boot device */
    status = g_boot.bs->HandleProtocol(
        loaded_image->DeviceHandle, &sfs_guid, (void **)&sfs);
    if (EFI_ERROR(status))
        return status;
```

**Step 2.** Ask the boot device for its filesystem protocol. `loaded_image->DeviceHandle` is the SD card partition. We request the Simple File System protocol -- effectively asking: "I want to access files on this device." The SFS protocol has a single method: `OpenVolume`.

```c
    /* Open the root directory */
    status = sfs->OpenVolume(sfs, &s_root);
    if (!EFI_ERROR(status))
        s_boot_root = s_root;
    return status;
}
```

**Step 3.** Open the volume. `OpenVolume` gives us a file handle pointing to the root directory of the FAT32 partition. We save it in both `s_root` (the active root for all operations) and `s_boot_root` (a permanent copy that never changes).

From this point on, every file operation starts from `s_root`. To open `\EFI\BOOT\BOOTAA64.EFI`, we call `s_root->Open(s_root, &file, L"\\EFI\\BOOT\\BOOTAA64.EFI", ...)`. The firmware handles all the FAT32 cluster chain traversal, directory parsing, and sector reads internally. We just provide paths and get data back.

Three `HandleProtocol` calls and one `OpenVolume` -- that is the entire initialization. About 20 lines of code to go from "I am a running binary" to "I can read any file on my boot device."

## Reading a Directory

```c
int fs_readdir(const CHAR16 *path, struct fs_entry *entries, int max_entries) {
    EFI_STATUS status;
    EFI_FILE_HANDLE dir = NULL;
    int count = 0;

    /* Open the directory */
    status = s_root->Open(s_root, &dir, (CHAR16 *)path,
                          EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status))
        return -1;
```

Open the path relative to root. The cast `(CHAR16 *)path` discards `const` because UEFI's `Open` function does not use `const` in its signature -- a common API annoyance in the UEFI specification. The path will not be modified, but the type system does not know that.

`EFI_FILE_MODE_READ` opens the directory for reading. The final `0` is the attributes parameter, which is only meaningful when creating files.

```c
    /* Allocate scratch buffer for variable-size EFI_FILE_INFO */
    UINTN buf_size = 1024;
    void *buf = mem_alloc(buf_size);
    if (!buf) {
        dir->Close(dir);
        return -1;
    }
```

Directory entries in UEFI are variable-size `EFI_FILE_INFO` structures. The structure has fixed fields (size, create time, modify time, file size, attributes) followed by a `CHAR16` filename that extends past the end. A file named `README.TXT` makes the structure 10 characters (20 bytes) larger in the filename field than a file named `A`. We allocate a 1024-byte scratch buffer, generous enough for filenames up to about 450 characters.

Why not allocate on the stack? Because `EFI_FILE_INFO` is large (the fixed fields alone are over 80 bytes), and we need room for long filenames. A 1KB heap allocation is cheap and avoids stack overflow risk.

```c
    /* Read entries one at a time */
    for (;;) {
        UINTN read_size = buf_size;
        status = dir->Read(dir, &read_size, buf);

        if (EFI_ERROR(status))
            break;
        if (read_size == 0)
            break;  /* no more entries */
```

Each `Read()` call on a directory handle returns one entry. When there are no more entries, `Read()` succeeds but sets `read_size` to 0. This is UEFI's way of signaling end-of-directory -- no special return code, just a zero size.

We reset `read_size = buf_size` on every iteration. This is critical. `Read()` modifies `read_size` to reflect the actual bytes returned. If we forgot to reset it, the second iteration would use the first entry's size as the buffer limit. If the second entry happened to be larger, `Read()` would fail with `EFI_BUFFER_TOO_SMALL`. Resetting to the full buffer size on each call prevents this.

```c
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;

        /* Skip "." and ".." */
        if (info->FileName[0] == L'.' &&
            (info->FileName[1] == 0 ||
             (info->FileName[1] == L'.' && info->FileName[2] == 0)))
            continue;
```

Skip `.` (current directory) and `..` (parent directory). Every FAT32 directory contains these two entries. We handle "go up" through navigation keys in the file browser instead -- showing `.` and `..` in the listing would waste two slots and confuse the sorting.

The check is careful: `.` is a single dot followed by null. `..` is two dots followed by null. We cannot just check the first character, because a file named `.hidden` should not be skipped.

```c
        if (count >= max_entries)
            break;

        char16_to_ascii(info->FileName, entries[count].name, FS_MAX_NAME);
        entries[count].size = info->FileSize;
        entries[count].is_dir = (info->Attribute & EFI_FILE_DIRECTORY) ? 1 : 0;
        count++;
    }

    mem_free(buf);
    dir->Close(dir);

    sort_entries(entries, count);
    return count;
}
```

For each entry, we convert the filename from `CHAR16` to ASCII using our helper, copy the file size directly, and check the `EFI_FILE_DIRECTORY` attribute flag to determine if this entry is a directory. Then increment the count and continue.

After the loop finishes, we free the scratch buffer and close the directory handle. Resource cleanup is not optional -- UEFI firmware has a finite pool of file handles, and leaking them eventually causes `Open` calls to fail.

Finally, we sort the entries (directories first, then alphabetical) and return the count. The caller now has a clean, sorted array of ASCII entries ready for display.

## Reading a File

```c
void *fs_readfile(const CHAR16 *path, UINTN *out_size) {
    EFI_STATUS status;
    EFI_FILE_HANDLE file = NULL;

    *out_size = 0;

    /* Guard against uninitialized filesystem */
    if (!s_root)
        return NULL;

    /* Open the file */
    status = s_root->Open(s_root, &file, (CHAR16 *)path,
                          EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status))
        return NULL;
```

Open the file for reading. We initialize `*out_size = 0` early so the caller gets a sensible value even on error paths. The `s_root` null check guards against calling `fs_readfile` before `fs_init` -- without it, dereferencing a null root handle would crash the firmware.

```c
    /* Get file size via GetInfo */
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    EFI_FILE_INFO *info = NULL;

    /* First call to get required buffer size */
    file->GetInfo(file, &info_guid, &info_size, NULL);
    info = (EFI_FILE_INFO *)mem_alloc(info_size);
    if (!info) {
        file->Close(file);
        return NULL;
    }
```

To read a file, we first need to know its size so we can allocate the right buffer. `GetInfo` uses the UEFI **two-call pattern**, which deserves its own explanation because it appears throughout the UEFI specification.

### The Two-Call GetInfo Pattern

Many UEFI functions return variable-size data. The caller does not know the size in advance -- it depends on the filename length, the volume label, or other runtime factors. UEFI handles this with a two-call pattern:

1. **First call** with `info_size = 0` and `buffer = NULL`. This deliberately fails with `EFI_BUFFER_TOO_SMALL`, but as a side effect, it sets `info_size` to the exact number of bytes needed.
2. **Allocate** a buffer of that size.
3. **Second call** with the properly-sized buffer. This time it succeeds and fills in the data.

This pattern avoids two bad alternatives: fixed-size buffers (which might be too small) and oversized buffers (which waste memory). It is verbose, but it is safe. We will see this pattern again in `fs_volume_info` and `fs_file_size` below, and later in `fs_rename` (Chapter 10).

Now the second call:

```c
    status = file->GetInfo(file, &info_guid, &info_size, info);
    if (EFI_ERROR(status)) {
        mem_free(info);
        file->Close(file);
        return NULL;
    }

    UINT64 file_size = info->FileSize;
    mem_free(info);
```

The second call fills the buffer with the `EFI_FILE_INFO` structure. We extract `FileSize` and immediately free the info structure -- we only needed that one field. Keeping the allocation alive any longer than necessary would waste pool memory.

```c
    if (file_size == 0) {
        file->Close(file);
        return NULL;
    }

    /* Allocate buffer and read file */
    void *data = mem_alloc((UINTN)file_size);
    if (!data) {
        file->Close(file);
        return NULL;
    }

    UINTN read_size = (UINTN)file_size;
    status = file->Read(file, &read_size, data);
    file->Close(file);

    if (EFI_ERROR(status)) {
        mem_free(data);
        return NULL;
    }

    *out_size = read_size;
    return data;
}
```

Allocate a buffer of exactly the file size, read the entire file in one call, and close the handle. The `Read` call on a file handle (as opposed to a directory handle) reads raw bytes. We pass the full file size as the requested read count, and UEFI reads everything in one shot. For larger files, the firmware handles reading across multiple FAT32 clusters internally.

Note the careful error handling: every error path closes the file handle and frees any allocated memory. Resource leaks in a UEFI application exhaust the firmware's pool, eventually causing allocation failures that are extremely difficult to debug -- the symptom is `mem_alloc` returning NULL in an unrelated function, far from the leak.

The function returns the allocated buffer to the caller, who is responsible for calling `mem_free()` when done. This ownership transfer is documented in the header comment. It is the simplest possible API: give me a path, I give you the contents.

## Volume and File Queries

With the core functions in place, we add the utility functions declared in our header. These all follow patterns we have already seen -- `GetInfo`, `Open`, `Close` -- just applied to different questions.

### Querying Disk Space

```c
int fs_volume_info(UINT64 *total_bytes, UINT64 *free_bytes) {
    if (!s_root) return -1;

    EFI_GUID fsi_guid = EFI_FILE_SYSTEM_INFO_ID;
    UINTN buf_size = 0;

    /* First call to get required size */
    s_root->GetInfo(s_root, &fsi_guid, &buf_size, NULL);
    if (buf_size == 0) return -1;

    EFI_FILE_SYSTEM_INFO *info = (EFI_FILE_SYSTEM_INFO *)mem_alloc(buf_size);
    if (!info) return -1;

    EFI_STATUS status = s_root->GetInfo(s_root, &fsi_guid, &buf_size, info);
    if (EFI_ERROR(status)) {
        mem_free(info);
        return -1;
    }

    *total_bytes = info->VolumeSize;
    *free_bytes = info->FreeSpace;
    mem_free(info);
    return 0;
}
```

Same two-call `GetInfo` pattern as in `fs_readfile`, but with a different GUID. Instead of `EFI_FILE_INFO_ID` (which queries a specific file), we use `EFI_FILE_SYSTEM_INFO_ID` to query the volume itself. And instead of calling `GetInfo` on a file handle, we call it on `s_root` -- the root directory handle is also a volume handle.

The `EFI_FILE_SYSTEM_INFO` structure contains the volume label, total size, free space, block size, and read-only flag. We extract `VolumeSize` and `FreeSpace` as raw byte counts. Callers convert to megabytes for display by dividing by `1024 * 1024`.

This function is used by the boot banner (Chapter 8) to show available disk space right alongside platform and display info. It will also be used by the editor for checking available space before saves, and by the browser for paste operations. Returning raw byte counts keeps the function generic -- each caller formats the numbers for its own display context.

### Getting File Size

```c
UINT64 fs_file_size(const CHAR16 *path) {
    if (!s_root) return 0;

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = s_root->Open(s_root, &file, (CHAR16 *)path,
                                      EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return 0;

    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    file->GetInfo(file, &info_guid, &info_size, NULL);
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)mem_alloc(info_size);
    if (!info) { file->Close(file); return 0; }

    status = file->GetInfo(file, &info_guid, &info_size, info);
    UINT64 size = EFI_ERROR(status) ? 0 : info->FileSize;
    mem_free(info);
    file->Close(file);
    return size;
}
```

This is essentially the first half of `fs_readfile` -- open the file, get its info, extract `FileSize` -- without the second half that actually reads the data. Open, query, clean up.

It returns 0 for non-existent files because `Open` fails and we return early. This is the same return value as an empty file, which is fine for the "check available space" use case where 0 bytes means "nothing to worry about" either way.

### Checking Existence

```c
int fs_exists(const CHAR16 *path) {
    if (!s_root) return 0;
    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = s_root->Open(s_root, &file, (CHAR16 *)path,
                                      EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return 0;
    file->Close(file);
    return 1;
}
```

The simplest possible existence check -- try to open the path, close immediately. If `Open` succeeds, the path exists; if it fails, it does not. This works for both files and directories, since UEFI's `Open` handles both.

Five lines of actual logic. Sometimes the best code is the code that does the least.

The remaining two functions declared in `fs.h` -- `fs_writefile` and `fs_rename` -- are covered in Chapter 11 (Writing to Disk) and Chapter 10 (Browsing Files) respectively, where they are introduced alongside the features that need them.

## Adding str_cmp to mem.c

The sort function above uses `name_cmp` for case-insensitive comparison within the filesystem module. But the rest of the workstation also needs a general-purpose string comparison. We add it to the memory utilities.

In `src/mem.h`, add the declaration:

```c
int str_cmp(const CHAR8 *a, const CHAR8 *b);
```

And in `src/mem.c`, the implementation:

```c
int str_cmp(const CHAR8 *a, const CHAR8 *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(UINT8)*a - (int)(UINT8)*b;
}
```

Case-sensitive this time -- standard `strcmp` semantics. Returns 0 if equal, negative if `a < b`, positive if `a > b`. The loop advances both pointers while characters match and neither string has ended. When it stops, the difference between the current characters gives the comparison result.

This goes in `mem.c` rather than `fs.c` because it is a general utility. The file browser uses it to compare file extensions, the editor uses it to compare filenames, and later the compiler shim will need it too. Keeping string utilities alongside memory utilities in `mem.c` gives us a single place to find all the basic operations that the C standard library would normally provide.

## Wiring Into main.c

The filesystem needs to be initialized before anything tries to read files. In `efi_main`, we call `fs_init()` right after `mem_init()`:

```c
/* Initialize subsystems */
mem_init();
fs_init();

status = fb_init();
```

Order matters. `mem_init` must come first because `fs_init` uses `mem_alloc` indirectly (through UEFI protocol calls that may trigger allocations). `fs_init` comes before `fb_init` because the framebuffer banner wants to display disk space information via `fs_volume_info`.

The framebuffer main loop launches the file browser after showing the banner:

```c
#include "browse.h"

static void fb_loop(void) {
    print_banner_fb();

    struct key_event ev;
    kbd_wait(&ev);

    browse_run();
}
```

The banner now includes disk information from `fs_volume_info`:

```c
/* In print_banner_fb() -- after platform and display info */
UINT64 total_bytes, free_bytes;
if (fs_volume_info(&total_bytes, &free_bytes) == 0) {
    UINT32 total_mb = (UINT32)(total_bytes / (1024 * 1024));
    UINT32 free_mb = (UINT32)(free_bytes / (1024 * 1024));
    fb_print("  Disk:     ", COLOR_GRAY);
    uint_to_str(free_mb, num); fb_print(num, COLOR_GRAY);
    fb_print(" MB free / ", COLOR_GRAY);
    uint_to_str(total_mb, num); fb_print(num, COLOR_GRAY);
    fb_print(" MB total\n", COLOR_GRAY);
}
```

This gives the user immediate feedback about storage capacity -- useful information for a survival system where you need to know how much space remains for documents and source code.

And we add `fs.c` and `browse.c` to the Makefile's `SOURCES` list. The Phase 2 sources now include everything from Phase 1 plus the filesystem and browser:

```makefile
SOURCES  := $(SRCDIR)/main.c $(SRCDIR)/fb.c $(SRCDIR)/kbd.c \
            $(SRCDIR)/mem.c $(SRCDIR)/font.c \
            $(SRCDIR)/fs.c $(SRCDIR)/browse.c
```

## What We Built

Six functions implemented so far -- `fs_init`, `fs_readdir`, `fs_readfile`, `fs_volume_info`, `fs_file_size`, and `fs_exists` -- plus two internal helpers (`char16_to_ascii`, `name_cmp`, `sort_entries`). Together they encapsulate all the complexity of UEFI file access: the three-step protocol chain, variable-size structures, the two-call `GetInfo` pattern, character conversion, sorting, and careful resource cleanup on every error path. The remaining two functions declared in our header (`fs_writefile` and `fs_rename`) arrive in the next two chapters when we need them.

The rest of our workstation never needs to know about `HandleProtocol`, `EFI_LOADED_IMAGE`, `CHAR16` strings, or `EFI_FILE_INFO` structures. It calls `fs_readdir()` and gets back a sorted array of ASCII entries. It calls `fs_readfile()` and gets back a buffer of bytes. It asks `fs_volume_info()`, `fs_file_size()`, and `fs_exists()` for quick answers about the filesystem state.

This clean abstraction is not just aesthetic. When we later add the compiler shim in Chapter 13, TCC's `open()` call will use `fs_readfile` to load header files. When the editor saves in Chapter 11, it will use `fs_writefile`. When the browser pastes files in Chapter 10, it will use `fs_exists` to check for conflicts. Every feature we build from here on stands on this filesystem layer.

Next: building the interactive file browser UI on top of it.
