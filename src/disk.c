/*
 * disk.c — Block device enumeration and I/O via UEFI BlockIO protocol
 *
 * Enumerates all block devices, marks boot device as protected,
 * provides raw block read/write for FAT32 formatting and USB writing.
 */

#include "boot.h"
#include "mem.h"
#include "disk.h"

/* Get the boot partition handle via LoadedImage protocol */
static EFI_HANDLE get_boot_partition(void) {
    EFI_GUID li_guid = LOADED_IMAGE_PROTOCOL;
    EFI_LOADED_IMAGE *li = NULL;
    EFI_STATUS status;

    status = g_boot.bs->HandleProtocol(
        g_boot.image_handle, &li_guid, (VOID **)&li);
    if (EFI_ERROR(status) || !li)
        return NULL;
    return li->DeviceHandle;
}

/* Get length of a device path (excluding the end-of-path node).
 * End-of-path type is 0x7f per UEFI spec. */
static UINTN devpath_prefix_len(EFI_DEVICE_PATH *dp) {
    UINTN len = 0;
    while (dp->Type != 0x7f) {
        UINTN node_len = dp->Length[0] | ((UINTN)dp->Length[1] << 8);
        if (node_len < 4) break;  /* malformed */
        len += node_len;
        dp = (EFI_DEVICE_PATH *)((UINT8 *)dp + node_len);
    }
    return len;
}

/*
 * Check if a whole-disk handle is the parent of a partition handle.
 * Compares device paths: the partition's path should start with the
 * disk's path (its prefix), followed by a partition node (HD()).
 */
static int disk_is_parent_of(EFI_HANDLE disk, EFI_HANDLE partition) {
    if (!disk || !partition) return 0;
    if (disk == partition) return 1;

    EFI_GUID dp_guid = { 0x9576e91, 0x6d3f, 0x11d2,
        {0x8e, 0x39, 0x0, 0xa0, 0xc9, 0x69, 0x72, 0x3b} };
    EFI_DEVICE_PATH *disk_dp = NULL, *part_dp = NULL;

    g_boot.bs->HandleProtocol(disk, &dp_guid, (void **)&disk_dp);
    g_boot.bs->HandleProtocol(partition, &dp_guid, (void **)&part_dp);
    if (!disk_dp || !part_dp) return 0;

    UINTN disk_len = devpath_prefix_len(disk_dp);
    UINTN part_len = devpath_prefix_len(part_dp);
    if (disk_len == 0 || part_len <= disk_len) return 0;

    /* The partition's device path must start with the disk's device path */
    UINT8 *db = (UINT8 *)disk_dp;
    UINT8 *pb = (UINT8 *)part_dp;
    for (UINTN i = 0; i < disk_len; i++) {
        if (db[i] != pb[i]) return 0;
    }
    return 1;
}

/*
 * Check if a whole-disk handle has any partition matching one of the given
 * handles. Used to deduplicate [DISK] entries against [USB], [exFAT], etc.
 */
int disk_has_claimed_partition(EFI_HANDLE disk, EFI_HANDLE *claimed, int nclaimed) {
    for (int i = 0; i < nclaimed; i++) {
        if (disk_is_parent_of(disk, claimed[i]))
            return 1;
    }
    return 0;
}

/* Format size as human-readable string */
static void format_size(UINT64 bytes, char *buf) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    UINT64 val = bytes;
    while (val >= 1024 && u < 4) { val /= 1024; u++; }

    /* Simple integer formatting */
    char tmp[24];
    int len = 0;
    UINT64 v = val;
    if (v == 0) tmp[len++] = '0';
    else {
        while (v > 0) { tmp[len++] = '0' + (int)(v % 10); v /= 10; }
    }
    int pos = 0;
    for (int i = len - 1; i >= 0; i--) buf[pos++] = tmp[i];
    buf[pos++] = ' ';
    for (int i = 0; units[u][i]; i++) buf[pos++] = units[u][i];
    buf[pos] = '\0';
}

int disk_enumerate(struct disk_device *devs, int max) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_STATUS status;
    UINTN handle_count = 0;
    EFI_HANDLE *handles = NULL;
    int count = 0;

    EFI_HANDLE boot_part = get_boot_partition();

    status = g_boot.bs->LocateHandleBuffer(
        ByProtocol, &bio_guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || !handles)
        return 0;

    for (UINTN i = 0; i < handle_count && count < max; i++) {
        EFI_BLOCK_IO *bio = NULL;
        status = g_boot.bs->HandleProtocol(
            handles[i], &bio_guid, (VOID **)&bio);
        if (EFI_ERROR(status) || !bio)
            continue;

        EFI_BLOCK_IO_MEDIA *media = bio->Media;
        if (!media || !media->MediaPresent)
            continue;

        /* Skip logical partitions — we want whole-disk devices */
        if (media->LogicalPartition)
            continue;

        struct disk_device *d = &devs[count];
        mem_set(d, 0, sizeof(*d));
        d->handle = handles[i];
        d->block_io = bio;
        d->block_size = media->BlockSize;
        d->media_id = media->MediaId;
        d->size_bytes = (UINT64)(media->LastBlock + 1) * (UINT64)media->BlockSize;
        d->is_removable = media->RemovableMedia ? 1 : 0;
        d->is_boot_device = disk_is_parent_of(handles[i], boot_part) ? 1 : 0;

        /* Build descriptive name */
        char sizebuf[32];
        format_size(d->size_bytes, sizebuf);

        int pos = 0;
        if (d->is_removable) {
            const char *r = "USB ";
            while (*r) d->name[pos++] = *r++;
        } else {
            const char *r = "Disk ";
            while (*r) d->name[pos++] = *r++;
        }
        for (int j = 0; sizebuf[j] && pos < 60; j++)
            d->name[pos++] = sizebuf[j];
        if (d->is_boot_device && pos < 52) {
            const char *tag = " [BOOT]";
            while (*tag && pos < 63) d->name[pos++] = *tag++;
        }
        d->name[pos] = '\0';

        count++;
    }

    g_boot.bs->FreePool(handles);
    return count;
}

int disk_write_blocks(struct disk_device *dev, UINT64 lba, UINT64 count, void *buf) {
    if (!dev || !dev->block_io || dev->is_boot_device)
        return -1;

    UINTN buf_size = (UINTN)(count * (UINT64)dev->block_size);
    EFI_STATUS status = dev->block_io->WriteBlocks(
        dev->block_io, dev->media_id, (EFI_LBA)lba, buf_size, buf);

    if (EFI_ERROR(status))
        return -1;

    /* Flush */
    dev->block_io->FlushBlocks(dev->block_io);
    return 0;
}

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

void disk_reconnect(struct disk_device *dev) {
    if (!dev || !dev->handle) return;

    /* Disconnect old drivers first — drops any stale cached filesystem
     * state (e.g. SFS from boot that has old FAT data in memory).
     * Then reconnect so DiskIo + FAT driver bind fresh to the new data. */
    g_boot.bs->DisconnectController(dev->handle, NULL, NULL);
    g_boot.bs->ConnectController(dev->handle, NULL, NULL, TRUE);
}

int disk_read_blocks(struct disk_device *dev, UINT64 lba, UINT64 count, void *buf) {
    if (!dev || !dev->block_io)
        return -1;

    UINTN buf_size = (UINTN)(count * (UINT64)dev->block_size);
    EFI_STATUS status = dev->block_io->ReadBlocks(
        dev->block_io, dev->media_id, (EFI_LBA)lba, buf_size, buf);

    return EFI_ERROR(status) ? -1 : 0;
}
