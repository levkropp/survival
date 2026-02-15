/*
 * fat32.h — FAT32 filesystem creation on block devices
 */
#ifndef FAT32_H
#define FAT32_H

#include "disk.h"

/* Format a block device as FAT32 with MBR. Returns 0 on success. */
int fat32_format(struct disk_device *dev);

/* Write a file to a FAT32-formatted device. Path uses backslashes.
 * Creates parent directories as needed. Returns 0 on success. */
int fat32_write_file(struct disk_device *dev, const char *path,
                     void *data, UINT64 size);

/* Create a directory on a FAT32-formatted device. Returns 0 on success. */
int fat32_mkdir(struct disk_device *dev, const char *path);

#endif /* FAT32_H */
