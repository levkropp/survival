---
layout: default
title: "Chapter 19: USB Volumes"
parent: "Phase 4: USB & Cloning"
nav_order: 1
---

# Chapter 19: USB Volumes

## The Island Problem

The workstation can rebuild itself. It carries its source code, its compiler, and its editor on a single FAT32 volume. But that volume is an island. Everything lives on one SD card or hard disk. If you plug in a USB drive, the workstation does not know it exists.

This matters for survival. You want to copy documents to a USB stick and hand it to someone. You want to read files from a drive someone gave you. You want to back up your work to a second device. The workstation needs to see USB drives as browsable volumes, right alongside the boot filesystem.

The goal for this chapter:

1. USB drives appear in the root directory listing as "[USB] LABEL (X GB)"
2. Users can enter a USB drive and browse, edit, and save files on it
3. Pressing Backspace at the USB root returns to the boot volume

## Adding F12

Before we start on USB volumes, a quick addition to the keyboard module. We will need the F12 key in the next chapter for cloning the workstation to USB. The key mapping is trivial, but we add it now so the plumbing is ready.

In `src/kbd.h`, add the constant:

```c
#define KEY_F12     0x9B
```

The gap between F10 (0x99) and F12 (0x9B) is intentional -- we skipped 0x9A for F11, which UEFI firmware sometimes uses for boot menu access. We leave it unmapped to avoid conflicts.

In `src/kbd.c`, add the scan code mapping inside `scan_to_key()`:

```c
case 0x16: return KEY_F12;
```

UEFI assigns scan code 0x16 to F12. The pattern continues from F1 at 0x0B through F10 at 0x14, skipping 0x15 (F11). One line in the header, one line in the switch. Done.

## USB Volume Discovery

### The Data Structure

UEFI treats every storage device as a separate handle. The SD card we boot from is one handle. A USB drive is another. Each handle can expose multiple protocols -- SimpleFileSystem for file access, BlockIO for raw block access, and others.

We need a structure to track discovered USB volumes. In `src/fs.h`:

```c
#define FS_MAX_USB 8

struct fs_usb_volume {
    EFI_HANDLE handle;
    EFI_FILE_HANDLE root;
    char label[48];
};
```

Eight volumes is generous. Most people plug in one or two USB drives at a time. The `handle` is the UEFI device handle -- we need it to identify which device we are talking to. The `root` is the opened root directory of the filesystem on that device. The `label` is a human-readable string we will construct from the volume label and size.

Three new functions complete the API:

```c
int  fs_enumerate_usb(struct fs_usb_volume *vols, int max);
void fs_set_volume(EFI_FILE_HANDLE new_root);
void fs_restore_boot_volume(void);
```

### Remembering the Boot Device

Before we can find USB drives, we need to know which device is *not* a USB drive -- the boot device. Back in Chapter 9, `fs_init()` opened the boot volume's root directory and stored it in `s_root`. Now we also need to remember the device handle and the original root, so we can tell them apart later.

At the top of `src/fs.c`:

```c
static EFI_FILE_HANDLE s_root;
static EFI_FILE_HANDLE s_boot_root;
static EFI_HANDLE s_boot_device;
```

And in `fs_init()`, after successfully opening the volume:

```c
s_boot_device = loaded_image->DeviceHandle;

status = sfs->OpenVolume(sfs, &s_root);
if (!EFI_ERROR(status))
    s_boot_root = s_root;
return status;
```

`s_boot_device` is the UEFI handle of the device we booted from. `s_boot_root` is a permanent copy of the boot volume's root directory handle. `s_root` is the *active* root -- it starts pointing at the boot volume, but later we will swap it to point at a USB volume. When we want to come back, `s_boot_root` tells us where home is.

### Enumerating USB Volumes

The core of this chapter is `fs_enumerate_usb()`. It asks UEFI for every device that has a filesystem, filters out the boot device, checks if each remaining device is removable, and gathers volume information.

```c
int fs_enumerate_usb(struct fs_usb_volume *vols, int max) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID fsi_guid = EFI_FILE_SYSTEM_INFO_ID;
    EFI_STATUS status;
    UINTN handle_count = 0;
    EFI_HANDLE *handles = NULL;
    int count = 0;

    if (max > FS_MAX_USB) max = FS_MAX_USB;
```

Three GUIDs, three protocols. `sfs_guid` identifies the SimpleFileSystem protocol -- any device that has a browsable filesystem. `bio_guid` identifies BlockIO -- any device that has raw block storage. `fsi_guid` identifies the FILE_SYSTEM_INFO structure -- metadata about a volume's label and size.

Step 1: find every handle that exposes a filesystem.

```c
    status = g_boot.bs->LocateHandleBuffer(
        ByProtocol, &sfs_guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || !handles)
        return 0;
```

`LocateHandleBuffer` returns an array of handles. On a system with an SD card and one USB drive, this might return two handles. On a system with no USB drives, it returns one handle (the boot device). The `ByProtocol` search type means "find every handle that supports this protocol."

Step 2: iterate through handles, skip the boot device, and check for removable media.

```c
    for (UINTN i = 0; i < handle_count && count < max; i++) {
        if (handles[i] == s_boot_device)
            continue;

        EFI_BLOCK_IO *bio = NULL;
        status = g_boot.bs->HandleProtocol(
            handles[i], &bio_guid, (void **)&bio);
        if (EFI_ERROR(status) || !bio || !bio->Media)
            continue;
        if (!bio->Media->RemovableMedia)
            continue;
```

The `s_boot_device` comparison is why we saved the handle in `fs_init()`. Without it, the boot volume would show up as a USB entry -- confusing and wrong.

`BlockIO->Media->RemovableMedia` is a boolean flag that UEFI firmware sets for USB drives, SD cards in USB readers, and similar removable devices. Fixed internal drives have this set to FALSE. This is the simplest heuristic for "is this a USB drive" -- not perfect (a fixed USB hard drive might report FALSE), but correct for the common case of USB flash drives.

Step 3: open the volume and read its metadata.

```c
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
        status = g_boot.bs->HandleProtocol(
            handles[i], &sfs_guid, (void **)&sfs);
        if (EFI_ERROR(status) || !sfs)
            continue;

        EFI_FILE_HANDLE vol_root = NULL;
        status = sfs->OpenVolume(sfs, &vol_root);
        if (EFI_ERROR(status) || !vol_root)
            continue;

        struct fs_usb_volume *v = &vols[count];
        v->handle = handles[i];
        v->root = vol_root;
```

`OpenVolume` gives us a file handle to the root directory of the filesystem on this device. This is the same operation `fs_init()` does for the boot device. We save both the raw handle (for identification) and the root handle (for file operations).

Step 4: build the display label from volume metadata.

```c
        UINTN info_size = 0;
        vol_root->GetInfo(vol_root, &fsi_guid, &info_size, NULL);
        if (info_size > 0) {
            EFI_FILE_SYSTEM_INFO *info =
                (EFI_FILE_SYSTEM_INFO *)mem_alloc(info_size);
            if (info) {
                status = vol_root->GetInfo(
                    vol_root, &fsi_guid, &info_size, info);
                if (!EFI_ERROR(status)) {
                    int pos = 0;
                    int has_label = 0;
                    if (info->VolumeLabel[0] &&
                        info->VolumeLabel[0] != L' ') {
                        has_label = 1;
                        for (int j = 0;
                             info->VolumeLabel[j] && pos < 30; j++)
                            v->label[pos++] =
                                (char)(info->VolumeLabel[j] & 0x7F);
                    }
                    if (!has_label) {
                        const char *usb = "USB";
                        for (int j = 0; usb[j]; j++)
                            v->label[pos++] = usb[j];
                    }
```

The two-call `GetInfo` pattern again: first call with NULL buffer to get the required size, second call with an allocated buffer to get the data. `EFI_FILE_SYSTEM_INFO` contains the volume label (a CHAR16 string) and the total volume size.

If the volume has a label (like "BACKUP" or "DOCUMENTS"), we use it. If the label is empty or just spaces, we fall back to "USB". The label is truncated to 30 characters to leave room for the size suffix.

```c
                    UINT64 size_mb =
                        info->VolumeSize / (1024 * 1024);
                    if (size_mb > 0) {
                        v->label[pos++] = ' ';
                        v->label[pos++] = '(';
                        if (size_mb >= 1024) {
                            UINT64 size_gb = size_mb / 1024;
                            char tmp[12];
                            int t = 0;
                            UINT64 n = size_gb;
                            if (n == 0) tmp[t++] = '0';
                            else while (n > 0) {
                                tmp[t++] = '0' + (n % 10);
                                n /= 10;
                            }
                            while (t > 0)
                                v->label[pos++] = tmp[--t];
                            v->label[pos++] = ' ';
                            v->label[pos++] = 'G';
                            v->label[pos++] = 'B';
                        } else {
                            /* Same pattern but with MB */
                            char tmp[12];
                            int t = 0;
                            UINT64 n = size_mb;
                            if (n == 0) tmp[t++] = '0';
                            else while (n > 0) {
                                tmp[t++] = '0' + (n % 10);
                                n /= 10;
                            }
                            while (t > 0)
                                v->label[pos++] = tmp[--t];
                            v->label[pos++] = ' ';
                            v->label[pos++] = 'M';
                            v->label[pos++] = 'B';
                        }
                        v->label[pos++] = ')';
                    }
                    v->label[pos] = '\0';
```

The size formatting follows the same integer-to-string-with-reverse pattern used throughout the codebase. Volumes 1 GB and above show as "X GB"; smaller volumes show as "X MB". A 16 GB drive labeled "BACKUP" produces `BACKUP (16 GB)`. An unlabeled 512 MB drive produces `USB (512 MB)`.

The function ends by freeing the handle buffer UEFI allocated:

```c
    g_boot.bs->FreePool(handles);
    return count;
```

### Volume Switching

With USB volumes discovered, switching between them is almost comically simple. Every filesystem function in `fs.c` -- `fs_readdir`, `fs_readfile`, `fs_writefile` -- operates through `s_root`. Change `s_root`, and all file operations silently redirect to the new volume.

```c
void fs_set_volume(EFI_FILE_HANDLE new_root) {
    s_root = new_root;
}

void fs_restore_boot_volume(void) {
    s_root = s_boot_root;
}
```

Two one-line functions. The entire filesystem module was already designed around a single root handle. USB support is just swapping that handle.

This is the payoff of the abstraction from Chapter 9. We did not know we would need USB support when we wrote `fs_readfile()`. But because every function opens files relative to `s_root` instead of hardcoding a device, adding USB volumes requires zero changes to the file I/O functions.

## USB Entries in the Browser

### New State Variables

The file browser from Chapter 10 needs to track whether we are currently browsing a USB volume, which USB volume we are on, and how many real filesystem entries exist before the USB entries start.

At the top of `src/browse.c`:

```c
static int s_on_usb;
static int s_usb_vol_idx;
static struct fs_usb_volume s_usb_vols[FS_MAX_USB];
static int s_usb_count;
static int s_real_count;
```

`s_on_usb` is a boolean: are we currently inside a USB volume? `s_usb_vol_idx` tracks which one. `s_usb_vols` holds the discovered volumes. `s_usb_count` is how many USB volumes were found. `s_real_count` is the number of actual filesystem entries returned by `fs_readdir()` -- everything after that index in the `s_entries` array is a synthetic USB entry.

### Handle Leak Prevention

Each discovered USB volume has an open root handle from `OpenVolume`. If we enumerate again (because the user navigated away and back), we need to close the old handles first. UEFI firmware has finite handle resources -- leaking them will eventually cause failures.

```c
static void close_usb_handles(void) {
    for (int i = 0; i < s_usb_count; i++) {
        if (s_usb_vols[i].root) {
            s_usb_vols[i].root->Close(s_usb_vols[i].root);
            s_usb_vols[i].root = NULL;
        }
    }
}
```

This is called before every new enumeration. Close all existing handles, NULL them out, then enumerate fresh.

### Modified load_dir()

The directory loading function gains USB awareness:

```c
static void load_dir(void) {
    s_count = fs_readdir(s_path, s_entries, MAX_ENTRIES);
    if (s_count < 0) s_count = 0;
    s_real_count = s_count;

    if (!s_on_usb && path_is_root()) {
        close_usb_handles();
        s_usb_count = fs_enumerate_usb(s_usb_vols, FS_MAX_USB);
        for (int i = 0; i < s_usb_count && s_count < MAX_ENTRIES; i++) {
            struct fs_entry *e = &s_entries[s_count];
            int pos = 0;
            const char *tag = "[USB] ";
            while (tag[pos] && pos < FS_MAX_NAME - 2)
                e->name[pos] = tag[pos], pos++;
            int j = 0;
            while (s_usb_vols[i].label[j] && pos < FS_MAX_NAME - 1)
                e->name[pos++] = s_usb_vols[i].label[j++];
            e->name[pos] = '\0';
            e->size = 0;
            e->is_dir = 1;
            s_count++;
        }
    } else if (!s_on_usb) {
        close_usb_handles();
        s_usb_count = 0;
    }

    s_cursor = 0;
    s_scroll = 0;
}
```

The logic: after loading the normal directory contents, if we are at the boot volume's root, enumerate USB volumes and append them as synthetic directory entries. Each gets the `[USB]` prefix followed by the label from `fs_enumerate_usb`. They are marked as directories (`is_dir = 1`) so the cursor-movement logic treats them like folders.

`s_real_count` records where the real entries end and the USB entries begin. This boundary is critical for the ENTER handler -- pressing ENTER on entry number `s_real_count + 1` should switch to USB volume 1, not try to open a directory named "[USB] BACKUP (16 GB)".

When we are not at root (navigating subdirectories on the boot volume), USB handles get closed and `s_usb_count` resets to zero. No point keeping them open while we browse `/EFI/BOOT`.

### Entry Coloring

The draw function needs to distinguish USB entries from regular directories. In `draw_entry_line()`:

```c
int is_usb_entry = (!s_on_usb && entry_idx >= s_real_count
                    && s_usb_count > 0);
if (entry_idx == s_cursor) {
    fg = COLOR_BLACK;
    bg = COLOR_CYAN;
} else if (is_usb_entry) {
    fg = COLOR_ORANGE;
    bg = COLOR_BLACK;
} else if (e->is_dir) {
    fg = COLOR_GREEN;
    bg = COLOR_BLACK;
} else {
    fg = COLOR_WHITE;
    bg = COLOR_BLACK;
}
```

The selected entry still gets the cyan highlight bar. But unselected USB entries render in orange instead of the green used for regular directories. This gives the user an instant visual signal: green entries are local directories, orange entries are external drives. The color difference is obvious even from across a room.

## Entering and Leaving USB Volumes

### The ENTER Handler

The ENTER key handler in the main browser loop gains a new first branch:

```c
case KEY_ENTER:
    if (!s_on_usb && s_cursor >= s_real_count && s_usb_count > 0) {
        s_usb_vol_idx = s_cursor - s_real_count;
        s_on_usb = 1;
        fs_set_volume(s_usb_vols[s_usb_vol_idx].root);
        path_set_root();
        load_dir();
        draw_all();
    } else if (s_count > 0 && s_entries[s_cursor].is_dir) {
        path_append(s_entries[s_cursor].name);
        load_dir();
        draw_all();
    } else if (s_count > 0) {
        open_file();
        load_dir();
        draw_all();
    }
    break;
```

The condition `s_cursor >= s_real_count` detects that the cursor is on a USB entry. We compute the volume index by subtracting `s_real_count`, set the `s_on_usb` flag, switch the active filesystem root with `fs_set_volume()`, reset the path to `\`, and reload. From this point, `fs_readdir` and `fs_readfile` operate on the USB drive transparently.

The existing directory and file handlers continue to work unchanged. When `s_on_usb` is true, they navigate subdirectories and open files on the USB volume -- because `s_root` now points at the USB volume's root.

### The Back Handler

Backspace and ESC need special logic to leave a USB volume:

```c
case KEY_BS:
    if (s_on_usb && path_is_root()) {
        fs_restore_boot_volume();
        s_on_usb = 0;
        path_set_root();
        load_dir();
        draw_all();
    } else if (!path_is_root()) {
        path_up();
        load_dir();
        draw_all();
    }
    break;
```

When the user presses Backspace at the USB root (path is `\` and `s_on_usb` is true), we restore the boot volume and clear the USB flag. The browser reloads the boot root, which will enumerate USB volumes again and show them as orange entries. The user is back where they started.

When the user is inside a subdirectory on the USB volume, the normal `path_up()` logic handles it -- navigate up one directory on the USB volume, still browsing USB.

ESC follows the same pattern: at USB root, leave the USB volume. In a subdirectory, go up. At boot root with no USB, exit the browser entirely.

### Path Display

The path bar prepends `[USB]` when browsing a USB volume, so the user always knows where they are:

```c
static void draw_path(void) {
    char line[256];
    char path_ascii[256];
    int i = 0;

    line[i++] = ' ';
    if (s_on_usb) {
        const char *tag = "[USB] ";
        int k = 0;
        while (tag[k] && i < (int)g_boot.cols)
            line[i++] = tag[k++];
    }
    path_to_ascii(path_ascii, 240);
    int j = 0;
    while (path_ascii[j] && i < (int)g_boot.cols)
        line[i++] = path_ascii[j++];
    line[i] = '\0';
    pad_line(line, g_boot.cols);

    fb_string(0, 1, line, COLOR_YELLOW, COLOR_BLACK);
}
```

On the boot volume, the path bar shows ` \EFI\BOOT`. On a USB volume, it shows ` [USB] \Documents`. The yellow text on black background stays the same -- the `[USB]` prefix is the only difference.

### Status Bar

The status bar changes when browsing a USB volume to show the available operations:

```c
if (s_on_usb)
    msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename F12:Clone BS:Back";
else
    msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename BS:Back ESC:Exit";
```

On USB, `F12:Clone` appears (for the cloning feature in the next chapter). `ESC:Exit` changes to just `BS:Back` -- pressing ESC at USB root returns to the boot volume rather than exiting the browser.

## UEFI Protocol Summary

This chapter used three UEFI protocols:

**SimpleFileSystem** (`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID`) provides file-level access to a storage device. `OpenVolume` returns a root directory handle. From there, `Open`, `Read`, `Write`, `Close`, and `GetInfo` handle all file operations. Every browsable volume -- boot disk or USB -- exposes this protocol.

**BlockIO** (`EFI_BLOCK_IO_PROTOCOL_GUID`) provides raw block-level access to storage. We do not use it for reading or writing blocks here. We use it only for its `Media->RemovableMedia` flag, which tells us whether a device is removable. This is the cheapest way to distinguish USB flash drives from internal disks.

**FILE_SYSTEM_INFO** (`EFI_FILE_SYSTEM_INFO_ID`) is not a protocol but an information type retrieved via `GetInfo` on a root directory handle. It contains the volume label (a CHAR16 string set when the filesystem was formatted), the total volume size, and the free space. We use the label and size to build the display string.

The interplay between these protocols is what makes USB discovery work. `LocateHandleBuffer` finds devices with filesystems. `BlockIO` filters for removable ones. `SimpleFileSystem` opens them. `FILE_SYSTEM_INFO` names them.

## What We Built

The workstation is no longer an island. Plug in a USB drive and it appears in the file browser as an orange entry with its label and size. Navigate into it and you can browse, edit, and save files just like on the boot volume. Press Backspace to return home.

The implementation required:

- 3 new functions in `fs.c` (about 120 lines for `fs_enumerate_usb`, 2 lines each for `fs_set_volume` and `fs_restore_boot_volume`)
- 5 new state variables in `browse.c`
- Modified `load_dir()` to append USB entries at the boot root
- Modified ENTER/BS/ESC handlers for USB-aware navigation
- Orange coloring for USB entries, `[USB]` prefix on the path bar

The binary grew by about 2 KB. The key insight was that the filesystem abstraction from Chapter 9 -- a single `s_root` handle through which all operations flow -- made USB support almost free. Swap the handle, and the entire filesystem module redirects. No changes to `fs_readfile`, `fs_writefile`, `fs_readdir`, or any function that reads or writes files.

In the next chapter, we will use this foundation to clone the entire workstation onto a USB drive -- making it bootable on a second machine.
