/*
 * exfat.h — exFAT filesystem driver (read/write)
 *
 * Ported from src/exfat.c (Part 1 bare-metal UEFI workstation).
 * Changes from the original:
 *   - UINT8/UINT16/UINT32/UINT64/UINTN → stdint.h / size_t
 *   - mem_alloc/mem_free → malloc/free
 *   - mem_set/mem_copy → memset/memcpy
 *   - struct fs_entry → struct exfat_dir_info
 *   - Added exfat_mount_sdcard() convenience wrapper for ESP32
 */
#ifndef EXFAT_H
#define EXFAT_H

#include <stdint.h>
#include <stddef.h>

/* Block I/O callbacks */
typedef int (*exfat_block_read_fn)(void *ctx, uint64_t lba, uint32_t count,
                                   void *buf);
typedef int (*exfat_block_write_fn)(void *ctx, uint64_t lba, uint32_t count,
                                    const void *buf);

/* Opaque volume handle */
struct exfat_vol;

#define EXFAT_MAX_NAME 128

/* Directory entry (pre-converted to ASCII) */
struct exfat_dir_info {
    char     name[EXFAT_MAX_NAME];
    uint64_t size;
    uint8_t  is_dir;
};

/* Mount an exFAT volume via callbacks. Returns NULL on error.
 * block_size is the underlying device block size (typically 512). */
struct exfat_vol *exfat_mount(exfat_block_read_fn read_fn,
                               exfat_block_write_fn write_fn,
                               void *ctx, uint32_t block_size);

/* Unmount and free all resources. */
void exfat_unmount(struct exfat_vol *vol);

/* Convenience: mount exFAT from an SD card GPT partition.
 * All I/O is offset by partition_start_lba.
 * Only one SD-card volume can be mounted at a time. */
struct exfat_vol *exfat_mount_sdcard(uint32_t partition_start_lba);

/* Read directory contents. "/" for root.
 * Returns entry count, or -1 on error. */
int exfat_readdir(struct exfat_vol *vol, const char *path,
                  struct exfat_dir_info *entries, int max_entries);

/* Read entire file into malloc'd buffer. Caller must free().
 * Returns NULL on error. */
void *exfat_readfile(struct exfat_vol *vol, const char *path,
                     size_t *out_size);

/* Write data to a file (create or replace). Returns 0 on success. */
int exfat_writefile(struct exfat_vol *vol, const char *path,
                    const void *data, size_t size);

/* Create a directory (and parents). Returns 0 on success. */
int exfat_mkdir(struct exfat_vol *vol, const char *path);

/* Rename a file/dir. new_name is just the filename. Returns 0 on success. */
int exfat_rename(struct exfat_vol *vol, const char *path,
                 const char *new_name);

/* Delete a file or empty directory. Returns 0 on success. */
int exfat_delete(struct exfat_vol *vol, const char *path);

/* Get volume total and free space. Returns 0 on success. */
int exfat_volume_info(struct exfat_vol *vol, uint64_t *total_bytes,
                      uint64_t *free_bytes);

/* Get file size in bytes. Returns 0 if not found. */
uint64_t exfat_file_size(struct exfat_vol *vol, const char *path);

/* Check if path exists. Returns 1 if yes. */
int exfat_exists(struct exfat_vol *vol, const char *path);

/* Get volume label (ASCII). Returns empty string if none. */
const char *exfat_get_label(struct exfat_vol *vol);

#endif /* EXFAT_H */
