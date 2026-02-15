/*
 * ntfs.h — NTFS filesystem driver (read-only)
 *
 * Portable: uses callback-based block I/O, no UEFI dependency.
 * Supports: file/dir read, path resolution, MFT parsing.
 * Does NOT support: compression, encryption, sparse files, ADS.
 */
#ifndef NTFS_H
#define NTFS_H

#include "boot.h"
#include "fs.h"

/* Block I/O callbacks */
typedef int (*ntfs_block_read_fn)(void *ctx, UINT64 lba, UINT32 count, void *buf);

/* Opaque volume handle */
struct ntfs_vol;

/* Mount an NTFS volume (read-only). Returns NULL on error.
   block_size is the underlying device block size (typically 512). */
struct ntfs_vol *ntfs_mount(ntfs_block_read_fn read_fn,
                             void *ctx, UINT32 block_size);

/* Unmount and free all resources */
void ntfs_unmount(struct ntfs_vol *vol);

/* Read directory contents. path is ASCII with '/' separators.
   "/" for root. Returns entry count, or -1 on error. */
int ntfs_readdir(struct ntfs_vol *vol, const char *path,
                 struct fs_entry *entries, int max_entries);

/* Read entire file into newly allocated buffer. Returns NULL on error. */
void *ntfs_readfile(struct ntfs_vol *vol, const char *path, UINTN *out_size);

/* Get volume info. Returns 0 on success. */
int ntfs_volume_info(struct ntfs_vol *vol, UINT64 *total_bytes, UINT64 *free_bytes);

/* Get file size. Returns 0 if not found. */
UINT64 ntfs_file_size(struct ntfs_vol *vol, const char *path);

/* Check if path exists. Returns 1 if yes. */
int ntfs_exists(struct ntfs_vol *vol, const char *path);

/* Get volume label (ASCII). Returns empty string if none. */
const char *ntfs_get_label(struct ntfs_vol *vol);

#endif /* NTFS_H */
