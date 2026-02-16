---
layout: default
title: "Chapter 11: Writing to Disk"
parent: "Phase 2: Filesystem & Editor"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 3
---

# Chapter 11: Writing to Disk

## The Missing Half

Up to this point, the workstation can read files, browse directories, and display text. But it cannot change anything. Every file on the SD card is frozen the moment you boot. You can write code in the editor, stare at it on screen, and then lose it all when power is cut. That is not a tool. That is a terminal for viewing.

Think about what that means in practice. You boot the workstation, open the editor, and type fifty lines of notes about a problem you are solving. You press Escape to exit. The notes are gone. There is no save. There is no persistence. The editor is a toy that forgets everything you give it.

The filesystem protocol we use — UEFI's Simple File System — supports writing. We have been using only its read half. The SD card is formatted FAT32, and FAT32 is a read-write filesystem. UEFI's firmware driver knows how to create files, write data, and update the FAT. We just need to call the right functions.

This chapter adds three things: a bounded string copy utility, a `fs_writefile` function that creates or replaces files, and the editor's save path that ties it all together. The total new code is around 60 lines. But those 60 lines transform the workstation from a viewer into a creator.

## str_copy: A Bounded Copy

Before we can save files, we need to copy filenames around safely. Our string utilities so far — `str_len` and `str_cmp` — do not include a bounded copy. We need one that will not overflow a destination buffer regardless of what the source contains.

Add the declaration to `src/mem.h`:

```c
void str_copy(char *dst, const char *src, UINTN max);
```

And the implementation in `src/mem.c`:

```c
void str_copy(char *dst, const char *src, UINTN max) {
    UINTN i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}
```

This is our replacement for the standard `strncpy`, but with one critical difference: it always null-terminates the result. The standard `strncpy` has a notorious quirk — if the source string is exactly `max` characters long, the destination is left without a terminator. That is a buffer overflow waiting to happen. Our version copies at most `max - 1` characters and unconditionally writes the null byte at the end. The function returns nothing because there is nothing useful to report: either the string fit or it was truncated.

The loop structure is deliberate. We check `i < max - 1` before checking `src[i]`, so we never read past the end of either the source or the destination. On every iteration, `i` is both the index into `dst` and the index into `src`, so no separate pointer arithmetic is needed.

## The Write API

Add one function declaration to `src/fs.h`:

```c
EFI_STATUS fs_writefile(const CHAR16 *path, const void *data, UINTN size);
```

The signature mirrors `fs_readfile` in shape — a CHAR16 path and a data buffer — but the direction is reversed. Instead of returning allocated memory, it takes a caller-provided buffer to write. The path is a UEFI-style string because that is what the firmware's file protocol expects: wide characters with backslash separators, like `L"\\notes\\todo.txt"`.

The return type is `EFI_STATUS` rather than a simple success/failure flag. UEFI defines specific status codes for different failure modes — `EFI_VOLUME_FULL`, `EFI_WRITE_PROTECTED`, `EFI_DEVICE_ERROR` — and the caller needs to distinguish between them. The editor will map these to different error messages in the status bar.

Note the `const` on both `path` and `data`. We promise not to modify either argument. UEFI's own API is not const-correct — we will need to cast these away inside the implementation — but our public interface should be honest about what it does.

## Delete and Recreate

The implementation of `fs_writefile` uses a pattern that may seem counterintuitive at first: it deletes the existing file and creates a new one from scratch. Here is the function, shown section by section with the reasoning behind each step.

### Opening for Deletion

```c
EFI_STATUS fs_writefile(const CHAR16 *path, const void *data, UINTN size) {
    EFI_STATUS status;
    EFI_FILE_HANDLE file = NULL;

    /* Try to delete existing file first */
    status = s_root->Open(s_root, &file, (CHAR16 *)path,
                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(status)) {
        file->Delete(file);  /* Delete also closes the handle */
        file = NULL;
    }
```

The first step opens the file with both `READ` and `WRITE` mode flags. We need write access because `Delete()` modifies the filesystem — it removes the directory entry and frees the file's clusters. If the file does not exist (this is the first save of a new file), `Open` returns an error status, the `if` block is skipped, and we fall through to creation.

There is a UEFI quirk here that is easy to miss. The `Delete()` function invalidates and closes the file handle. After calling `Delete()`, you cannot call `Close()` on the same handle — that would be a double-free. The UEFI specification defines `Delete()` this way: it removes the file and disposes of the handle in a single operation. That is why we set `file = NULL` immediately after the call. If we forget and later call `Close(file)`, the firmware may corrupt its internal state.

### Why Delete First

Why not just open the existing file and overwrite it? UEFI's `Write()` function writes bytes starting at the current file position and extends the file if the write goes past the end. But it does not truncate. If the old file was 1000 bytes and the new content is 500 bytes, `Write()` would replace the first 500 bytes and leave the remaining 500 stale bytes in place. The result: a corrupted file with old data appended to new data.

Some UEFI implementations support `SetInfo` to change the file size, but behavior varies across firmware vendors. Delete-and-recreate is portable and unambiguous. The new file contains exactly what we write, nothing more. There is no scenario where stale data leaks through.

### Creating Fresh

```c
    /* Create fresh file */
    status = s_root->Open(s_root, &file, (CHAR16 *)path,
                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                          EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status))
        return status;
```

The second `Open` call uses three mode flags. `EFI_FILE_MODE_READ` and `EFI_FILE_MODE_WRITE` request read-write access. `EFI_FILE_MODE_CREATE` tells the firmware to create the file if it does not already exist. Together, these give us a handle to a brand-new, zero-length file. If creation fails — perhaps the directory does not exist, or the volume is write-protected — we return the error to the caller immediately.

The UEFI specification requires that `CREATE` must always be combined with `READ | WRITE`. Attempting to open with `CREATE` alone or with only `READ | CREATE` is invalid and may produce an error on strict firmware implementations. This three-flag combination is the only correct way to create a new file.

### Writing

```c
    /* Write contents */
    UINTN write_size = size;
    status = file->Write(file, &write_size, (void *)data);
    if (EFI_ERROR(status)) {
        file->Close(file);
        return status;
    }
```

The `Write()` call takes a pointer to the byte count, not the count itself. This is because the `size` parameter is in-out: on entry it specifies how many bytes we want to write, and on return the firmware updates it to reflect how many bytes were actually written. In theory, a UEFI filesystem driver could perform a partial write — writing fewer bytes than requested if the volume fills up mid-operation. In practice, FAT32 drivers allocate all necessary clusters before writing and either succeed completely or fail completely. But the in-out convention exists for robustness.

The `(void *)` cast on `data` discards the `const` qualifier. UEFI's file protocol was designed before const-correctness was standard practice, so its `Write()` function takes a non-const `void *` even though it never modifies the buffer. This cast is safe.

If writing fails, we close the handle before returning. Even though the file is empty (or partially written), we must not leak the handle. UEFI firmware tracks open handles and may refuse to open new ones if too many are outstanding.

### Flushing and Closing

```c
    file->Flush(file);
    file->Close(file);
    return EFI_SUCCESS;
}
```

`Flush()` forces the firmware to write any cached data to the physical storage medium. Without this call, the data might live only in a write-back cache inside the firmware's filesystem driver. If power drops before the cache writes back — which is entirely possible on an SD card — the file could be empty, truncated, or contain stale data.

SD cards have their own internal write buffers and wear-leveling logic, so `Flush()` does not guarantee the data has reached flash cells. But it does guarantee the firmware has issued the write commands. That is the strongest guarantee we can get without a journaling filesystem.

`Close()` releases the file handle. After this call, the file exists on disk with exactly the bytes we wrote. The function returns `EFI_SUCCESS` to tell the caller the save succeeded.

### Error Handling Summary

Every error path in this function either returns before opening a handle or closes the handle before returning. There is no path where a handle leaks. If the delete succeeds but the create fails, we return the create error — the old file is gone and the caller knows the save failed. If the create succeeds but the write fails, we close the handle — the file exists but may be empty or incomplete. These are not ideal outcomes, but they are the honest truth about what happened, reported back to the caller via the status code.

## The Editor's Save Path

With `fs_writefile` in place, the editor can save files. The save operation involves three steps: serialize the document to a byte buffer, check that there is enough disk space, and write the buffer to disk. Here is `doc_save` from `src/edit.c`:

```c
static int doc_save(void) {
    UINTN size = 0;
    char *buf = doc_serialize(&size);
    if (!buf)
        return -1;

    /* Check disk space (account for existing file we'll replace) */
    UINT64 total_bytes, free_bytes;
    if (fs_volume_info(&total_bytes, &free_bytes) == 0) {
        UINT64 old_size = fs_file_size(s_filepath);
        if ((UINT64)size > free_bytes + old_size) {
            mem_free(buf);
            return -2;  /* out of space */
        }
    }

    EFI_STATUS status = fs_writefile(s_filepath, buf, size);
    mem_free(buf);

    if (EFI_ERROR(status))
        return -1;

    s_modified = 0;
    return 0;
}
```

`doc_serialize` walks every line in the editor's line buffer, concatenates them with newline separators, and returns a freshly allocated byte array. It calculates the total size first — the sum of all line lengths plus one newline between each pair of adjacent lines — then allocates a buffer of exactly that size and copies the data. If memory allocation fails, it returns NULL and we bail with -1. The buffer is owned by the caller; after `fs_writefile` returns, we free it immediately regardless of success or failure.

The disk space check uses `fs_volume_info` from Chapter 9, which queries UEFI's `EFI_FILE_SYSTEM_INFO` for total and free bytes. The critical detail is the `free_bytes + old_size` calculation. Since `fs_writefile` deletes the old file before creating the new one, the old file's clusters will be freed first. The true available space is the current free space plus the space the old file currently occupies. For a new file that does not exist yet, `fs_file_size` returns 0, which is correct — there is no old file to reclaim space from.

If the new content would not fit, we return -2 instead of -1. This distinct error code lets the caller show a specific message ("not enough disk space") rather than a generic failure.

After writing, if `fs_writefile` succeeds, we clear the `s_modified` flag. This flag controls the asterisk in the editor's title bar and the "unsaved changes" prompt when exiting. Clearing it tells the user that the on-screen content matches what is on disk.

The caller is `handle_save`, which maps error codes to status bar messages:

```c
static int handle_save(void) {
    int ret = doc_save();
    if (ret == 0) {
        draw_header();
        draw_info("Saved.");
        return 0;
    } else if (ret == -2) {
        draw_info("Error: not enough disk space!");
        return -1;
    } else {
        draw_info("Error: save failed!");
        return -1;
    }
}
```

Three outcomes, three messages. On success, we redraw the header (to remove the modified asterisk) and show "Saved." in the info line. On disk-full, we show a specific error. On any other failure — write protection, device error, permission issue — we show a generic error. The function returns 0 on success and -1 on failure, so the editor's exit prompt knows whether it can safely close.

The separation between `doc_save` and `handle_save` is deliberate. `doc_save` is a pure data operation — serialize, check space, write. It knows nothing about the screen. `handle_save` is a UI operation — it calls `doc_save` and translates the result into visual feedback. This separation matters because other parts of the editor also need to save: the exit handler calls `handle_save` when the user presses F2 from the "unsaved changes" prompt, and the compile-and-run handler (added in later chapters) auto-saves before compiling. Both callers go through `handle_save` and get the same error feedback.

## What Could Go Wrong

A few failure modes are worth understanding:

**Disk full.** `Write()` returns `EFI_VOLUME_FULL` if FAT32 runs out of clusters. The proactive check in `doc_save` catches this before we delete the old file — so the user gets a warning and keeps their existing data. Without the proactive check, the sequence would be: delete old file, attempt write, fail. The old file is gone and the new one did not land. On a multi-gigabyte SD card, this is unlikely for text files, but it matters when the card is nearly full or when copying large files through the browser.

**Write protection.** Some SD card adapters have a physical write-protect switch. If it is engaged, `Open` with WRITE mode returns `EFI_WRITE_PROTECTED`. The error propagates through `doc_save` to `handle_save`, which displays the generic "save failed" message. The user can check the switch and try again.

**Power loss during write.** FAT32 has no journaling. There is no transaction log, no atomic commit, no recovery mechanism. If power is lost between `Delete` and the end of `Write`, the old file is gone and the new file is incomplete — or missing entirely. Our delete-then-write strategy means there is a window where the data exists only in memory. This is an inherent limitation of FAT32 and a conscious trade-off for simplicity.

A journaling approach would write the new data to a temporary file, then atomically rename it over the old one. FAT32 does not support atomic rename, so even that strategy is imperfect. A more robust approach would require a log-structured filesystem, which is far beyond our scope. For a survival workstation writing text files to an SD card, the simplicity of delete-and-recreate is worth the risk. The alternative — implementing a journaling layer on top of UEFI's filesystem protocol — would add hundreds of lines of code for a failure mode that requires losing power at exactly the wrong millisecond.

## What We Built

Around 35 lines of code for `fs_writefile`, plus 25 lines for `doc_save` and `handle_save`, plus 7 lines for `str_copy`. That is the entirety of our write support. Combined with `fs_readfile` from Chapter 9 and `fs_readdir` from Chapter 8, we now have a complete read-write filesystem layer: enumerate directories, read files, and write files — the three operations a text editor needs.

The API surface is intentionally minimal. One function to write a file, one function to save the document, one function to handle the UI. No append mode, no partial writes, no file locking, no temporary files. Every one of those features would add complexity for scenarios that a single-user workstation does not face. If we need them later, we can add them later.

With this chapter, the workstation crosses a threshold. It is no longer a viewer. You can open a file, edit it, press F2, and the changes persist across reboots. The SD card is now a working storage medium, not just a boot device. Everything that follows — the compiler, the file browser's copy and paste, the self-hosting rebuild — depends on this ability to write.
