---
layout: default
title: "Chapter 23: The ISO Writer"
parent: "Phase 5: Streaming I/O & Tools"
nav_order: 2
---

# Chapter 23: The ISO Writer

## The Escape Hatch

The workstation can propagate itself. Press F12 on a USB drive and you get a bootable clone -- source code, compiler, editor, everything. But what if the survival scenario is not "preserve this workstation" but "escape it"? What if someone hands you a USB stick with a Linux ISO and says "install this on a spare drive"?

On a normal computer, you would use `dd` or Etcher or Rufus. But we are not on a normal computer. We are on a bare-metal UEFI application with no operating system. There is no `dd`. There is no shell. There is only the workstation.

So the workstation needs to become the tool. Navigate to the ISO file in the file browser, press F10, pick a target USB drive, confirm, and watch the bytes stream. When it finishes, the target USB is a bootable Linux installer. Reboot, select it, and you are running a full operating system.

This is the inverse of cloning. Chapter 20 copied the workstation TO a USB drive using file-level operations (FAT32 file copy). This chapter writes raw disk data FROM a file to a USB drive using block-level operations. Cloning propagates the workstation. ISO writing escapes it.

## Why Hybrid ISOs Just Work

A Linux ISO file is not just a CD-ROM image. Modern Linux distributions -- Ubuntu, Fedora, Alpine, Arch, and nearly every other -- produce "hybrid" ISOs. These contain both an ISO 9660 filesystem (the traditional CD/DVD format) and a valid GPT or MBR partition table with an embedded FAT32 EFI System Partition.

This dual nature means you can write the ISO directly to a USB drive and get a bootable disk. The UEFI firmware ignores the ISO 9660 structure entirely. It sees the GPT/MBR partition table, finds the EFI System Partition, locates `\EFI\BOOT\BOOTX64.EFI` (or `BOOTAA64.EFI`), and boots from it. The firmware does not know or care that the bytes also form a valid CD-ROM image.

The core operation is identical to `dd if=ubuntu.iso of=/dev/sdX`: read the file sequentially, write each chunk to consecutive blocks on the target device. No filesystem creation. No partition table manipulation. No bootloader installation. Just raw bytes, streamed from file to device, block by block.

## iso.h -- The API

The entire module exposes one function:

```c
int iso_write(EFI_FILE_HANDLE iso_root, const CHAR16 *iso_path,
              const char *iso_name, UINT64 iso_size,
              EFI_HANDLE iso_vol_handle);
```

Five parameters, each serving a specific purpose:

- `iso_root`: The volume root handle where the ISO lives. Passed explicitly because the ISO might be on a USB volume, not the current `s_root`.
- `iso_path`: CHAR16 path to the ISO file on that volume (e.g., `\alpine-virt-3.23.3-aarch64.iso`).
- `iso_name`: ASCII display name for the UI -- the filename shown to the user.
- `iso_size`: File size in bytes, known in advance from the directory listing.
- `iso_vol_handle`: The EFI_HANDLE of the volume containing the ISO. Used for same-device detection -- if the ISO lives on the same physical device as the write target, we need to copy it elsewhere first.

Returns 0 on success, -1 on error or cancel.

## iso.c -- The Module

About 300 lines organized into UI helpers, same-device detection, and the main write function.

### UI Helpers

```c
#define CHUNK_SIZE (64 * 1024)  /* 64KB streaming chunks */

static void iso_print(const char *msg, UINT32 color) {
    if (g_boot.framebuffer) fb_print(msg, color);
}
```

64KB chunks balance two concerns: large enough for reasonable throughput (fewer UEFI calls per megabyte), small enough to show smooth progress. Each chunk is one `Read()` call plus one `WriteBlocks()` call.

`iso_print` guards `fb_print` calls -- only print if we have a framebuffer. Without it, `fb_print` on a console-only system would write to a NULL pointer.

The progress display uses `fb_string()` to overwrite a fixed row instead of scrolling:

```c
static void show_progress(UINT64 written, UINT64 total, UINT32 row) {
    char line[128];
    UINT64 written_mb = written / (1024 * 1024);
    UINT64 total_mb = total / (1024 * 1024);
    int pct = total > 0 ? (int)((written * 100) / total) : 0;

    snprintf(line, sizeof(line), "  Writing: %llu MB / %llu MB (%d%%)",
             (unsigned long long)written_mb, (unsigned long long)total_mb, pct);

    /* Pad to full width to overwrite previous line */
    int len = (int)strlen(line);
    while (len < (int)g_boot.cols && len < 126) line[len++] = ' ';
    line[len] = '\0';

    fb_string(0, row, line, COLOR_WHITE, COLOR_BLACK);
}
```

The padding is important. Without it, a shorter string could leave stale characters from the previous line. If the percentage display shrinks from `(100%)` back to... well, it will not shrink in this case, but the padding costs nothing and prevents a class of bugs.

A second progress function handles the same-device temp copy, using yellow text to distinguish it from the main white write progress:

```c
static void show_copy_progress(UINT64 written, UINT64 total, UINT32 row) {
    /* Same structure as show_progress, but COLOR_YELLOW */
}
```

Yellow means "preparatory work." White means "the actual write." The user can tell at a glance which phase they are watching.

### Same-Device Detection

The trickiest problem in the ISO writer: what if the ISO file is on the same USB drive the user wants to write to? You cannot read from a device while simultaneously overwriting it with raw blocks. The first `WriteBlocks` call would corrupt the FAT32 filesystem that the ISO file lives on. Subsequent `Read` calls would return garbage.

```c
static int is_same_device(EFI_HANDLE vol_handle, struct disk_device *dev) {
    if (!vol_handle || !dev) return 0;

    /* Direct handle match (superfloppy / whole-disk filesystem) */
    if (vol_handle == dev->handle) return 1;

    /* Check if the ISO volume is a partition on the target device */
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO *vol_bio = NULL;
    EFI_STATUS status = g_boot.bs->HandleProtocol(
        vol_handle, &bio_guid, (VOID **)&vol_bio);
    if (!EFI_ERROR(status) && vol_bio && vol_bio->Media) {
        if (vol_bio->Media->LogicalPartition && dev->is_removable) {
            UINT64 vol_size = (UINT64)(vol_bio->Media->LastBlock + 1) *
                              (UINT64)vol_bio->Media->BlockSize;
            if (dev->size_bytes >= vol_size)
                return 1;
        }
    }

    return 0;
}
```

Two cases must be handled:

**Superfloppy**: The USB drive has no partition table -- the FAT32 filesystem occupies the entire device. The SimpleFileSystem handle IS the BlockIO handle. Direct comparison catches this.

**Partitioned**: The USB drive has a GPT or MBR partition table. The FAT32 filesystem lives on a partition, which gets its own UEFI handle separate from the whole-disk handle. The `BlockIO->Media->LogicalPartition` flag is true for partition handles. If the volume is a logical partition and the target device is removable and at least as large as the partition, they are likely the same physical disk.

This heuristic is not perfect. Two USB drives of the same size could cause a false positive. But the consequence of a false positive is an unnecessary temp copy -- safe, just slower. The consequence of a false negative is data corruption. The heuristic errs on the side of caution.

### The Main Function

`iso_write()` follows a clear linear flow: header, enumerate, select, detect same-device, temp copy if needed, confirm, stream write, cleanup, done.

**The header screen** clears the framebuffer and shows the ISO name and size:

```c
fb_clear(COLOR_BLACK);
iso_print("\n", COLOR_WHITE);
iso_print("  ========================================\n", COLOR_CYAN);
iso_print("       WRITE ISO TO DISK\n", COLOR_CYAN);
iso_print("  ========================================\n", COLOR_CYAN);
```

Followed by the ISO filename and size in MB. This is the same visual structure as the clone screen -- a cyan header bar, then contextual information.

**Device enumeration and display**:

```c
struct disk_device devs[DISK_MAX_DEVICES];
int ndevs = disk_enumerate(devs, DISK_MAX_DEVICES);
```

`disk_enumerate` returns every block device the firmware knows about: SD cards, USB drives, NVMe drives, virtual disks. Each one is displayed with color-coding that tells the user everything they need to know at a glance:

- **Gray** -- too small for the ISO. Skipped for selection.
- **Magenta** with `* ISO SOURCE` -- the device containing the ISO file. Writing here would be circular and pointless.
- **Red** -- the boot device. Writing here destroys the workstation. Allowed, but with extra confirmation.
- **Yellow** -- viable target. The normal case.

The default selection auto-picks the first viable non-boot, non-source, removable device. On a typical setup (boot SD card + ISO USB + target USB), the user just presses ENTER.

**The selection loop**:

```c
for (;;) {
    struct key_event ev;
    kbd_wait(&ev);
    if (ev.code == KEY_ESC) return -1;
    if (ev.code == KEY_ENTER) break;
    if (ev.code >= '1' && ev.code <= '0' + ndevs) {
        int idx = ev.code - '1';
        if (devs[idx].size_bytes >= iso_size) {
            target_idx = idx;
            /* Update selection indicator on display */
        }
    }
}
```

Number keys select a device. Only devices large enough for the ISO are accepted -- pressing "2" on a 256MB drive when the ISO is 3GB does nothing. ESC cancels and returns to the file browser.

**Same-device handling**: If `is_same_device()` returns true, the ISO must be copied to the boot volume before the write can begin:

1. Check boot volume free space. Is there room for a multi-gigabyte temp file?
2. Open the ISO for streaming read from the USB volume using `fs_open_read`.
3. Open `\__iso_temp__.iso` for streaming write on the boot volume using `fs_open_write`.
4. Stream-copy in 64KB chunks with yellow progress display.
5. Close both handles.
6. Re-open the temp file for reading -- this becomes the data source for the block write.

The temp file name `__iso_temp__.iso` uses double underscores to minimize collision with real files. Nobody names their ISO this way.

This is the only scenario where the ISO writer touches the boot volume's filesystem. In the normal case (ISO on one USB, target on another USB), the boot volume is never involved.

**Two-tier confirmation**: The confirmation step adapts to the target:

For non-boot devices, a single key: "Press Y to proceed." This is the same pattern used by the clone function in Chapter 20. You are overwriting a USB drive, which is destructive, but reversible -- you can reformat the drive later.

For the boot device, three separate keystrokes: "Type YES to confirm." Each character is echoed in red. Any wrong character cancels. This is the nuclear option -- it destroys the workstation's filesystem. The source code, the compiler, the editor, the boot binary. Everything. The extra friction is intentional. You have to really mean it.

```c
if (is_boot) {
    iso_print("  Type YES to confirm: ", COLOR_RED);
    const char expect[] = "YES";
    for (int i = 0; i < 3; i++) {
        kbd_wait(&ev);
        if (ev.code != expect[i] && ev.code != (expect[i] + 32)) {
            /* Wrong character -- cancel */
            ...
            return -1;
        }
        /* Echo the character in red */
    }
}
```

The `expect[i] + 32` handles lowercase -- the user can type "YES" or "yes" or "Yes" and any combination. But they must type exactly three correct characters in order.

**The streaming write loop** -- the core of the module:

```c
void *chunk = mem_alloc(CHUNK_SIZE);
UINT64 written = 0;
UINT64 lba = 0;
UINT32 block_size = target->block_size;

while (written < file_size) {
    UINTN to_read = CHUNK_SIZE;
    if (written + to_read > file_size)
        to_read = (UINTN)(file_size - written);

    if (fs_stream_read(read_handle, chunk, &to_read) < 0 || to_read == 0) {
        write_error = 1;
        break;
    }

    /* Pad last chunk to block boundary */
    UINTN padded = to_read;
    if (padded % block_size != 0) {
        UINTN pad_to = ((padded / block_size) + 1) * block_size;
        mem_set((char *)chunk + padded, 0, pad_to - padded);
        padded = pad_to;
    }

    UINT64 blocks = padded / block_size;
    int rc;
    if (is_boot)
        rc = disk_write_blocks_force(target, lba, blocks, chunk);
    else
        rc = disk_write_blocks(target, lba, blocks, chunk);

    if (rc < 0) {
        write_error = 1;
        break;
    }

    written += to_read;
    lba += blocks;
    show_progress(written, file_size, progress_row);
}
```

Walk through the key details:

First, read up to 64KB from the file. On the last chunk, `to_read` is clamped to the remaining bytes. `fs_stream_read` updates `to_read` with the actual number of bytes returned. If it returns 0 bytes, we have hit an unexpected EOF -- the file is shorter than `iso_size` claimed.

Second, block alignment. Block devices require writes aligned to `block_size`, typically 512 bytes. The last chunk of an ISO is almost never a perfect multiple of 512. The padding fills the remainder with zeros using `mem_set`, then rounds `padded` up to the next block boundary. Those trailing zeros occupy the last partial sector on the device -- harmless dead space.

Third, the write call branches on `is_boot`. `disk_write_blocks` refuses to touch the boot device (Chapter 22's safety check). `disk_write_blocks_force` bypasses that check. The `is_boot` flag was captured during device selection, and we only reach this code after the three-keystroke confirmation.

Fourth, progress updates on every chunk. At 64KB chunks and typical USB 2.0 write speeds (~10 MB/s), that is about 160 updates per second. The user sees smooth, continuous progress -- megabyte by megabyte, percentage point by percentage point.

**Cleanup**: After the loop, close the file handle and free the chunk buffer. If a temp file was created for same-device handling, delete it with `fs_delete_file`. Then `disk_reconnect(target)` forces the firmware to re-probe the target device -- the ISO write destroyed whatever filesystem was there, but UEFI may still have a stale SimpleFileSystem protocol cached from before the write. `disk_reconnect` calls `DisconnectController` to drop the stale driver, then `ConnectController` to let the firmware re-examine the device. With ISO 9660 data now at block zero instead of a FAT32 BPB, the FAT driver will not bind, and the device will have no SimpleFileSystem protocol. Then display the result:

On success, a green banner: "ISO WRITTEN SUCCESSFULLY". Below it, an important explanation: the target device is no longer a FAT32 volume. The raw ISO has replaced the FAT32 filesystem -- the device now contains whatever the ISO contained (typically ISO 9660 with an embedded EFI partition). When the user returns to the file browser, the target device will appear as a red `[DISK]` entry instead of an orange `[USB]` entry. Chapter 24 explains how these raw devices become visible and how the user can format them back to FAT32 when they are done with the ISO.

On failure, a red banner with the error. The partially-written device is in an undefined state -- neither valid ISO nor valid FAT32. The user should retry or reformat.

Either way, "Press any key to return" brings the user back to the file browser.

## browse.c -- F10 Integration

### ISO File Detection

The file browser needs to know when the cursor is on an ISO file so it can show the F10 option. A small helper at the top of browse.c:

```c
static int is_iso_file(const char *name) {
    int len = 0;
    while (name[len]) len++;
    if (len < 5) return 0;  /* need at least "x.iso" */
    char c1 = name[len - 4];
    char c2 = name[len - 3];
    char c3 = name[len - 2];
    char c4 = name[len - 1];
    if (c1 != '.') return 0;
    if (c2 != 'i' && c2 != 'I') return 0;
    if (c3 != 's' && c3 != 'S') return 0;
    if (c4 != 'o' && c4 != 'O') return 0;
    return 1;
}
```

Case-insensitive extension check. No `tolower()` available -- we are bare-metal, no libc -- so manual character comparison against both cases. The function is defined early in the file because `draw_status_msg()` references it.

### Dynamic Status Bar

The status bar is the interface contract. It tells the user what actions are available right now. When the cursor sits on an ISO file, F10:WriteISO appears. When it moves to a regular file or directory, F10 disappears.

```c
int on_iso = (s_count > 0 && s_cursor < s_count
              && !s_entries[s_cursor].is_dir
              && is_iso_file(s_entries[s_cursor].name));
if (s_on_usb) {
    if (on_iso)
        msg = " ENTER:Open F3:Copy F10:WriteISO F12:Clone BS:Back";
    else
        msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename F12:Clone BS:Back";
} else {
    if (on_iso)
        msg = " ENTER:Open F3:Copy F10:WriteISO BS:Back ESC:Exit";
    else
        msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename BS:Back ESC:Exit";
}
```

Four branches: USB vs. boot volume, ISO vs. non-ISO. On the ISO variants, some less-relevant operations (F4:New, F8:Paste, F9:Rename) are dropped to make room for F10:WriteISO. These operations still work -- their key handlers do not check the status bar -- but hiding them keeps the bar from overflowing on narrow screens and focuses the user's attention on the relevant action.

### The F10 Key Handler

```c
case KEY_F10:
    if (s_count > 0 && !s_entries[s_cursor].is_dir
        && is_iso_file(s_entries[s_cursor].name)) {
        /* Build full CHAR16 path to the ISO file */
        CHAR16 iso_path[MAX_PATH];
        int ip = 0;
        while (s_path[ip] && ip < MAX_PATH - 1) {
            iso_path[ip] = s_path[ip];
            ip++;
        }
        if (ip > 1 || iso_path[0] != L'\\')
            iso_path[ip++] = L'\\';
        int ij = 0;
        while (s_entries[s_cursor].name[ij] && ip < MAX_PATH - 1)
            iso_path[ip++] = (CHAR16)s_entries[s_cursor].name[ij++];
        iso_path[ip] = 0;

        /* Determine volume root and handle */
        EFI_FILE_HANDLE vol_root = NULL;
        EFI_HANDLE vol_handle = NULL;
        if (s_on_usb && s_usb_vol_idx >= 0 && s_usb_vol_idx < s_usb_count) {
            vol_root = s_usb_vols[s_usb_vol_idx].root;
            vol_handle = s_usb_vols[s_usb_vol_idx].handle;
        } else {
            vol_root = fs_get_boot_root();
            vol_handle = NULL; /* boot volume -- never same as target */
        }

        iso_write(vol_root, iso_path,
                  s_entries[s_cursor].name,
                  s_entries[s_cursor].size,
                  vol_handle);
        load_dir();
        draw_all();
    }
    break;
```

The path-building is the same pattern used throughout browse.c: concatenate the current directory path, a backslash separator, and the filename, widening each ASCII byte to CHAR16 with a cast. The `if (ip > 1 || iso_path[0] != L'\\')` guard prevents a double backslash at root.

The volume root and handle come from the USB state when browsing a USB volume, or from `fs_get_boot_root()` when on the boot volume. For boot-volume ISOs, `vol_handle` is NULL. This causes `is_same_device()` to return false, since writing a boot-volume ISO back to the boot device would be self-destructive rather than useful -- and the boot device already gets the three-keystroke confirmation gate regardless.

After `iso_write()` returns -- whether success, failure, or cancel -- `load_dir()` and `draw_all()` refresh the browser. The file browser picks up right where the user left off, cursor position and all.

### Build Integration

`iso.c` joins the source file list in the Makefile. The `copy-sources` target already copies `src/*.c` and `src/*.h` to the ESP, so the new files are automatically available for F6 self-hosting rebuild.

In `edit.c`, the F6 rebuild source list must be updated manually to include `iso.c`. This list is a hardcoded array of filenames that the rebuild function compiles, separate from the Makefile. Missing a file here means F6 produces a binary without the ISO writer -- functional but incomplete. The fix is one line: add `"/src/iso.c"` to the array.

## The Complete Flow

Step back and trace the full user experience:

1. The user has two USB drives plugged in. One contains a Linux ISO downloaded on another computer. The other is blank.
2. In the file browser, the boot volume root shows two orange `[USB]` entries.
3. Navigate into the first USB drive. Find `alpine-virt-3.23.3-aarch64.iso`. The status bar shows `F10:WriteISO`.
4. Press F10. The screen clears to a cyan-bordered "WRITE ISO TO DISK" header. Below it, the ISO name and size. Below that, a numbered list of block devices, color-coded.
5. The second USB drive is highlighted in yellow as the default target. Press ENTER.
6. "Press Y to proceed." Press Y.
7. The progress display begins: "Writing: 0 MB / 847 MB (0%)". The numbers climb. At USB 2.0 speeds, an 847MB Alpine ISO takes about 90 seconds.
8. Green banner: "ISO WRITTEN SUCCESSFULLY". Press any key.
9. Back in the file browser. Unplug the target USB, reboot the machine, select the target USB from the UEFI boot menu, and Alpine Linux boots.

The edge cases are handled too. If the user accidentally selects the USB drive containing the ISO (same-device case), the workstation copies the ISO to a temp file on the boot volume first, then writes from the temp copy. If the user selects the boot device, they must type Y-E-S. If the target is too small, it is grayed out and unselectable. If the user gets cold feet, ESC cancels at every stage.

## What We Built

The survival workstation now covers both directions of the survival spectrum. Cloning (Chapter 20) propagates the workstation -- any USB drive becomes a new, independent, self-modifying survival system. ISO writing (this chapter) escapes the workstation -- any Linux ISO on any USB drive can be burned to another USB drive, creating a bootable path to a full operating system.

The implementation is about 350 lines of new code across iso.c, iso.h, and the browse.c changes. The streaming I/O from Chapter 22 made the actual write loop almost trivial -- read a chunk, pad to block alignment, write it, repeat. The bulk of the code is user interface: device enumeration, color-coded listings, same-device detection, two-tier confirmation, progress display, success and failure messaging. The plumbing is simple. The interface is where the complexity lives.

This is a pattern that recurs throughout the workstation. The filesystem operations, the block device operations, the memory allocator -- each is a thin wrapper around a UEFI protocol call. The real work is in deciding when to call them, what to show the user, and how to handle the cases where things go wrong. A streaming block write is ten lines. A safe, informative, cancelable streaming block write with device selection and same-device detection is three hundred.

The escape hatch is open. The workstation can now propagate itself (Chapter 20) and escape itself (this chapter). But there is one more gap to close: after writing an ISO to a USB drive, that drive is no longer FAT32 -- it cannot be browsed, cloned to, or reused without external tools. Chapter 24 adds the format tool, which brings these orphaned devices back into the fold.
