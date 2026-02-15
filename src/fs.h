#ifndef FS_H
#define FS_H

#include "boot.h"

#define FS_MAX_NAME    128
#define FS_MAX_ENTRIES 256

/* Volume type for dispatch */
enum fs_vol_type { FS_VOL_SFS, FS_VOL_EXFAT, FS_VOL_NTFS };

/* Custom volume descriptor (exFAT/NTFS found on BlockIO handles) */
struct fs_custom_volume {
    EFI_HANDLE      handle;
    enum fs_vol_type type;
    char            label[48];
    UINT64          size_bytes;
};

/* A single directory entry (pre-converted to ASCII) */
struct fs_entry {
    char     name[FS_MAX_NAME];
    UINT64   size;
    UINT8    is_dir;
};

/* Initialize filesystem — opens the boot volume.
   Call after g_boot is set up. */
EFI_STATUS fs_init(void);

/* Read directory contents into caller-provided array.
   path is CHAR16, e.g. L"\\" for root or L"\\EFI\\BOOT".
   Returns number of entries, or -1 on error.
   Entries are sorted: directories first, then alphabetical. */
int fs_readdir(const CHAR16 *path, struct fs_entry *entries, int max_entries);

/* Read entire file into a newly allocated buffer.
   Caller must mem_free() the returned pointer.
   Sets *out_size to file size. Returns NULL on error. */
void *fs_readfile(const CHAR16 *path, UINTN *out_size);

/* Write data to a file, creating or replacing it.
   Uses delete-and-recreate to ensure clean write. */
EFI_STATUS fs_writefile(const CHAR16 *path, const void *data, UINTN size);

/* Get volume size and free space in bytes.
   Returns 0 on success, -1 on error. */
int fs_volume_info(UINT64 *total_bytes, UINT64 *free_bytes);

/* Get file size in bytes. Returns 0 if file doesn't exist. */
UINT64 fs_file_size(const CHAR16 *path);

/* Check if a file or directory exists. Returns 1 if yes, 0 if no. */
int fs_exists(const CHAR16 *path);

/* Rename a file. new_name is just the filename, not a full path. */
EFI_STATUS fs_rename(const CHAR16 *path, const CHAR16 *new_name);

/* Create a directory (single component or full path).
   Returns EFI_SUCCESS if created or already exists. */
EFI_STATUS fs_mkdir(const CHAR16 *path);

/* ---- Streaming file I/O ---- */

/* Open a file for streaming read on a specific volume root (NULL = current).
   Returns file handle and sets *out_size to file size. NULL on error. */
EFI_FILE_HANDLE fs_open_read(EFI_FILE_HANDLE root, const CHAR16 *path, UINT64 *out_size);

/* Open a file for streaming write (delete-and-recreate).
   root=NULL uses current s_root. Returns file handle, NULL on error. */
EFI_FILE_HANDLE fs_open_write(EFI_FILE_HANDLE root, const CHAR16 *path);

/* Read up to *size bytes from a streaming handle. Updates *size to actual bytes read.
   Returns 0 on success, -1 on error. *size=0 means EOF. */
int fs_stream_read(EFI_FILE_HANDLE file, void *buf, UINTN *size);

/* Write size bytes to a streaming handle. Returns 0 on success, -1 on error. */
int fs_stream_write(EFI_FILE_HANDLE file, const void *buf, UINTN size);

/* Flush and close a streaming handle. */
void fs_stream_close(EFI_FILE_HANDLE file);

/* Delete a file on a specific volume root (NULL = current). */
EFI_STATUS fs_delete_file(EFI_FILE_HANDLE root, const CHAR16 *path);

/* Get the boot volume root handle. */
EFI_FILE_HANDLE fs_get_boot_root(void);

/* ---- USB volume discovery ---- */

#define FS_MAX_USB 8

struct fs_usb_volume {
    EFI_HANDLE handle;
    EFI_FILE_HANDLE root;
    char label[48];
};

/* Enumerate removable USB volumes (excludes boot volume).
   Returns count found (up to max). */
int fs_enumerate_usb(struct fs_usb_volume *vols, int max);

/* Switch filesystem operations to a different volume root */
void fs_set_volume(EFI_FILE_HANDLE new_root);

/* Restore to the boot volume root */
void fs_restore_boot_volume(void);

/* ---- Custom volume support (exFAT/NTFS) ---- */

/* Mount a custom (non-SFS) volume via BlockIO handle.
   Returns 0 on success, -1 on error. */
int fs_set_custom_volume(enum fs_vol_type type, EFI_HANDLE handle);

/* Returns 1 if current volume is read-only (NTFS), 0 otherwise */
int fs_is_read_only(void);

/* Query current volume type */
enum fs_vol_type fs_get_vol_type(void);

/* Enumerate exFAT/NTFS volumes on all BlockIO handles.
   Returns count found (up to max). */
int fs_enumerate_custom_volumes(struct fs_custom_volume *vols, int max);

/* Check if a handle's block device has a valid FAT32 boot sector */
int fs_has_valid_fat32(EFI_HANDLE handle);

#endif /* FS_H */
