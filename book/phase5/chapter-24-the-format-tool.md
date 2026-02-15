# Chapter 24: The Format Tool

## The Invisible Drive

The workstation can browse FAT32 USB volumes, clone to them, and write ISOs to raw block devices. But there is a gap. Plug in a USB drive formatted as ext4 or NTFS -- which is most USB drives that have been used with Linux or Windows -- and nothing happens. The file browser does not show it. It is invisible.

The reason is simple. UEFI's SimpleFileSystem protocol only speaks FAT32. When `fs_enumerate_usb()` scans for volumes, it queries every handle for SimpleFileSystem support. ext4 drives have no SimpleFileSystem. NTFS drives have no SimpleFileSystem. A USB drive that previously had an ISO written to it (Chapter 23) has no SimpleFileSystem either -- it now contains a raw ISO 9660 image that UEFI cannot parse. These drives exist at the block device level -- `disk_enumerate()` finds them -- but the file browser never sees them.

The format tool closes this gap. Non-FAT block devices appear at the browser root as red `[DISK]` entries, alongside the orange `[USB]` entries. Press ENTER on one and the workstation offers to format it as FAT32. After formatting, the device becomes a normal FAT32 volume -- a `[USB]` entry you can browse, write files to, or clone the workstation onto.

Survival scenario: someone hands you a laptop with an NTFS-formatted USB stick. Format it, clone the workstation onto it, and you have another survival system. Or: you wrote a Linux ISO to a USB drive in Chapter 23 and now want to reuse it -- format it back to FAT32 and the drive is yours again.

But the gap is wider than that. Even a USB drive that IS already FAT32 -- an orange `[USB]` entry you can browse into -- sometimes needs to be wiped clean and reformatted. Maybe you want a fresh filesystem. Maybe the directory structure is a mess. The format tool should work on both: raw devices that need a filesystem, and existing FAT32 volumes that need a fresh start.

The implementation adds about 200 lines across three existing files, including a block-level FAT32 validation function, USB volume filtering, and dynamic cluster sizing in the formatter. No new source files.

## Adding F11

Chapter 19 deliberately skipped F11 when mapping function keys, noting that some firmware uses it for the boot menu. We do not need F11 as the primary trigger -- ENTER on a `[DISK]` entry does the job -- but we should map it for completeness. The status bar will show both `ENTER:Format` and `F11:Format` when the cursor sits on a raw disk entry.

In `src/kbd.h`, one line between F10 and F12:

```c
#define KEY_F10     0x99
#define KEY_F11     0x9A
#define KEY_F12     0x9B
```

Our key codes are arbitrary internal constants -- they just need to be unique values above the ASCII range. 0x9A fills the gap between 0x99 (F10) and 0x9B (F12).

In `src/kbd.c`, one line in `scan_to_key()`:

```c
    case 0x14: return KEY_F10;
    case 0x15: return KEY_F11;
    case 0x16: return KEY_F12;
```

UEFI scan code 0x15 is F11 in the UEFI specification's scan code table. The gap between 0x14 (F10) and 0x16 (F12) was intentional in the original code -- now filled.

## Discovering Raw Devices

The file browser already has two categories of entries at the boot volume root: regular filesystem entries (files and directories from `fs_readdir`) and `[USB]` entries (from `fs_enumerate_usb`). We are adding a third: `[DISK]` entries for block devices that have no filesystem.

Three new module-level variables in `src/browse.c`:

```c
/* Raw block device state (non-FAT devices shown as [DISK]) */
static struct disk_device s_disk_devs[DISK_MAX_DEVICES];
static int s_disk_count;
static int s_disk_start_idx;
```

`s_disk_devs` stores the actual device structures returned by `disk_enumerate()`. We need these later when the user selects a device for formatting -- the `fat32_format()` function takes a `struct disk_device *`.

`s_disk_count` tracks how many raw devices were found. `s_disk_start_idx` records where in the `s_entries` array the `[DISK]` entries begin. The entry array has three regions: regular files (0 to `s_real_count - 1`), USB volumes (`s_real_count` to `s_real_count + s_usb_count - 1`), and raw disks (`s_disk_start_idx` to `s_disk_start_idx + s_disk_count - 1`).

Two new includes are needed at the top of browse.c:

```c
#include "disk.h"
#include "fat32.h"
```

`disk.h` provides `disk_enumerate()` and the `struct disk_device` type. `fat32.h` provides `fat32_format()`.

## The Stale Filesystem Problem

There is a subtle trap with UEFI's driver model. When UEFI boots, it probes each block device and binds a FAT driver to any device with a valid FAT32 boot sector. The FAT driver installs a SimpleFileSystem protocol on the device's handle. This binding persists for the lifetime of the boot session -- even if the underlying data changes.

Consider: a USB drive arrives with FAT32. UEFI binds the FAT driver. The ISO writer (Chapter 23) overwrites the drive with raw ISO 9660 data. The FAT32 boot sector is gone, replaced by an ISO 9660 primary volume descriptor. But UEFI does not know this. The SimpleFileSystem protocol is still attached to the handle. `fs_enumerate_usb()` still finds it. The file browser still shows it as an orange `[USB]` entry.

If the user tries to browse into that entry, they will find garbage -- or nothing -- because the FAT driver is reading ISO 9660 data as if it were FAT32. The directory listing is meaningless. Worse: the device should appear as a red `[DISK]` entry, ready for reformatting, but it is masquerading as a valid volume.

The fix is `has_valid_fat32()`, a function that reads block 0 of a device and checks whether the actual on-disk data is still FAT32:

```c
static int has_valid_fat32(EFI_HANDLE handle) {
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

Two checks. The boot signature `0x55AA` at offset 510 validates that this looks like a boot sector at all. The string `FAT32` at offset 82 is the filesystem type field in the FAT32 BPB -- it is always present in a valid FAT32 volume boot record, and never present in an ISO 9660 image (offset 82 of an ISO 9660 primary volume descriptor contains part of the system identifier string).

This function bypasses UEFI's cached protocol entirely. It goes directly to the hardware via `ReadBlocks`, reads the actual bytes on disk, and makes its own determination. The firmware's opinion about whether a device has a filesystem is irrelevant -- only the bytes matter.

`has_valid_fat32()` is used in two places in `load_dir()`: once to filter the USB volume list (removing volumes whose FAT32 was destroyed), and once to decide whether a block device should appear as `[DISK]` (showing devices where SFS is cached but FAT32 is gone).

## Enumerating in load_dir()

The enumeration happens in `load_dir()`, right after the existing USB enumeration block. When we are at the boot volume root (not browsing a USB drive, path is `\`), we enumerate all block devices and filter them:

```c
        /* Validate USB volumes -- remove those with destroyed FAT32.
           UEFI caches SFS protocol, so a device whose FAT32 was overwritten
           (e.g. by an ISO write) may still appear as a valid volume. */
        for (int i = 0; i < s_usb_count; ) {
            if (!has_valid_fat32(s_usb_vols[i].handle)) {
                if (s_usb_vols[i].root) {
                    s_usb_vols[i].root->Close(s_usb_vols[i].root);
                    s_usb_vols[i].root = NULL;
                }
                for (int j = i; j < s_usb_count - 1; j++)
                    s_usb_vols[j] = s_usb_vols[j + 1];
                s_usb_count--;
            } else {
                i++;
            }
        }

        for (int i = 0; i < s_usb_count && s_count < MAX_ENTRIES; i++) {
            /* ... build [USB] entries as before ... */
        }

        /* Enumerate raw block devices (no filesystem) */
        s_disk_count = 0;
        s_disk_start_idx = s_count;
        struct disk_device all_devs[DISK_MAX_DEVICES];
        int ndevs = disk_enumerate(all_devs, DISK_MAX_DEVICES);

        EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        for (int i = 0; i < ndevs && s_count < MAX_ENTRIES; i++) {
            if (all_devs[i].is_boot_device) continue;

            /* Skip devices that have SimpleFileSystem AND valid FAT32.
               If SFS exists but FAT32 is invalid (stale cache), show as [DISK]. */
            void *sfs = NULL;
            EFI_STATUS st = g_boot.bs->HandleProtocol(
                all_devs[i].handle, &sfs_guid, &sfs);
            if (!EFI_ERROR(st) && sfs && has_valid_fat32(all_devs[i].handle))
                continue;

            /* This is a raw block device -- show as [DISK] */
            s_disk_devs[s_disk_count] = all_devs[i];
            struct fs_entry *e = &s_entries[s_count];
            int pos = 0;
            const char *tag = "[DISK] ";
            while (tag[pos] && pos < FS_MAX_NAME - 2)
                e->name[pos] = tag[pos], pos++;
            int j = 0;
            while (all_devs[i].name[j] && pos < FS_MAX_NAME - 1)
                e->name[pos++] = all_devs[i].name[j++];
            e->name[pos] = '\0';
            e->size = all_devs[i].size_bytes;
            e->is_dir = 0;
            s_count++;
            s_disk_count++;
        }
```

Walk through the logic. Two stages: first, validate the USB volumes; then, enumerate raw block devices.

The USB validation loop runs `has_valid_fat32()` on every volume that `fs_enumerate_usb()` returned. If a volume's on-disk data is no longer FAT32 -- because the ISO writer overwrote it, or because the user swapped drives -- the volume is removed from the list. Its root handle is closed to release firmware resources, and the remaining entries shift down. The loop uses a `while` with manual index management instead of `for` because removing an element changes the count mid-iteration.

After validation, the surviving USB volumes get their `[USB]` entries as before.

Then comes the block device enumeration. `s_disk_start_idx = s_count` records where we are in the entry array before adding any disk entries. At this point, `s_count` already includes regular files and validated USB entries.

`disk_enumerate()` returns every block device the firmware knows about -- SD cards, USB drives, NVMe drives, virtual disks. It is the same function used by the ISO writer (Chapter 23) to list write targets. Each device has a handle, a name (like "USB 8 GB"), a size, and an `is_boot_device` flag.

The first filter is `is_boot_device`. The boot SD card should never appear as a formattable entry. Formatting the boot device would destroy the running workstation.

The second filter has two parts: `HandleProtocol` checks whether the device's handle supports SimpleFileSystem, AND `has_valid_fat32()` checks whether the on-disk data is actually FAT32. Both must be true to skip the device. If the handle has SFS but `has_valid_fat32()` returns false, the firmware has a stale cached protocol -- the device should appear as a `[DISK]` entry. This is the key fix for the post-ISO-write scenario: the firmware thinks the device has a filesystem, but the bytes say otherwise.

If a device passes both filters, it is a raw block device with no valid filesystem. We copy the device structure into `s_disk_devs` (for later use by the format function), then build a display name: the `[DISK]` prefix followed by the device name from `disk_enumerate()`. The name field already contains a human-readable description like "USB 8 GB" or "Disk 512 GB".

Setting `e->is_dir = 0` means the entry is not treated as a directory. This prevents `draw_entry_line()` from prepending `[DIR]` to the name -- without this, entries would display as `[DIR] [DISK] USB 128 MB`, which is both ugly and misleading. The ENTER handler checks the disk entry index range directly rather than relying on `is_dir`, so the entry still responds to ENTER. The `e->size` field holds the raw device size, which `draw_entry_line()` will display in the size column.

The `else if (!s_on_usb)` branch also needs to clear `s_disk_count`:

```c
    } else if (!s_on_usb) {
        close_usb_handles();  /* close USB handles when not at root */
        s_usb_count = 0;
        s_disk_count = 0;
    }
```

When not at root, there are no USB or disk entries to show.

## Coloring the Entries

Raw disks need to stand out visually. Orange means "mounted USB volume, safe to browse." Red means "raw device, formatting will erase everything." The color difference communicates risk at a glance.

In `draw_entry_line()`, the color selection logic gains two new checks:

```c
    /* Choose colors based on selection */
    UINT32 fg, bg;
    int is_usb_entry = (!s_on_usb && entry_idx >= s_real_count
                        && entry_idx < s_real_count + s_usb_count);
    int is_disk_entry = (!s_on_usb && s_disk_count > 0
                         && entry_idx >= s_disk_start_idx
                         && entry_idx < s_disk_start_idx + s_disk_count);
    if (entry_idx == s_cursor) {
        fg = COLOR_BLACK;
        bg = COLOR_CYAN;
    } else if (is_disk_entry) {
        fg = COLOR_RED;
        bg = COLOR_BLACK;
    } else if (is_usb_entry) {
        fg = COLOR_ORANGE;
        bg = COLOR_BLACK;
    } else if (e->is_dir) {
```

`is_usb_entry` also gets a tighter bounds check. The original code only checked `entry_idx >= s_real_count`, which could match disk entries too now that they sit after USB entries. Adding `entry_idx < s_real_count + s_usb_count` makes it precise.

`is_disk_entry` checks the same way: within the disk entry range. The cursor highlight (cyan background) takes priority over both -- when the cursor is on a disk entry, it is cyan like everything else, so the user can see where they are. The red only appears on unselected disk entries.

## The Status Bar

The status bar is the interface contract. It tells the user what actions are available right now. When the cursor sits on a `[DISK]` entry, the bar should show `ENTER:Format F11:Format`. When it sits on a `[USB]` entry, the bar should show `ENTER:Open F11:Format` -- you can still browse into it, but F11 offers a reformat. When the cursor moves to a regular file or directory, those options disappear.

In `draw_status_msg()`, two new checks before the existing USB/boot branches:

```c
        int on_disk = (!s_on_usb && s_disk_count > 0
                       && s_cursor >= s_disk_start_idx
                       && s_cursor < s_disk_start_idx + s_disk_count);
        int on_usb_entry = (!s_on_usb && s_usb_count > 0
                            && s_cursor >= s_real_count
                            && s_cursor < s_real_count + s_usb_count);
        if (on_disk) {
            msg = " ENTER:Format F11:Format                           BS:Back ESC:Exit";
        } else if (on_usb_entry) {
            msg = " ENTER:Open F11:Format                             BS:Back ESC:Exit";
        } else if (s_on_usb) {
```

For `[DISK]` entries, both ENTER and F11 trigger format -- there is nothing else to do with a raw device. For `[USB]` entries, ENTER still opens the volume for browsing (the existing behavior), but F11 offers to reformat it. The user sees both options and picks the one they want.

`on_usb_entry` uses precise bounds: `s_cursor >= s_real_count` (past the regular file entries) and `s_cursor < s_real_count + s_usb_count` (before the disk entries). This prevents the check from matching disk entries that sit after USB entries in the array.

The extra spaces before `BS:Back ESC:Exit` push the navigation keys to the right, separating them from the action keys. This is a visual convention the status bar already follows.

## The Format Function

`do_format_disk()` handles the confirmation UI and calls `fat32_format()`. It takes a `struct disk_device *` directly -- not an index -- so it can be called from both the `[DISK]` path (which has devices in `s_disk_devs`) and the `[USB]` path (which finds the device dynamically). The function follows the same pattern used by `clone_to_usb()` in Chapter 20 and `iso_write()` in Chapter 23: clear the screen, show a warning, wait for confirmation, do the work, show the result.

```c
static void do_format_disk(struct disk_device *dev) {
    fb_clear(COLOR_BLACK);
    fb_print("\n", COLOR_WHITE);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("       FORMAT DEVICE AS FAT32\n", COLOR_CYAN);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("\n", COLOR_WHITE);
```

The cyan header bar. Same visual structure as clone and ISO write. The user learns to recognize cyan borders as "major operation ahead."

```c
    /* Show device info */
    fb_print("  Device: ", COLOR_WHITE);
    fb_print(dev->name, COLOR_WHITE);
    fb_print("\n\n", COLOR_WHITE);
    fb_print("  This will ERASE all data on this device!\n", COLOR_RED);
    fb_print("  A new FAT32 filesystem will be created.\n", COLOR_YELLOW);
    fb_print("\n", COLOR_WHITE);
    fb_print("  Press 'Y' to proceed, any other key to cancel.\n", COLOR_YELLOW);
```

The device name comes from `disk_enumerate()` -- something like "USB 8 GB" or "Disk 512 GB". The red warning line is the most important sentence on the screen. Yellow text explains what will happen. The user sees exactly which device will be erased and knows the cost.

```c
    struct key_event ev;
    kbd_wait(&ev);
    if (ev.code != 'Y' && ev.code != 'y') return;
```

One-key confirmation gate. Unlike the ISO writer's boot-device case (which requires typing Y-E-S), a single Y is enough here. The boot device is already excluded from the disk list, so there is no "destroy the workstation" scenario. The worst outcome is erasing data on a USB drive -- destructive, but the same risk level as cloning.

Accepting both uppercase and lowercase Y is a small mercy. The user should not have to fumble with Shift in a stressful moment.

```c
    fb_print("\n  Formatting...\n", COLOR_WHITE);

    int rc = fat32_format(dev);

    /* Force firmware to re-probe -- pick up the new FAT32 */
    disk_reconnect(dev);
```

`fat32_format()` is the function we built in the disk and filesystem chapters. It creates a superfloppy FAT32 filesystem -- the BPB goes directly at LBA 0 with no MBR partition table. This is how `mkfs.vfat` formats USB drives and it is the most compatible layout for UEFI firmware. The function takes a `struct disk_device *` and uses `disk_write_blocks()` internally. On a typical USB drive, this completes in under a second -- it only writes the metadata sectors (boot sector, FAT tables, root directory), not the entire device.

One subtlety: `fat32_format()` dynamically selects the cluster size. The FAT32 specification requires at least 65,525 data clusters -- below this threshold, the filesystem is technically FAT16, and UEFI's FAT driver will refuse to bind. A fixed 8 sectors-per-cluster (4 KB clusters) works for drives 256 MB and larger, but a 128 MB drive with 4 KB clusters yields only about 32,700 clusters -- half the minimum. The formatter starts at 8 sectors-per-cluster and halves until the cluster count exceeds 65,525. On a 128 MB drive, it selects 2 sectors-per-cluster (1 KB clusters). On drives 256 MB and up, 8 sectors-per-cluster works on the first try.

After formatting, `disk_reconnect()` calls `DisconnectController` then `ConnectController` on the device handle. `DisconnectController` is critical -- it detaches any stale drivers that were bound to the handle before the format. Without it, UEFI might see the old (now stale) SimpleFileSystem protocol and skip re-probing. The disconnect drops the stale state, then `ConnectController` forces a fresh probe. With a valid FAT32 BPB now at LBA 0 and enough clusters to qualify as FAT32, the firmware's FAT driver binds to the handle and installs a new SimpleFileSystem protocol. When `load_dir()` runs next, `fs_enumerate_usb()` will find the newly attached SFS, `has_valid_fat32()` will confirm the on-disk data is real, and the device will appear as an orange `[USB]` entry.

```c
    if (rc < 0) {
        fb_print("  Format FAILED.\n", COLOR_RED);
    } else {
        fb_print("\n", COLOR_WHITE);
        fb_print("  ========================================\n", COLOR_GREEN);
        fb_print("    FORMAT COMPLETE!\n", COLOR_GREEN);
        fb_print("  ========================================\n", COLOR_GREEN);
        fb_print("\n", COLOR_WHITE);
        fb_print("  The device is now FAT32.\n", COLOR_WHITE);
        fb_print("  It will appear as a [USB] volume.\n", COLOR_WHITE);
    }

    fb_print("\n  Press any key to return.\n", COLOR_DGRAY);
    kbd_wait(&ev);
}
```

Green banner on success, red message on failure. The success text tells the user what to expect next: the device will appear as a `[USB]` volume when they return to the browser. This is important context -- the `[DISK]` entry they just formatted will vanish, replaced by an orange `[USB]` entry they can browse into.

The "press any key" prompt is dark gray. It is functional but visually quiet -- the green banner is the main event.

## Finding the Disk Behind a USB Volume

For `[DISK]` entries, we already have the `struct disk_device` in `s_disk_devs` -- it was saved during `load_dir()`. But for `[USB]` entries, we only have a `struct fs_usb_volume` with a SimpleFileSystem handle. We need to find the whole-disk block device underneath it so we can call `fat32_format()`.

```c
static int find_disk_for_usb(int usb_idx, struct disk_device *out) {
    EFI_HANDLE vol_handle = s_usb_vols[usb_idx].handle;

    struct disk_device devs[DISK_MAX_DEVICES];
    int ndevs = disk_enumerate(devs, DISK_MAX_DEVICES);

    /* Case 1: direct handle match (superfloppy -- no partition table) */
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].handle == vol_handle && !devs[i].is_boot_device) {
            *out = devs[i];
            return 0;
        }
    }

    /* Case 2: USB volume is a partition on the disk */
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO *vol_bio = NULL;
    g_boot.bs->HandleProtocol(vol_handle, &bio_guid, (VOID **)&vol_bio);
    if (vol_bio && vol_bio->Media && vol_bio->Media->LogicalPartition) {
        UINT64 vol_size = (UINT64)(vol_bio->Media->LastBlock + 1) *
                          (UINT64)vol_bio->Media->BlockSize;
        for (int i = 0; i < ndevs; i++) {
            if (devs[i].is_removable && !devs[i].is_boot_device
                && devs[i].size_bytes >= vol_size) {
                *out = devs[i];
                return 0;
            }
        }
    }

    return -1;
}
```

Two cases, matching the two ways a USB drive can present itself to UEFI.

**Superfloppy**: The USB drive has no partition table. The FAT32 filesystem occupies the entire device. In this case, the SimpleFileSystem handle and the BlockIO handle are the same UEFI handle. A direct comparison catches it: `devs[i].handle == vol_handle`.

**Partitioned**: The USB drive has a GPT or MBR partition table. The FAT32 filesystem lives on a partition, which gets its own UEFI handle separate from the whole-disk handle. We detect this by checking `BlockIO->Media->LogicalPartition` on the volume's handle. If it is a logical partition, we look for a removable whole-disk device that is at least as large as the partition.

This heuristic is the same one the ISO writer uses in `is_same_device()` (Chapter 23). It is not perfect -- two USB drives of the same size could cause a false positive. But the consequence of a false positive is formatting the wrong device, and the confirmation screen shows the device name and size, so the user can catch it. The consequence of a false negative is the F11 key silently doing nothing -- safe but unhelpful.

The function returns 0 on success and populates the output `struct disk_device`, or returns -1 if no matching disk was found. The caller checks the return value before proceeding.

## Wiring Up the Key Handlers

Two places in the main browser loop handle the format action: ENTER and F11.

The ENTER handler needs a new check before the existing USB volume check:

```c
        case KEY_ENTER:
            if (!s_on_usb && s_disk_count > 0
                && s_cursor >= s_disk_start_idx
                && s_cursor < s_disk_start_idx + s_disk_count) {
                do_format_disk(&s_disk_devs[s_cursor - s_disk_start_idx]);
                load_dir();
                draw_all();
            } else if (!s_on_usb && s_cursor >= s_real_count && s_usb_count > 0
                       && s_cursor < s_real_count + s_usb_count) {
                /* Entering a USB volume */
```

The order matters. Disk entries come after USB entries in the array, and we check disk entries first because their index range is distinct. `s_cursor - s_disk_start_idx` converts the cursor position to a disk device index (0 to `s_disk_count - 1`), which indexes into `s_disk_devs`. We pass the device pointer directly to `do_format_disk`.

After `do_format_disk()` returns, `load_dir()` re-enumerates everything. If the format succeeded, the device now has a FAT32 filesystem. On the next `load_dir()`, `HandleProtocol` with the SimpleFileSystem GUID will succeed on this device's handle, so it will be filtered out of the `[DISK]` list and picked up by `fs_enumerate_usb()` instead. The red `[DISK]` entry vanishes. An orange `[USB]` entry appears. The user sees the transformation.

The USB entry check also gains a tighter bounds condition: `s_cursor < s_real_count + s_usb_count`. Without this, cursor positions in the disk entry range could accidentally trigger USB volume navigation.

ENTER on a `[USB]` entry still navigates into the volume -- that behavior is unchanged. Only F11 triggers format on USB volumes, because ENTER already has a useful meaning there.

The F11 handler handles both entry types:

```c
        case KEY_F11:
            if (!s_on_usb && s_disk_count > 0
                && s_cursor >= s_disk_start_idx
                && s_cursor < s_disk_start_idx + s_disk_count) {
                /* Format a raw [DISK] entry */
                do_format_disk(&s_disk_devs[s_cursor - s_disk_start_idx]);
                load_dir();
                draw_all();
            } else if (!s_on_usb && s_usb_count > 0
                       && s_cursor >= s_real_count
                       && s_cursor < s_real_count + s_usb_count) {
                /* Format a [USB] entry -- find underlying block device */
                int usb_idx = s_cursor - s_real_count;
                struct disk_device dev;
                if (find_disk_for_usb(usb_idx, &dev) == 0) {
                    /* Close the volume handle before destroying its filesystem */
                    if (s_usb_vols[usb_idx].root) {
                        s_usb_vols[usb_idx].root->Close(s_usb_vols[usb_idx].root);
                        s_usb_vols[usb_idx].root = NULL;
                    }
                    do_format_disk(&dev);
                    load_dir();
                    draw_all();
                }
            }
            break;
```

The first branch is the `[DISK]` case -- same as ENTER. The second branch is the `[USB]` case, which requires more care.

`find_disk_for_usb` locates the underlying block device. If it fails (returns -1), F11 silently does nothing -- the device cannot be matched to a whole-disk block device, so formatting is not possible. This can happen with unusual UEFI firmware that creates handles in unexpected ways.

If the match succeeds, we must close the USB volume's root handle before formatting. The reason: `fat32_format()` will overwrite the device's FAT32 metadata -- the boot sector, the FAT tables, the root directory. The UEFI firmware has an open SimpleFileSystem connection to this device. Writing new FAT32 structures underneath it could confuse the firmware. Closing the handle first releases the firmware's internal state cleanly.

Setting `s_usb_vols[usb_idx].root = NULL` after closing prevents a double-close. When `load_dir()` calls `close_usb_handles()` at the start of the next enumeration, it checks for NULL and skips already-closed handles.

After formatting, `load_dir()` re-enumerates. The device now has a fresh FAT32 filesystem. UEFI's SimpleFileSystem driver picks it up, `fs_enumerate_usb()` finds it, and it reappears as an orange `[USB]` entry -- empty, clean, ready for use.

## What We Built

About 200 lines across three files, no new source files created:

- `src/kbd.h` -- one line: `KEY_F11` constant
- `src/kbd.c` -- one line: scan code 0x15 mapping
- `src/browse.c` -- `has_valid_fat32()` block-level validation, USB volume filtering in `load_dir()`, three state variables, disk enumeration with stale-SFS detection, color logic, status bar updates, `find_disk_for_usb()` helper, format confirmation function, ENTER and F11 handlers for both `[DISK]` and `[USB]` entries
- `src/fat32.c` -- dynamic sectors-per-cluster selection to meet FAT32's 65,525 cluster minimum

The feature closes the last visibility gap in the workstation's device handling. Before this chapter, the file browser showed a filtered view of the world: only devices with a valid (or seemingly valid) FAT32 filesystem. Drives with other filesystems, no filesystem at all, or stale cached protocols were invisible -- you could not interact with them even though the hardware was right there. And even FAT32 volumes that you wanted to wipe clean required leaving the workstation to find some other tool.

Now the browser shows the full picture. FAT32 volumes appear as orange `[USB]` entries, ready to browse or reformat. Non-FAT block devices -- including drives whose FAT32 was destroyed by an ISO write -- appear as red `[DISK]` entries, ready to format. The color coding -- orange for safe, red for dangerous -- communicates the state at a glance. F11 on any device entry brings up the format screen.

This completes the device lifecycle. A USB drive can be:

1. **Formatted** (this chapter) -- raw or foreign filesystem becomes FAT32
2. **Browsed** (Chapter 19) -- navigate files, open them in the editor
3. **Cloned to** (Chapter 20) -- receive a full copy of the workstation
4. **Written with an ISO** (Chapter 23) -- become a bootable Linux installer
5. **Reformatted** (this chapter) -- wiped clean or reclaimed from ISO 9660

The cycle is complete. No USB drive is ever permanently lost to the workstation. Whatever state it arrives in -- NTFS, ext4, ISO 9660, corrupted, stale-cached, or just full of junk -- the workstation can bring it back to FAT32 and put it to use.

Phase 5 is complete. The workstation started as a blank screen and a blinking cursor. Twenty-four chapters later, it edits code, compiles itself, browses USB drives, clones itself onto new media, writes bootable disk images, and formats any drive back to FAT32. All of it fits in under 4 MB. All of it runs without an operating system. All of it can rebuild itself from source.
