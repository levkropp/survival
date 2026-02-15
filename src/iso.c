/*
 * iso.c — ISO image writer for Survival Workstation
 *
 * Stream-writes a .iso file to a block device chunk by chunk.
 * Modern Linux ISOs are "hybrid" — dd'ing them to a USB drive
 * creates a valid UEFI-bootable disk.
 */

#include "boot.h"
#include "fb.h"
#include "kbd.h"
#include "mem.h"
#include "fs.h"
#include "disk.h"
#include "iso.h"
#include "shim.h"

#define CHUNK_SIZE (64 * 1024)  /* 64KB streaming chunks */

/* ---- UI helpers ---- */

static void iso_print(const char *msg, UINT32 color) {
    if (g_boot.framebuffer) fb_print(msg, color);
}

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

static void show_copy_progress(UINT64 copied, UINT64 total, UINT32 row) {
    char line[128];
    UINT64 copied_mb = copied / (1024 * 1024);
    UINT64 total_mb = total / (1024 * 1024);
    int pct = total > 0 ? (int)((copied * 100) / total) : 0;

    snprintf(line, sizeof(line), "  Copying ISO to boot volume: %llu MB / %llu MB (%d%%)",
             (unsigned long long)copied_mb, (unsigned long long)total_mb, pct);

    int len = (int)strlen(line);
    while (len < (int)g_boot.cols && len < 126) line[len++] = ' ';
    line[len] = '\0';

    fb_string(0, row, line, COLOR_YELLOW, COLOR_BLACK);
}

/* ---- Same-device detection ---- */

static int is_same_device(EFI_HANDLE vol_handle, struct disk_device *dev) {
    if (!vol_handle || !dev) return 0;

    /* Direct handle match (superfloppy / whole-disk filesystem) */
    if (vol_handle == dev->handle) return 1;

    /* Check if the ISO volume is a partition on the target device.
     * If the volume handle has BlockIO with LogicalPartition=true,
     * and the target is removable, they might be the same physical disk.
     * Compare by checking if the volume handle's BlockIO parent matches. */
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO *vol_bio = NULL;
    EFI_STATUS status = g_boot.bs->HandleProtocol(
        vol_handle, &bio_guid, (VOID **)&vol_bio);
    if (!EFI_ERROR(status) && vol_bio && vol_bio->Media) {
        if (vol_bio->Media->LogicalPartition && dev->is_removable) {
            /* The volume is a partition. If the device is removable and
             * big enough to contain this partition, likely the same disk. */
            UINT64 vol_size = (UINT64)(vol_bio->Media->LastBlock + 1) *
                              (UINT64)vol_bio->Media->BlockSize;
            if (dev->size_bytes >= vol_size)
                return 1;
        }
    }

    return 0;
}

/* ---- Main ISO write function ---- */

int iso_write(EFI_FILE_HANDLE iso_root, const CHAR16 *iso_path,
              const char *iso_name, UINT64 iso_size,
              EFI_HANDLE iso_vol_handle) {

    /* Header screen */
    fb_clear(COLOR_BLACK);
    iso_print("\n", COLOR_WHITE);
    iso_print("  ========================================\n", COLOR_CYAN);
    iso_print("       WRITE ISO TO DEVICE\n", COLOR_CYAN);
    iso_print("  ========================================\n", COLOR_CYAN);
    iso_print("\n", COLOR_WHITE);

    /* Show ISO info */
    char buf[256];
    snprintf(buf, sizeof(buf), "  ISO: %s\n", iso_name);
    iso_print(buf, COLOR_WHITE);

    UINT64 size_mb = iso_size / (1024 * 1024);
    snprintf(buf, sizeof(buf), "  Size: %llu MB\n", (unsigned long long)size_mb);
    iso_print(buf, COLOR_WHITE);
    iso_print("\n", COLOR_WHITE);

    /* Enumerate block devices */
    struct disk_device devs[DISK_MAX_DEVICES];
    int ndevs = disk_enumerate(devs, DISK_MAX_DEVICES);

    if (ndevs == 0) {
        iso_print("  No block devices found.\n", COLOR_RED);
        iso_print("  Press any key to return.\n", COLOR_DGRAY);
        struct key_event ev;
        kbd_wait(&ev);
        return -1;
    }

    /* Show device list */
    iso_print("  Select target device:\n\n", COLOR_WHITE);

    int default_idx = -1;
    for (int i = 0; i < ndevs; i++) {
        /* Skip devices smaller than ISO */
        int too_small = (devs[i].size_bytes < iso_size) ? 1 : 0;
        int is_source = is_same_device(iso_vol_handle, &devs[i]);

        UINT32 color;
        if (too_small)
            color = COLOR_DGRAY;
        else if (is_source)
            color = COLOR_MAGENTA;
        else if (devs[i].is_boot_device)
            color = COLOR_RED;
        else
            color = COLOR_YELLOW;

        snprintf(buf, sizeof(buf), "  [%d] %s", i + 1, devs[i].name);
        iso_print(buf, color);

        if (is_source)
            iso_print(" * ISO SOURCE", COLOR_MAGENTA);
        else if (too_small)
            iso_print(" (too small)", COLOR_DGRAY);
        iso_print("\n", COLOR_WHITE);

        /* Pick first viable non-boot, non-source removable device as default */
        if (default_idx < 0 && !too_small && !devs[i].is_boot_device
            && !is_source && devs[i].is_removable)
            default_idx = i;
    }

    /* Fallback: any non-boot, non-too-small device */
    if (default_idx < 0) {
        for (int i = 0; i < ndevs; i++) {
            if (!devs[i].is_boot_device && devs[i].size_bytes >= iso_size) {
                default_idx = i;
                break;
            }
        }
    }

    if (default_idx < 0) {
        iso_print("\n  No suitable target device found.\n", COLOR_RED);
        iso_print("  Press any key to return.\n", COLOR_DGRAY);
        struct key_event ev;
        kbd_wait(&ev);
        return -1;
    }

    int target_idx = default_idx;
    iso_print("\n", COLOR_WHITE);
    snprintf(buf, sizeof(buf), "  Target: [%d] %s\n", target_idx + 1, devs[target_idx].name);
    iso_print(buf, COLOR_YELLOW);
    iso_print("  Press number to change, ENTER to confirm, ESC to cancel.\n", COLOR_DGRAY);

    /* Device selection loop */
    for (;;) {
        struct key_event ev;
        kbd_wait(&ev);
        if (ev.code == KEY_ESC) return -1;
        if (ev.code == KEY_ENTER) break;
        if (ev.code >= '1' && ev.code <= '0' + ndevs) {
            int idx = ev.code - '1';
            if (devs[idx].size_bytes >= iso_size) {
                target_idx = idx;
                snprintf(buf, sizeof(buf), "  Target: [%d] %s\n",
                         target_idx + 1, devs[target_idx].name);
                iso_print(buf, COLOR_YELLOW);
            }
        }
    }

    struct disk_device *target = &devs[target_idx];

    /* Same-device detection */
    int same_device = is_same_device(iso_vol_handle, target);
    EFI_FILE_HANDLE read_handle = NULL;
    EFI_FILE_HANDLE temp_handle = NULL;
    int using_temp = 0;
    UINT64 file_size = 0;

    if (same_device) {
        /* ISO is on the target device — must copy to boot volume first */
        iso_print("\n  ISO is on the target device!\n", COLOR_YELLOW);
        iso_print("  Copying to boot volume as temporary file...\n", COLOR_YELLOW);

        /* Check boot volume free space */
        EFI_FILE_HANDLE boot_root = fs_get_boot_root();
        UINT64 total_bytes, free_bytes;
        /* Temporarily switch to boot volume for volume info */
        EFI_FILE_HANDLE saved_root = boot_root;  /* just for clarity */
        fs_set_volume(saved_root);
        if (fs_volume_info(&total_bytes, &free_bytes) != 0 || free_bytes < iso_size) {
            fs_set_volume(iso_root);  /* restore */
            iso_print("  Not enough space on boot volume for temp copy.\n", COLOR_RED);
            iso_print("  Press any key to return.\n", COLOR_DGRAY);
            struct key_event ev;
            kbd_wait(&ev);
            return -1;
        }

        /* Open ISO for reading from original volume */
        EFI_FILE_HANDLE src = fs_open_read(iso_root, iso_path, &file_size);
        if (!src) {
            fs_set_volume(iso_root);
            iso_print("  Failed to open ISO file.\n", COLOR_RED);
            iso_print("  Press any key to return.\n", COLOR_DGRAY);
            struct key_event ev;
            kbd_wait(&ev);
            return -1;
        }

        /* Open temp file for writing on boot volume */
        static const CHAR16 temp_name[] = {'\\','_','_','i','s','o','_','t','e','m','p','_','_','.','i','s','o',0};
        temp_handle = fs_open_write(boot_root, temp_name);
        if (!temp_handle) {
            fs_stream_close(src);
            fs_set_volume(iso_root);
            iso_print("  Failed to create temp file.\n", COLOR_RED);
            iso_print("  Press any key to return.\n", COLOR_DGRAY);
            struct key_event ev;
            kbd_wait(&ev);
            return -1;
        }

        /* Stream copy ISO → temp file */
        UINT32 progress_row = g_boot.cursor_y + 1;
        void *chunk = mem_alloc(CHUNK_SIZE);
        if (!chunk) {
            fs_stream_close(src);
            fs_stream_close(temp_handle);
            iso_print("  Out of memory.\n", COLOR_RED);
            struct key_event ev;
            kbd_wait(&ev);
            return -1;
        }

        UINT64 copied = 0;
        while (copied < file_size) {
            UINTN to_read = CHUNK_SIZE;
            if (copied + to_read > file_size)
                to_read = (UINTN)(file_size - copied);

            if (fs_stream_read(src, chunk, &to_read) < 0 || to_read == 0)
                break;
            if (fs_stream_write(temp_handle, chunk, to_read) < 0)
                break;
            copied += to_read;
            show_copy_progress(copied, file_size, progress_row);
        }
        mem_free(chunk);
        fs_stream_close(src);
        fs_stream_close(temp_handle);

        if (copied < file_size) {
            fs_delete_file(boot_root, temp_name);
            iso_print("\n  Copy failed.\n", COLOR_RED);
            iso_print("  Press any key to return.\n", COLOR_DGRAY);
            struct key_event ev;
            kbd_wait(&ev);
            return -1;
        }

        /* Now read from the temp file on boot volume */
        read_handle = fs_open_read(boot_root, temp_name, &file_size);
        using_temp = 1;
        iso_print("\n  Temp copy complete.\n\n", COLOR_GREEN);
    } else {
        /* ISO is on a different device — read directly */
        read_handle = fs_open_read(iso_root, iso_path, &file_size);
    }

    if (!read_handle) {
        iso_print("  Failed to open ISO file.\n", COLOR_RED);
        iso_print("  Press any key to return.\n", COLOR_DGRAY);
        struct key_event ev;
        kbd_wait(&ev);
        return -1;
    }

    if (file_size == 0) {
        iso_print("  ISO file is empty (0 bytes).\n", COLOR_RED);
        iso_print("  Press any key to return.\n", COLOR_DGRAY);
        fs_stream_close(read_handle);
        if (using_temp) {
            static const CHAR16 temp_name[] = {'\\','_','_','i','s','o','_','t','e','m','p','_','_','.','i','s','o',0};
            fs_delete_file(fs_get_boot_root(), temp_name);
        }
        struct key_event ev;
        kbd_wait(&ev);
        return -1;
    }

    /* Confirmation */
    int is_boot = target->is_boot_device;
    if (is_boot) {
        iso_print("  !! WARNING: TARGET IS THE BOOT DEVICE !!\n", COLOR_RED);
        iso_print("  THIS WILL DESTROY THE WORKSTATION!\n", COLOR_RED);
        iso_print("\n  Type YES to confirm: ", COLOR_RED);

        /* Require typing Y, E, S */
        char confirm[4] = {0, 0, 0, 0};
        int ci = 0;
        while (ci < 3) {
            struct key_event ev;
            kbd_wait(&ev);
            if (ev.code == KEY_ESC) {
                fs_stream_close(read_handle);
                if (using_temp) {
                    static const CHAR16 temp_name[] = {'\\','_','_','i','s','o','_','t','e','m','p','_','_','.','i','s','o',0};
                    fs_delete_file(fs_get_boot_root(), temp_name);
                }
                return -1;
            }
            if (ev.code >= 0x20 && ev.code <= 0x7E) {
                char ch[2] = {(char)ev.code, 0};
                iso_print(ch, COLOR_RED);
                confirm[ci++] = (char)ev.code;
            }
        }
        if (!(confirm[0] == 'Y' && confirm[1] == 'E' && confirm[2] == 'S')) {
            iso_print("\n\n  Cancelled.\n", COLOR_WHITE);
            iso_print("  Press any key to return.\n", COLOR_DGRAY);
            fs_stream_close(read_handle);
            if (using_temp) {
                static const CHAR16 temp_name[] = {'\\','_','_','i','s','o','_','t','e','m','p','_','_','.','i','s','o',0};
                fs_delete_file(fs_get_boot_root(), temp_name);
            }
            struct key_event ev;
            kbd_wait(&ev);
            return -1;
        }
        iso_print("\n\n", COLOR_WHITE);
    } else {
        iso_print("\n  This will ERASE all data on the target device!\n", COLOR_RED);
        iso_print("  Press 'Y' to proceed, any other key to cancel.\n", COLOR_YELLOW);

        struct key_event ev;
        kbd_wait(&ev);
        if (ev.code != 'Y' && ev.code != 'y') {
            fs_stream_close(read_handle);
            if (using_temp) {
                static const CHAR16 temp_name[] = {'\\','_','_','i','s','o','_','t','e','m','p','_','_','.','i','s','o',0};
                fs_delete_file(fs_get_boot_root(), temp_name);
            }
            return -1;
        }
        iso_print("\n", COLOR_WHITE);
    }

    /* Streaming write loop */
    iso_print("  Writing ISO to device...\n", COLOR_WHITE);
    UINT32 progress_row = g_boot.cursor_y;

    void *chunk = mem_alloc(CHUNK_SIZE);
    if (!chunk) {
        iso_print("  Out of memory.\n", COLOR_RED);
        fs_stream_close(read_handle);
        struct key_event ev;
        kbd_wait(&ev);
        return -1;
    }

    UINT64 written = 0;
    UINT64 lba = 0;
    UINT32 block_size = target->block_size;
    int write_error = 0;

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

    mem_free(chunk);
    fs_stream_close(read_handle);

    /* Cleanup temp file if used */
    if (using_temp) {
        static const CHAR16 temp_name[] = {'\\','_','_','i','s','o','_','t','e','m','p','_','_','.','i','s','o',0};
        fs_delete_file(fs_get_boot_root(), temp_name);
    }

    /* Force firmware to re-probe the target device.
       The old SFS driver is stale — the FAT32 is gone. */
    disk_reconnect(target);

    if (write_error) {
        iso_print("\n\n  Write failed!\n", COLOR_RED);
        iso_print("  Press any key to return.\n", COLOR_DGRAY);
        struct key_event ev;
        kbd_wait(&ev);
        return -1;
    }

    /* Success */
    iso_print("\n\n", COLOR_WHITE);
    iso_print("  ========================================\n", COLOR_GREEN);
    iso_print("    ISO WRITTEN SUCCESSFULLY!\n", COLOR_GREEN);
    iso_print("  ========================================\n", COLOR_GREEN);
    iso_print("\n", COLOR_WHITE);

    snprintf(buf, sizeof(buf), "  Wrote %llu MB to %s\n",
             (unsigned long long)(written / (1024 * 1024)), target->name);
    iso_print(buf, COLOR_WHITE);

    iso_print("\n  The ISO has been written as raw disk data.\n", COLOR_WHITE);
    iso_print("  The target device is no longer a FAT32 volume.\n", COLOR_WHITE);

    if (is_boot) {
        iso_print("\n  The boot device has been overwritten.\n", COLOR_YELLOW);
        iso_print("  Reboot to start Linux.\n", COLOR_YELLOW);
    } else {
        iso_print("  Reboot to boot from it, or remove and\n", COLOR_WHITE);
        iso_print("  plug into another machine.\n", COLOR_WHITE);
    }

    iso_print("\n  Press any key to return.\n", COLOR_DGRAY);
    struct key_event ev;
    kbd_wait(&ev);
    return 0;
}
