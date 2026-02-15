/*
 * exfat.h — exFAT filesystem driver (read/write)
 *
 * Portable: uses callback-based block I/O, no UEFI dependency.
 */
#ifndef EXFAT_H
#define EXFAT_H

#include "boot.h"
#include "fs.h"

/* Block I/O callbacks */
typedef int (*exfat_block_read_fn)(void *ctx, UINT64 lba, UINT32 count, void *buf);
typedef int (*exfat_block_write_fn)(void *ctx, UINT64 lba, UINT32 count, const void *buf);

/* Opaque volume handle */
struct exfat_vol;

/* Mount an exFAT volume. Returns NULL on error.
   block_size is the underlying device block size (typically 512). */
struct exfat_vol *exfat_mount(exfat_block_read_fn read_fn,
                               exfat_block_write_fn write_fn,
                               void *ctx, UINT32 block_size);

/* Unmount and free all resources */
void exfat_unmount(struct exfat_vol *vol);

/* Read directory contents. path is ASCII with '/' separators.
   "/" for root. Returns entry count, or -1 on error. */
int exfat_readdir(struct exfat_vol *vol, const char *path,
                  struct fs_entry *entries, int max_entries);

/* Read entire file into newly allocated buffer. Returns NULL on error. */
void *exfat_readfile(struct exfat_vol *vol, const char *path, UINTN *out_size);

/* Write data to a file (create or replace). Returns 0 on success. */
int exfat_writefile(struct exfat_vol *vol, const char *path,
                    const void *data, UINTN size);

/* Create a directory. Returns 0 on success. */
int exfat_mkdir(struct exfat_vol *vol, const char *path);

/* Rename a file/dir. new_name is just the filename. Returns 0 on success. */
int exfat_rename(struct exfat_vol *vol, const char *path, const char *new_name);

/* Delete a file. Returns 0 on success. */
int exfat_delete(struct exfat_vol *vol, const char *path);

/* Get volume info. Returns 0 on success. */
int exfat_volume_info(struct exfat_vol *vol, UINT64 *total_bytes, UINT64 *free_bytes);

/* Get file size. Returns 0 if not found. */
UINT64 exfat_file_size(struct exfat_vol *vol, const char *path);

/* Check if path exists. Returns 1 if yes. */
int exfat_exists(struct exfat_vol *vol, const char *path);

/* Get volume label (ASCII). Returns empty string if none. */
const char *exfat_get_label(struct exfat_vol *vol);

#endif /* EXFAT_H */
