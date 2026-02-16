/*
 * fat32.h — FAT32 filesystem creation on an SD card partition
 *
 * Ported from src/fat32.c (Part 1). Key differences:
 *   - Uses stdint.h instead of UEFI types
 *   - Writes via sdcard_write() instead of disk_write_blocks()
 *   - partition_start_lba offsets all I/O (GPT partition, not superfloppy)
 *   - Streaming write support: pre-allocate cluster chain, write chunks
 */
#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>

/* Progress callback: called periodically during long operations.
 * current/total are in arbitrary units (e.g., sectors zeroed). */
typedef void (*fat32_progress_cb)(int current, int total);

/* Format a FAT32 filesystem within a partition.
 * partition_start_lba: starting LBA on the SD card (e.g., 2048 for GPT)
 * partition_sectors: total sectors in the partition
 * progress: optional callback for FAT zeroing progress (may be NULL)
 * Returns 0 on success. */
int fat32_format(uint32_t partition_start_lba, uint32_t partition_sectors,
                 fat32_progress_cb progress);

/* Create a directory path (e.g., "EFI/BOOT"). Creates parents as needed.
 * Returns 0 on success. */
int fat32_mkdir(const char *path);

/* Write a complete file. Creates parent directories as needed.
 * Returns 0 on success. */
int fat32_write_file(const char *path, const void *data, uint32_t size);

/* --- Streaming write API (for decompressing large files) --- */

/* Begin a streaming write: create the directory entry and pre-allocate
 * a cluster chain for 'size' bytes. Returns a handle (>0) or -1. */
int fat32_stream_open(const char *path, uint32_t size);

/* Write a chunk to the stream. Must be called in order. Returns 0 on success. */
int fat32_stream_write(int handle, const void *data, uint32_t len);

/* Finalize the stream. Returns 0 on success. */
int fat32_stream_close(int handle);

#endif /* FAT32_H */
