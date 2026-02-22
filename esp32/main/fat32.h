/*
 * fat32.h — FAT32 filesystem on an SD card partition
 *
 * Ported from src/fat32.c (Part 1 bare-metal UEFI workstation).
 *   - Uses stdint.h instead of UEFI types
 *   - Writes via sdcard_write() instead of disk_write_blocks()
 *   - partition_start_lba offsets all I/O (GPT partition, not superfloppy)
 *   - Streaming write support: pre-allocate cluster chain, write chunks
 *   - Read support: directory listing and file reading (Chapter 36)
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

/* --- Reading API (Chapter 36) --- */

/* Directory entry info returned by fat32_read_dir() */
struct fat32_dir_info {
    char     name[128];    /* long filename or reconstructed 8.3 */
    uint32_t size;         /* file size in bytes (0 for dirs) */
    uint8_t  is_dir;       /* 1 if directory */
};

/* Initialize for reading an existing FAT32 partition (parses BPB).
 * partition_start_lba: starting LBA on the SD card.
 * Returns 0 on success. */
int fat32_read_init(uint32_t partition_start_lba);

/* Open a directory for enumeration. "" or "/" for root.
 * Returns a handle (>0) or -1 on error. */
int fat32_open_dir(const char *path);

/* Read next entry. Returns 1=got entry, 0=end, -1=error. */
int fat32_read_dir(int handle, struct fat32_dir_info *info);

/* Close directory handle. */
void fat32_close_dir(int handle);

/* Open a file for sequential reading.
 * Returns a handle (>0) or -1 on error. */
int fat32_file_open(const char *path);

/* Read up to len bytes. Returns bytes read, 0=EOF, -1=error. */
int fat32_file_read(int handle, void *buf, uint32_t len);

/* Close file handle. */
void fat32_file_close(int handle);

/* --- Streaming write API (for decompressing large files) --- */

/* Begin a streaming write: create the directory entry and pre-allocate
 * a cluster chain for 'size' bytes. Returns a handle (>0) or -1. */
int fat32_stream_open(const char *path, uint32_t size);

/* Write a chunk to the stream. Must be called in order. Returns 0 on success. */
int fat32_stream_write(int handle, const void *data, uint32_t len);

/* Finalize the stream. Returns 0 on success. */
int fat32_stream_close(int handle);

/* Get volume total and free space. Returns 0 on success. */
int fat32_volume_info(uint64_t *total_bytes, uint64_t *free_bytes);

#endif /* FAT32_H */
