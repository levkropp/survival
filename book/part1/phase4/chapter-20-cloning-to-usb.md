---
layout: default
title: "Chapter 20: Cloning to USB"
parent: "Phase 4: USB & Cloning"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 2
---

# Chapter 20: Cloning to USB

## The Seed

The workstation can edit its own source code. It can compile itself. But there is still a single point of failure: the SD card. If that card dies, the workstation dies with it. Every line of code, every note, every customization -- gone.

The fix is cloning. Plug in a USB drive, press F12, and walk away with a bootable copy of the entire workstation -- source code, compiler, editor, filesystem driver, everything. The clone is independent. Pull the USB drive, plug it into any UEFI machine, and boot. A second survival workstation, grown from the first.

This is the ultimate survival feature. Not backup (which implies a primary and a copy), but reproduction. Each clone can make further clones. The system is self-propagating.

## Why UEFI Makes This Simple

On a traditional OS, creating a bootable USB involves partition tables, bootloader installation, initramfs generation, and filesystem formatting. Entire tools exist for this -- `dd`, Rufus, Etcher -- each papering over the complexity underneath.

UEFI wants almost none of that. A UEFI-bootable USB drive needs three things:

1. A FAT32 filesystem
2. The EFI binary at `\EFI\BOOT\BOOTX64.EFI` (or `BOOTAA64.EFI` on ARM64)
3. Whatever files the application needs

That is it. No boot sector. No bootloader chain. No MBR. The firmware searches removable media for a FAT32 partition, looks for the well-known path `\EFI\BOOT\BOOT*.EFI`, and runs it.

Our workstation's boot volume IS the filesystem. Source code lives in `\src\`. Tools live in `\tools\`. The EFI binary lives at the standard path. Everything the workstation needs is on one FAT32 partition. Cloning means: copy every file from the boot volume to the USB volume.

We do not even need to format the USB drive. UEFI's SimpleFileSystem protocol only works on FAT32 volumes -- if we can see the USB drive through SimpleFileSystem, it is already FAT32. We just write files using the same `fs_writefile` and `fs_mkdir` we built in Chapters 9 and 11.

## The Volume Switching Dance

The challenge is not the copy itself but the bookkeeping. Our filesystem layer (`fs.c`) has a single global root handle, `s_root`, through which all file operations flow. To clone, we need to read from the boot volume and write to the USB volume -- switching `s_root` back and forth.

Chapter 10 added USB browsing support with two functions:

```c
void fs_set_volume(EFI_FILE_HANDLE new_root) {
    s_root = new_root;
}

void fs_restore_boot_volume(void) {
    s_root = s_boot_root;
}
```

`s_boot_root` is saved during `fs_init()` -- it is the root handle of the volume the workstation booted from. `fs_set_volume` temporarily points `s_root` at a different volume (the USB drive). `fs_restore_boot_volume` switches back.

The clone function will call `fs_restore_boot_volume()` before every read and `fs_set_volume(usb_root)` before every write. It is mechanical and repetitive, but it is unambiguous. There is never a question about which volume we are operating on.

## The Recursive Copy Engine

The clone needs to copy an entire directory tree. Source directories contain subdirectories (`src/tcc-headers/`, `tools/tinycc/`), so we need recursion. Here is the full function, then we will walk through it.

First, a module-level variable to hold the USB root handle during the copy:

```c
static EFI_FILE_HANDLE s_clone_usb_root;
```

This is set once before the copy starts and read by every level of the recursion.

```c
static int clone_copy_recursive(const CHAR16 *src_path, const CHAR16 *dst_path) {
    /* Read source directory from boot volume */
    fs_restore_boot_volume();
    struct fs_entry entries[64];
    int n = fs_readdir(src_path, entries, 64);
    if (n < 0) return 0;
```

Switch to the boot volume, read the source directory. We use a local array of 64 entries -- enough for any single directory in the workstation. If the read fails (the directory does not exist or is unreadable), return silently. This is a copy, not a validation pass.

Now iterate over every entry:

```c
    for (int i = 0; i < n; i++) {
        /* Build source path (CHAR16) */
        CHAR16 src[MAX_PATH];
        int si = 0;
        while (src_path[si] && si < MAX_PATH - 1) { src[si] = src_path[si]; si++; }
        if (si > 1 || src[0] != L'\\') src[si++] = L'\\';
        int j = 0;
        while (entries[i].name[j] && si < MAX_PATH - 1)
            src[si++] = (CHAR16)entries[i].name[j++];
        src[si] = 0;
```

This builds the full source path by concatenating the parent path, a backslash separator, and the entry name. The `if (si > 1 || src[0] != L'\\')` guard handles the root directory case: if the parent path is just `\`, we do not add a second backslash (producing `\\file` instead of `\file`). The entry names come from `fs_readdir` as ASCII `char` arrays, so we widen each byte to `CHAR16` with a cast.

The destination path is built identically:

```c
        /* Build destination path (CHAR16) */
        CHAR16 dst[MAX_PATH];
        int di = 0;
        while (dst_path[di] && di < MAX_PATH - 1) { dst[di] = dst_path[di]; di++; }
        if (di > 1 || dst[0] != L'\\') dst[di++] = L'\\';
        j = 0;
        while (entries[i].name[j] && di < MAX_PATH - 1)
            dst[di++] = (CHAR16)entries[i].name[j++];
        dst[di] = 0;
```

Since we are cloning the entire volume structure, source and destination paths are mirror images. `\src\main.c` on the boot volume becomes `\src\main.c` on the USB volume.

Now the actual copy logic, which splits on directories versus files:

```c
        if (entries[i].is_dir) {
            /* Create directory on USB via SFS */
            fs_set_volume(s_clone_usb_root);
            fs_mkdir(dst);
            fs_restore_boot_volume();
            clone_copy_recursive(src, dst);
```

For directories: switch to USB, create the directory, switch back to boot, and recurse. `fs_mkdir` will silently succeed if the directory already exists (the UEFI `Open` with `CREATE` flag returns the existing handle). This means running the clone twice on the same USB drive is safe -- it overwrites files and reuses directories.

```c
        } else {
            /* Show progress */
            char msg[256];
            int mp = 0;
            const char *pre = " Copying ";
            while (pre[mp] && mp < 240) { msg[mp] = pre[mp]; mp++; }
            j = 0;
            while (dst[j] && mp < 250) msg[mp++] = (char)(dst[j++] & 0x7F);
            msg[mp] = '\0';
            draw_status_msg(msg);
```

For files: first, show the user what is happening. The status bar at the bottom of the screen updates with each file name. This is important feedback -- the clone might take 30 seconds on a large workstation, and a frozen screen looks like a crash.

The `& 0x7F` mask when converting `CHAR16` to `char` strips the high byte. All our filenames are ASCII, so this is safe.

```c
            /* Read from boot volume */
            fs_restore_boot_volume();
            UINTN file_size;
            void *data = fs_readfile(src, &file_size);
            if (data) {
                /* Write to USB volume via SFS */
                fs_set_volume(s_clone_usb_root);
                fs_writefile(dst, data, file_size);
                fs_restore_boot_volume();
                mem_free(data);
            }
        }
    }
    return 0;
}
```

Switch to boot, read the file into memory, switch to USB, write it out, switch back to boot, free the buffer. Every file is read entirely into memory and written in one shot. For the workstation's files (the largest being `libtcc.c` at about 60KB), this is fine. We are not streaming gigabytes.

The `fs_restore_boot_volume()` call after writing is important. The next iteration of the loop needs to be on the boot volume to read the next file. Without it, the next `fs_readfile` would try to read from the USB volume and either fail or copy a USB file onto itself.

## The Clone UI

The clone is triggered by `clone_to_usb()`, called when the user presses F12 while browsing a USB volume. Here is the full function:

```c
static void clone_to_usb(void) {
    if (!s_on_usb || s_usb_vol_idx < 0 || s_usb_vol_idx >= s_usb_count)
        return;
```

Safety check. This function should only be called when browsing a USB volume, but defensive programming costs one line.

```c
    /* Warning screen */
    fb_clear(COLOR_BLACK);
    fb_print("\n", COLOR_WHITE);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("       CLONE WORKSTATION TO USB\n", COLOR_CYAN);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("\n", COLOR_WHITE);
    fb_print("  This will copy the entire boot volume\n", COLOR_YELLOW);
    fb_print("  onto the USB drive, creating a bootable\n", COLOR_YELLOW);
    fb_print("  Survival Workstation clone.\n", COLOR_YELLOW);
    fb_print("\n", COLOR_WHITE);
    fb_print("  Press 'Y' to proceed, any other key to cancel.\n", COLOR_YELLOW);

    struct key_event ev;
    kbd_wait(&ev);
    if (ev.code != 'Y' && ev.code != 'y') {
        fb_print("\n  Cancelled.\n", COLOR_WHITE);
        fb_print("  Press any key to return.\n", COLOR_WHITE);
        kbd_wait(&ev);
        return;
    }
```

A clear warning, and a confirmation gate. Cloning overwrites files on the USB drive. The user must press Y explicitly -- any other key cancels.

```c
    /* Save USB root handle, then switch to boot volume for reading */
    s_clone_usb_root = s_usb_vols[s_usb_vol_idx].root;
    fs_restore_boot_volume();
```

Save the USB volume's root handle into the module-level variable where `clone_copy_recursive` can reach it. Then switch to the boot volume so the recursive copy starts by reading from boot.

```c
    /* Recursively copy boot volume to USB via UEFI SFS */
    fb_print("\n  Copying files...\n", COLOR_WHITE);
    CHAR16 root_path[2] = { L'\\', 0 };
    clone_copy_recursive(root_path, root_path);
```

One line does all the work. Copy from root (`\`) to root (`\`). The recursive function handles every subdirectory, every file, switching volumes as needed.

```c
    fb_print("\n\n", COLOR_WHITE);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("    BOOTABLE USB CLONE CREATED!\n", COLOR_GREEN);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("\n", COLOR_WHITE);
    fb_print("  The USB drive is now a bootable copy of\n", COLOR_WHITE);
    fb_print("  the Survival Workstation.\n", COLOR_WHITE);
    fb_print("\n", COLOR_WHITE);
    fb_print("  Press any key to return.\n", COLOR_WHITE);
    kbd_wait(&ev);

    /* Return to boot volume root */
    s_on_usb = 0;
    path_set_root();
}
```

Success screen. Then return the browser to the boot volume root. The user is back where they started, and the USB drive is bootable.

## Wiring It Up

The F12 key handler in the main browser loop is three lines:

```c
case KEY_F12:
    if (s_on_usb) {
        clone_to_usb();
        load_dir();
        draw_all();
    }
    break;
```

`clone_to_usb()` already resets `s_on_usb` and the path, so `load_dir()` reloads the boot volume root. `draw_all()` redraws the browser.

The key only works when browsing a USB volume. The status bar reflects this -- it shows the available commands based on context:

```c
if (s_on_usb)
    msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename F12:Clone BS:Back";
else
    msg = " ENTER:Open F3:Copy F4:New F8:Paste F9:Rename BS:Back ESC:Exit";
```

When you are browsing a USB volume, F12:Clone appears. The interface communicates what is possible.

## What the Clone Produces

After F12 completes, the USB drive contains an exact mirror of the boot volume:

```
\EFI\BOOT\BOOTAA64.EFI     (or BOOTX64.EFI)
\src\main.c
\src\fb.c
\src\kbd.c
\src\mem.c
\src\font.c
\src\fs.c
\src\browse.c
\src\edit.c
\src\shim.c
\src\tcc.c
\src\tcc-headers\...
\src\user-headers\...
\tools\tinycc\libtcc.c
\tools\tinycc\config.h
...
```

The EFI binary is at the UEFI-standard path. The source code is alongside it. Plug this USB drive into any machine with UEFI firmware (which is effectively every machine manufactured since 2012), select it as the boot device, and the Survival Workstation boots. It has the editor. It has the compiler. It has its own source code. It can rebuild itself. And it can clone itself onto yet another USB drive.

## The Full Circle

Step back and look at what we have built across these twenty chapters.

Phase 1 gave us eyes (framebuffer), ears (keyboard), and hands (memory). Phase 2 gave us a filing cabinet (filesystem) and a notepad (editor). Phase 3 gave us a brain (TCC compiler) that can rebuild the entire system from source. And now Phase 4 gives us reproduction.

The workstation can:

1. **Edit its own source code** -- open any `.c` file, modify it, save it
2. **Compile itself** -- press F6 to rebuild the entire binary from source
3. **Clone itself onto USB drives** -- press F12 to create a bootable copy

This is a complete lifecycle. A single SD card boots a workstation that can modify itself, test changes, and propagate copies onto removable media. Each copy is fully independent -- it does not phone home, does not need a network, does not require any external tool.

Like a seed that contains the blueprint for the whole plant, plus the machinery to execute that blueprint, plus the ability to produce new seeds. The workstation carries its source, its compiler, and its cloning mechanism in a single FAT32 partition under 4MB.

Any USB drive becomes a new survival workstation. Any UEFI machine becomes a potential host. The system has escaped the single point of failure we started this chapter worrying about. It is no longer fragile. It propagates.
