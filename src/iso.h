/*
 * iso.h — ISO image writer for Survival Workstation
 *
 * Writes a .iso file directly to a block device, creating
 * a bootable Linux USB from a hybrid ISO image.
 */
#ifndef ISO_H
#define ISO_H

#include "boot.h"

/* Write an ISO file to a block device.
 * iso_root:       volume root handle where the ISO file lives (NULL = current)
 * iso_path:       CHAR16 path to the ISO file on that volume
 * iso_name:       ASCII display name of the ISO file
 * iso_size:       size of the ISO file in bytes
 * iso_vol_handle: EFI_HANDLE of the volume the ISO is on (for same-device detection)
 *
 * Returns 0 on success, -1 on error/cancel.
 */
int iso_write(EFI_FILE_HANDLE iso_root, const CHAR16 *iso_path,
              const char *iso_name, UINT64 iso_size,
              EFI_HANDLE iso_vol_handle);

#endif /* ISO_H */
