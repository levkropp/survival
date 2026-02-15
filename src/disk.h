/*
 * disk.h — Block device enumeration and I/O via UEFI BlockIO protocol
 */
#ifndef DISK_H
#define DISK_H

#include "boot.h"

#define DISK_MAX_DEVICES 16

struct disk_device {
    EFI_HANDLE          handle;
    EFI_BLOCK_IO        *block_io;
    UINT64              size_bytes;
    UINT32              block_size;
    UINT32              media_id;
    char                name[64];
    int                 is_removable;
    int                 is_boot_device; /* don't write to this! */
};

/* Enumerate block devices. Returns count found (up to max). */
int disk_enumerate(struct disk_device *devs, int max);

/* Write blocks to device. Returns 0 on success, -1 on error. */
int disk_write_blocks(struct disk_device *dev, UINT64 lba, UINT64 count, void *buf);

/* Read blocks from device. Returns 0 on success, -1 on error. */
int disk_read_blocks(struct disk_device *dev, UINT64 lba, UINT64 count, void *buf);

/* Force UEFI to re-probe a device (disconnect and reconnect drivers).
   Call after raw block writes that change or destroy the device's filesystem. */
void disk_reconnect(struct disk_device *dev);

/* Write blocks, bypassing the boot device safety check.
   Only for confirmed destructive operations (ISO write to boot device). */
int disk_write_blocks_force(struct disk_device *dev, UINT64 lba, UINT64 count, void *buf);

/* Check if a whole-disk handle has any partition matching one of the given
   handles.  Used to deduplicate [DISK] entries against USB/exFAT/NTFS. */
int disk_has_claimed_partition(EFI_HANDLE disk, EFI_HANDLE *claimed, int nclaimed);

#endif /* DISK_H */
