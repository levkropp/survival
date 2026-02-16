---
layout: default
title: "Chapter 22: Streaming I/O"
parent: "Phase 5: Streaming I/O & Tools"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 1
---

# Chapter 22: Streaming I/O

## The Size Wall

Every file operation in the workstation is atomic. `fs_readfile()` allocates a buffer the size of the entire file, reads every byte into it, and hands it back. `fs_writefile()` takes a buffer and writes every byte to disk in one call. This is simple and correct, and it has worked for twenty-one chapters because every file in the workstation is small. The largest is `libtcc.c` at about 60 KB. Even a generous source tree with every header and tool fits comfortably in a few hundred kilobytes.

A Linux ISO is 3 GB.

We cannot allocate 3 GB of RAM. The workstation runs on machines with 1 or 2 GB of total memory, and UEFI's memory allocator does not guarantee contiguous blocks of that size anyway. To write an ISO to a block device, we need to read it in small pieces -- a megabyte at a time, say -- and write each piece before reading the next. This is streaming I/O: open, read a chunk, process it, read the next chunk, close.

UEFI already supports this. The `Read()` method on `EFI_FILE_HANDLE` advances an internal file position on each call. Our `fs_readfile()` just happened to request the entire file in one `Read()` call. To stream, we call `Read()` multiple times with a smaller buffer. The protocol was always capable; we simply never asked.

This chapter adds seven functions to `fs.c` and one to `disk.c`. They are plumbing -- thin wrappers that normalize the interface and handle the details callers should not have to think about. Chapter 23 will use this plumbing to build the ISO writer.

## The Streaming API

The new functions live in `fs.h`, between the existing file operations and the USB volume discovery section:

```c
/* ---- Streaming file I/O ---- */
EFI_FILE_HANDLE fs_open_read(EFI_FILE_HANDLE root, const CHAR16 *path, UINT64 *out_size);
EFI_FILE_HANDLE fs_open_write(EFI_FILE_HANDLE root, const CHAR16 *path);
int fs_stream_read(EFI_FILE_HANDLE file, void *buf, UINTN *size);
int fs_stream_write(EFI_FILE_HANDLE file, const void *buf, UINTN size);
void fs_stream_close(EFI_FILE_HANDLE file);
EFI_STATUS fs_delete_file(EFI_FILE_HANDLE root, const CHAR16 *path);
EFI_FILE_HANDLE fs_get_boot_root(void);
```

Seven functions, about 75 lines of implementation. Each one does exactly one thing.

## Opening Files for Streaming

### fs_open_read

```c
EFI_FILE_HANDLE fs_open_read(EFI_FILE_HANDLE root, const CHAR16 *path, UINT64 *out_size) {
    if (!root) root = s_root;
    if (!root) return NULL;

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = root->Open(root, &file, (CHAR16 *)path,
                                    EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return NULL;

    /* Get file size */
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    file->GetInfo(file, &info_guid, &info_size, NULL);
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)mem_alloc(info_size);
    if (!info) { file->Close(file); return NULL; }

    status = file->GetInfo(file, &info_guid, &info_size, info);
    if (EFI_ERROR(status)) { mem_free(info); file->Close(file); return NULL; }

    *out_size = info->FileSize;
    mem_free(info);
    return file;
}
```

The `root` parameter is the key design decision. Every existing filesystem function in `fs.c` operates through the global `s_root` handle -- whichever volume is currently active. That works for the editor and file browser, which only touch one volume at a time. But the ISO writer needs to read from a USB volume while writing to a block device, and may need to create a temporary file on the boot volume simultaneously. Three volumes, one operation.

Passing `root` explicitly lets the caller specify which volume to open the file on. Pass `NULL` to get the current volume (the common case). Pass a USB volume's root handle to read from that specific drive. Pass `fs_get_boot_root()` to access the boot volume regardless of which volume is currently active.

The two-call `GetInfo` pattern is familiar from `fs_readfile()` in Chapter 9. The first call passes NULL to learn how large the `EFI_FILE_INFO` structure needs to be (it varies because it includes the filename). The second call fills the buffer. We extract `FileSize`, free the info struct, and return the open handle.

The function returns the raw `EFI_FILE_HANDLE`, not a wrapper struct. The UEFI handle already maintains a read position, supports `Read()` and `Close()`, and is everything the caller needs. Adding a wrapper struct would be indirection for its own sake.

### fs_open_write

```c
EFI_FILE_HANDLE fs_open_write(EFI_FILE_HANDLE root, const CHAR16 *path) {
    if (!root) root = s_root;
    if (!root) return NULL;

    /* Delete existing file first */
    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = root->Open(root, &file, (CHAR16 *)path,
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(status)) {
        file->Delete(file);
        file = NULL;
    }

    /* Create fresh */
    status = root->Open(root, &file, (CHAR16 *)path,
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                         EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status)) return NULL;
    return file;
}
```

This uses the delete-and-recreate pattern from `fs_writefile()`. Open the existing file for write, delete it, then create a fresh file. The fresh file starts at size zero with the write position at offset zero -- exactly what streaming write needs.

A subtle UEFI behavior: `Delete()` also closes the handle. After `file->Delete(file)`, the handle is invalid. You must not call `Close()` after `Delete()`. This is why `file` is set to NULL before the second `Open` -- it is a new handle, not the old one.

If the file does not exist, the first `Open` fails, `Delete` is never called, and we fall through to the `CREATE` open. Either way, we end up with a fresh, empty file handle ready for streaming writes.

## Reading and Writing Chunks

```c
int fs_stream_read(EFI_FILE_HANDLE file, void *buf, UINTN *size) {
    EFI_STATUS status = file->Read(file, size, buf);
    return EFI_ERROR(status) ? -1 : 0;
}

int fs_stream_write(EFI_FILE_HANDLE file, const void *buf, UINTN size) {
    UINTN write_size = size;
    EFI_STATUS status = file->Write(file, &write_size, (void *)buf);
    return EFI_ERROR(status) ? -1 : 0;
}
```

These are almost embarrassingly thin. Each wraps a single UEFI call. Why bother?

Consistency. Every other filesystem function returns `int` (0 or -1) or `EFI_STATUS`. Callers should not have to remember that streaming reads check `EFI_STATUS` directly while everything else checks an `int`. These wrappers normalize the interface so every filesystem operation follows the same convention.

For `fs_stream_read`, the `size` parameter is in-out. You pass how many bytes you want. UEFI writes how many bytes it actually delivered. When it returns 0 bytes, you have reached EOF. The caller's read loop looks like this:

```c
UINTN chunk = CHUNK_SIZE;
while (chunk > 0) {
    chunk = CHUNK_SIZE;
    fs_stream_read(file, buf, &chunk);
    /* process chunk bytes in buf */
}
```

For `fs_stream_write`, we copy `size` into a local `write_size` because UEFI's `Write()` takes a pointer to `UINTN` -- it might modify the value to report how many bytes were actually written. We do not expose this to the caller because partial writes from UEFI firmware are rare enough to treat as errors in practice.

## Closing and Cleanup

```c
void fs_stream_close(EFI_FILE_HANDLE file) {
    if (file) {
        file->Flush(file);
        file->Close(file);
    }
}
```

Flush before close. This matters for writes. Without the explicit `Flush()`, the last chunk of data might still be sitting in a firmware buffer when we close the handle. On some UEFI implementations, `Close()` flushes implicitly. On others, it does not. The specification says the data "may" be flushed on close. We do not rely on "may."

```c
EFI_STATUS fs_delete_file(EFI_FILE_HANDLE root, const CHAR16 *path) {
    if (!root) root = s_root;
    if (!root) return EFI_NOT_READY;

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = root->Open(root, &file, (CHAR16 *)path,
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status)) return status;
    return file->Delete(file);  /* Delete also closes */
}
```

Opens a file then immediately deletes it. The ISO writer will use this to clean up temporary files on the boot volume. The comment is load-bearing: `Delete()` closes the handle as a side effect. Calling `Close()` after `Delete()` would operate on an invalid handle.

Like the open functions, `root` can be NULL (use current volume) or an explicit volume root.

```c
EFI_FILE_HANDLE fs_get_boot_root(void) {
    return s_boot_root;
}
```

One line. Returns the boot volume root handle that `fs_init()` saved during startup. The ISO writer needs this when creating temporary files on the boot volume -- it cannot use `s_root` because that might be pointing at a USB volume.

## Force-Writing to the Boot Device

The other half of this chapter's plumbing lives in `disk.c`. The existing `disk_write_blocks()` function has a safety check:

```c
int disk_write_blocks(struct disk_device *dev, UINT64 lba, UINT64 count, void *buf) {
    if (!dev || !dev->block_io || dev->is_boot_device)
        return -1;
    ...
```

It refuses to write to the boot device. This is correct. Accidentally overwriting the boot volume would destroy the workstation -- every source file, every tool, the compiler, the EFI binary. The safety check has been there since Chapter 19, and it has prevented exactly this catastrophe during development.

But ISO writing to the boot device is a deliberate, confirmed action. The user will have typed Y-E-S as three separate keystrokes in a confirmation dialog. We need a way to bypass the check for this specific case.

```c
int disk_write_blocks_force(struct disk_device *dev, UINT64 lba, UINT64 count, void *buf) {
    if (!dev || !dev->block_io)
        return -1;

    UINTN buf_size = (UINTN)(count * (UINT64)dev->block_size);
    EFI_STATUS status = dev->block_io->WriteBlocks(
        dev->block_io, dev->media_id, (EFI_LBA)lba, buf_size, buf);

    if (EFI_ERROR(status))
        return -1;

    dev->block_io->FlushBlocks(dev->block_io);
    return 0;
}
```

Identical to `disk_write_blocks` except the `dev->is_boot_device` check is removed. The function trusts its caller completely.

This is why it exists as a separate function rather than a `force` flag parameter on the original. Calling `disk_write_blocks_force` is a visible, searchable, auditable decision in the code. Grep the codebase for `_force` and you find every place that performs destructive writes. You cannot accidentally pass `force=true` -- you have to deliberately choose the dangerous function by name. The call site documents the intent.

## Why Not a Stream Abstraction?

There is no `stream_t` struct. No vtable. No abstraction layer that unifies file reads and block device writes behind a common interface.

This is deliberate. The ISO writer reads from a file and writes to a block device. These are fundamentally different operations. File reads go through the full UEFI storage stack: FAT32 filesystem driver, SimpleFileSystem protocol, BlockIO protocol underneath. Block writes go directly to BlockIO, bypassing the filesystem entirely -- they overwrite raw sectors, destroying whatever filesystem was there.

Wrapping both in a common `stream_read` / `stream_write` interface would hide this distinction without eliminating it. A caller who sees `stream_write(dest, buf, size)` might reasonably think they are writing to a file. They are not. They are destroying a partition table. The code should make that obvious, and raw `disk_write_blocks_force` calls make it obvious.

The streaming functions are thin wrappers, not an abstraction. They normalize return types and handle flush-before-close, but they do not pretend that reading a FAT32 file and writing raw blocks are the same operation.

## What We Built

Seven functions in `fs.c`, one in `disk.c`. About 80 lines of code total. No new data structures, no new abstractions, no new state.

- `fs_open_read` and `fs_open_write` open file handles for chunked access on any volume
- `fs_stream_read` and `fs_stream_write` normalize UEFI's native streaming calls
- `fs_stream_close` ensures data reaches disk before the handle closes
- `fs_delete_file` cleans up temporary files
- `fs_get_boot_root` gives callers access to the boot volume
- `disk_write_blocks_force` permits confirmed destructive writes to the boot device

These are the pipes and valves that Chapter 23 will connect. The ISO writer will open an ISO file with `fs_open_read`, read it one megabyte at a time with `fs_stream_read`, and write each megabyte to a block device with `disk_write_blocks_force`. The data flows through without ever needing 3 GB of RAM -- just one buffer, reused over and over, chunk by chunk.
