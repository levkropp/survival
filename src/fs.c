#include "fs.h"
#include "mem.h"
#include "exfat.h"
#include "ntfs.h"
#include "disk.h"

/* Root directory handle for the boot volume */
static EFI_FILE_HANDLE s_root;
static EFI_FILE_HANDLE s_boot_root;  /* saved boot volume root */
static EFI_HANDLE s_boot_device;     /* boot device handle */

/* Custom volume state */
static enum fs_vol_type s_vol_type = FS_VOL_SFS;
static struct exfat_vol *s_exfat = NULL;
static struct ntfs_vol *s_ntfs = NULL;
static EFI_HANDLE s_custom_handle = NULL;  /* BlockIO handle for custom vol */

/* Convert CHAR16 string to ASCII (truncates to 7-bit) */
static void char16_to_ascii(const CHAR16 *src, char *dst, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

/* Convert CHAR16 path to ASCII path with '/' separators */
static void path_to_ascii(const CHAR16 *src, char *dst, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        char c = (char)(src[i] & 0x7F);
        dst[i] = (c == '\\') ? '/' : c;
        i++;
    }
    dst[i] = '\0';
}

/* Case-insensitive ASCII comparison for sorting */
static int name_cmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)(UINT8)ca - (int)(UINT8)cb;
        a++; b++;
    }
    return (int)(UINT8)*a - (int)(UINT8)*b;
}

/* Sort entries: directories first, then alphabetical */
static void sort_entries(struct fs_entry *entries, int count) {
    /* Simple insertion sort — fine for <= 256 entries */
    for (int i = 1; i < count; i++) {
        struct fs_entry tmp;
        mem_copy(&tmp, &entries[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0) {
            int swap = 0;
            if (tmp.is_dir && !entries[j].is_dir) {
                swap = 1;
            } else if (tmp.is_dir == entries[j].is_dir) {
                if (name_cmp(tmp.name, entries[j].name) < 0)
                    swap = 1;
            }
            if (!swap) break;
            mem_copy(&entries[j + 1], &entries[j], sizeof(tmp));
            j--;
        }
        mem_copy(&entries[j + 1], &tmp, sizeof(tmp));
    }
}

/* ---- BlockIO callback wrappers for custom drivers ---- */

struct bio_ctx {
    EFI_BLOCK_IO *bio;
    UINT32 media_id;
};

static struct bio_ctx s_bio_ctx;

static int bio_read_cb(void *ctx, UINT64 lba, UINT32 count, void *buf) {
    struct bio_ctx *bc = (struct bio_ctx *)ctx;
    UINTN size = (UINTN)count * (UINTN)bc->bio->Media->BlockSize;
    EFI_STATUS st = bc->bio->ReadBlocks(bc->bio, bc->media_id,
                                         (EFI_LBA)lba, size, buf);
    return EFI_ERROR(st) ? -1 : 0;
}

static int bio_write_cb(void *ctx, UINT64 lba, UINT32 count, const void *buf) {
    struct bio_ctx *bc = (struct bio_ctx *)ctx;
    UINTN size = (UINTN)count * (UINTN)bc->bio->Media->BlockSize;
    EFI_STATUS st = bc->bio->WriteBlocks(bc->bio, bc->media_id,
                                          (EFI_LBA)lba, size, (void *)buf);
    if (EFI_ERROR(st)) return -1;
    bc->bio->FlushBlocks(bc->bio);
    return 0;
}

/* ---- Unmount any active custom volume ---- */

static void unmount_custom(void) {
    if (s_exfat) {
        exfat_unmount(s_exfat);
        s_exfat = NULL;
    }
    if (s_ntfs) {
        ntfs_unmount(s_ntfs);
        s_ntfs = NULL;
    }
    s_vol_type = FS_VOL_SFS;
    s_custom_handle = NULL;
}

/* ---- Public API ---- */

EFI_STATUS fs_init(void) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;

    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    /* Get the loaded image protocol to find our boot device */
    status = g_boot.bs->HandleProtocol(
        g_boot.image_handle, &li_guid, (void **)&loaded_image);
    if (EFI_ERROR(status))
        return status;

    /* Get the filesystem protocol from the boot device */
    status = g_boot.bs->HandleProtocol(
        loaded_image->DeviceHandle, &sfs_guid, (void **)&sfs);
    if (EFI_ERROR(status))
        return status;

    /* Save boot device handle for USB enumeration */
    s_boot_device = loaded_image->DeviceHandle;

    /* Open the root directory */
    status = sfs->OpenVolume(sfs, &s_root);
    if (!EFI_ERROR(status))
        s_boot_root = s_root;
    return status;
}

int fs_readdir(const CHAR16 *path, struct fs_entry *entries, int max_entries) {
    /* Dispatch to custom driver */
    if (s_vol_type == FS_VOL_EXFAT && s_exfat) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return exfat_readdir(s_exfat, apath, entries, max_entries);
    }
    if (s_vol_type == FS_VOL_NTFS && s_ntfs) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return ntfs_readdir(s_ntfs, apath, entries, max_entries);
    }

    /* SFS path */
    EFI_STATUS status;
    EFI_FILE_HANDLE dir = NULL;
    int count = 0;

    /* Open the directory */
    status = s_root->Open(s_root, &dir, (CHAR16 *)path,
                          EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status))
        return -1;

    /* Allocate scratch buffer for variable-size EFI_FILE_INFO */
    UINTN buf_size = 1024;
    void *buf = mem_alloc(buf_size);
    if (!buf) {
        dir->Close(dir);
        return -1;
    }

    /* Read entries one at a time */
    for (;;) {
        UINTN read_size = buf_size;
        status = dir->Read(dir, &read_size, buf);

        if (EFI_ERROR(status))
            break;
        if (read_size == 0)
            break;  /* no more entries */

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;

        /* Skip "." and ".." */
        if (info->FileName[0] == L'.' &&
            (info->FileName[1] == 0 ||
             (info->FileName[1] == L'.' && info->FileName[2] == 0)))
            continue;

        if (count >= max_entries)
            break;

        char16_to_ascii(info->FileName, entries[count].name, FS_MAX_NAME);
        entries[count].size = info->FileSize;
        entries[count].is_dir = (info->Attribute & EFI_FILE_DIRECTORY) ? 1 : 0;
        count++;
    }

    mem_free(buf);
    dir->Close(dir);

    sort_entries(entries, count);
    return count;
}

void *fs_readfile(const CHAR16 *path, UINTN *out_size) {
    /* Dispatch to custom driver */
    if (s_vol_type == FS_VOL_EXFAT && s_exfat) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return exfat_readfile(s_exfat, apath, out_size);
    }
    if (s_vol_type == FS_VOL_NTFS && s_ntfs) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return ntfs_readfile(s_ntfs, apath, out_size);
    }

    /* SFS path */
    EFI_STATUS status;
    EFI_FILE_HANDLE file = NULL;

    *out_size = 0;

    /* Guard against uninitialized filesystem */
    if (!s_root)
        return NULL;

    /* Open the file */
    status = s_root->Open(s_root, &file, (CHAR16 *)path,
                          EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status))
        return NULL;

    /* Get file size via GetInfo */
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    EFI_FILE_INFO *info = NULL;

    /* First call to get required buffer size */
    file->GetInfo(file, &info_guid, &info_size, NULL);
    info = (EFI_FILE_INFO *)mem_alloc(info_size);
    if (!info) {
        file->Close(file);
        return NULL;
    }

    status = file->GetInfo(file, &info_guid, &info_size, info);
    if (EFI_ERROR(status)) {
        mem_free(info);
        file->Close(file);
        return NULL;
    }

    UINT64 file_size = info->FileSize;
    mem_free(info);

    if (file_size == 0) {
        file->Close(file);
        return NULL;
    }

    /* Allocate buffer and read file */
    void *data = mem_alloc((UINTN)file_size);
    if (!data) {
        file->Close(file);
        return NULL;
    }

    UINTN read_size = (UINTN)file_size;
    status = file->Read(file, &read_size, data);
    file->Close(file);

    if (EFI_ERROR(status)) {
        mem_free(data);
        return NULL;
    }

    *out_size = read_size;
    return data;
}

int fs_volume_info(UINT64 *total_bytes, UINT64 *free_bytes) {
    if (s_vol_type == FS_VOL_EXFAT && s_exfat)
        return exfat_volume_info(s_exfat, total_bytes, free_bytes);
    if (s_vol_type == FS_VOL_NTFS && s_ntfs)
        return ntfs_volume_info(s_ntfs, total_bytes, free_bytes);

    if (!s_root) return -1;

    EFI_GUID fsi_guid = EFI_FILE_SYSTEM_INFO_ID;
    UINTN buf_size = 0;

    /* First call to get required size */
    s_root->GetInfo(s_root, &fsi_guid, &buf_size, NULL);
    if (buf_size == 0) return -1;

    EFI_FILE_SYSTEM_INFO *info = (EFI_FILE_SYSTEM_INFO *)mem_alloc(buf_size);
    if (!info) return -1;

    EFI_STATUS status = s_root->GetInfo(s_root, &fsi_guid, &buf_size, info);
    if (EFI_ERROR(status)) {
        mem_free(info);
        return -1;
    }

    *total_bytes = info->VolumeSize;
    *free_bytes = info->FreeSpace;
    mem_free(info);
    return 0;
}

UINT64 fs_file_size(const CHAR16 *path) {
    if (s_vol_type == FS_VOL_EXFAT && s_exfat) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return exfat_file_size(s_exfat, apath);
    }
    if (s_vol_type == FS_VOL_NTFS && s_ntfs) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return ntfs_file_size(s_ntfs, apath);
    }

    if (!s_root) return 0;

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = s_root->Open(s_root, &file, (CHAR16 *)path,
                                      EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return 0;

    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    file->GetInfo(file, &info_guid, &info_size, NULL);
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)mem_alloc(info_size);
    if (!info) { file->Close(file); return 0; }

    status = file->GetInfo(file, &info_guid, &info_size, info);
    UINT64 size = EFI_ERROR(status) ? 0 : info->FileSize;
    mem_free(info);
    file->Close(file);
    return size;
}

int fs_exists(const CHAR16 *path) {
    if (s_vol_type == FS_VOL_EXFAT && s_exfat) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return exfat_exists(s_exfat, apath);
    }
    if (s_vol_type == FS_VOL_NTFS && s_ntfs) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return ntfs_exists(s_ntfs, apath);
    }

    if (!s_root) return 0;
    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = s_root->Open(s_root, &file, (CHAR16 *)path,
                                      EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return 0;
    file->Close(file);
    return 1;
}

EFI_STATUS fs_rename(const CHAR16 *path, const CHAR16 *new_name) {
    if (s_vol_type == FS_VOL_NTFS)
        return EFI_WRITE_PROTECTED;
    if (s_vol_type == FS_VOL_EXFAT && s_exfat) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        char aname[256];
        char16_to_ascii(new_name, aname, 256);
        return exfat_rename(s_exfat, apath, aname) == 0
               ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }

    if (!s_root) return EFI_NOT_READY;

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = s_root->Open(s_root, &file, (CHAR16 *)path,
                                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status)) return status;

    /* Get current file info */
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    file->GetInfo(file, &info_guid, &info_size, NULL);

    /* Allocate extra space for the new name */
    UINTN alloc_size = info_size + 256 * sizeof(CHAR16);
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)mem_alloc(alloc_size);
    if (!info) { file->Close(file); return EFI_OUT_OF_RESOURCES; }

    status = file->GetInfo(file, &info_guid, &info_size, info);
    if (EFI_ERROR(status)) {
        mem_free(info);
        file->Close(file);
        return status;
    }

    /* Set new filename */
    int i = 0;
    while (new_name[i] && i < 255) {
        info->FileName[i] = new_name[i];
        i++;
    }
    info->FileName[i] = 0;
    info->Size = SIZE_OF_EFI_FILE_INFO + (UINT64)(i + 1) * sizeof(CHAR16);

    UINTN set_size = (UINTN)info->Size;
    status = file->SetInfo(file, &info_guid, set_size, info);

    mem_free(info);
    file->Close(file);
    return status;
}

void fs_set_volume(EFI_FILE_HANDLE new_root) {
    unmount_custom();
    s_root = new_root;
}

void fs_restore_boot_volume(void) {
    unmount_custom();
    s_root = s_boot_root;
}

int fs_enumerate_usb(struct fs_usb_volume *vols, int max) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID fsi_guid = EFI_FILE_SYSTEM_INFO_ID;
    EFI_STATUS status;
    UINTN handle_count = 0;
    EFI_HANDLE *handles = NULL;
    int count = 0;

    if (max > FS_MAX_USB) max = FS_MAX_USB;

    status = g_boot.bs->LocateHandleBuffer(
        ByProtocol, &sfs_guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || !handles)
        return 0;

    for (UINTN i = 0; i < handle_count && count < max; i++) {
        /* Skip boot device */
        if (handles[i] == s_boot_device)
            continue;

        /* Check if removable via BlockIO */
        EFI_BLOCK_IO *bio = NULL;
        status = g_boot.bs->HandleProtocol(
            handles[i], &bio_guid, (void **)&bio);
        if (EFI_ERROR(status) || !bio || !bio->Media)
            continue;
        if (!bio->Media->RemovableMedia)
            continue;

        /* Open the volume */
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
        status = g_boot.bs->HandleProtocol(
            handles[i], &sfs_guid, (void **)&sfs);
        if (EFI_ERROR(status) || !sfs)
            continue;

        EFI_FILE_HANDLE vol_root = NULL;
        status = sfs->OpenVolume(sfs, &vol_root);
        if (EFI_ERROR(status) || !vol_root)
            continue;

        struct fs_usb_volume *v = &vols[count];
        v->handle = handles[i];
        v->root = vol_root;

        /* Try to get volume label and size */
        UINTN info_size = 0;
        vol_root->GetInfo(vol_root, &fsi_guid, &info_size, NULL);
        if (info_size > 0) {
            EFI_FILE_SYSTEM_INFO *info = (EFI_FILE_SYSTEM_INFO *)mem_alloc(info_size);
            if (info) {
                status = vol_root->GetInfo(vol_root, &fsi_guid, &info_size, info);
                if (!EFI_ERROR(status)) {
                    /* Format: "LABEL (X GB)" or "USB (X GB)" */
                    int pos = 0;
                    /* Copy volume label if non-empty */
                    int has_label = 0;
                    if (info->VolumeLabel[0] && info->VolumeLabel[0] != L' ') {
                        has_label = 1;
                        for (int j = 0; info->VolumeLabel[j] && pos < 30; j++)
                            v->label[pos++] = (char)(info->VolumeLabel[j] & 0x7F);
                    }
                    if (!has_label) {
                        const char *usb = "USB";
                        for (int j = 0; usb[j]; j++)
                            v->label[pos++] = usb[j];
                    }
                    /* Append size */
                    UINT64 size_mb = info->VolumeSize / (1024 * 1024);
                    if (size_mb > 0) {
                        v->label[pos++] = ' ';
                        v->label[pos++] = '(';
                        if (size_mb >= 1024) {
                            UINT64 size_gb = size_mb / 1024;
                            /* Integer to string */
                            char tmp[12];
                            int t = 0;
                            UINT64 n = size_gb;
                            if (n == 0) tmp[t++] = '0';
                            else while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
                            while (t > 0) v->label[pos++] = tmp[--t];
                            v->label[pos++] = ' ';
                            v->label[pos++] = 'G';
                            v->label[pos++] = 'B';
                        } else {
                            char tmp[12];
                            int t = 0;
                            UINT64 n = size_mb;
                            if (n == 0) tmp[t++] = '0';
                            else while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
                            while (t > 0) v->label[pos++] = tmp[--t];
                            v->label[pos++] = ' ';
                            v->label[pos++] = 'M';
                            v->label[pos++] = 'B';
                        }
                        v->label[pos++] = ')';
                    }
                    v->label[pos] = '\0';
                } else {
                    str_copy(v->label, "USB", 48);
                }
                mem_free(info);
            } else {
                str_copy(v->label, "USB", 48);
            }
        } else {
            str_copy(v->label, "USB", 48);
        }

        count++;
    }

    g_boot.bs->FreePool(handles);
    return count;
}

/* ---- Streaming file I/O ---- */

EFI_FILE_HANDLE fs_open_read(EFI_FILE_HANDLE root, const CHAR16 *path, UINT64 *out_size) {
    if (!root) root = s_root;
    if (!root) return NULL;

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = root->Open(root, &file, (CHAR16 *)path,
                                    EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return NULL;

    /* Get file size */
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    file->GetInfo(file, &info_guid, &info_size, NULL);
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)mem_alloc(info_size);
    if (!info) { file->Close(file); return NULL; }

    status = file->GetInfo(file, &info_guid, &info_size, info);
    if (EFI_ERROR(status)) { mem_free(info); file->Close(file); return NULL; }

    *out_size = info->FileSize;
    mem_free(info);
    return file;
}

EFI_FILE_HANDLE fs_open_write(EFI_FILE_HANDLE root, const CHAR16 *path) {
    if (!root) root = s_root;
    if (!root) return NULL;

    /* Delete existing file first */
    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = root->Open(root, &file, (CHAR16 *)path,
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(status)) {
        file->Delete(file);
        file = NULL;
    }

    /* Create fresh */
    status = root->Open(root, &file, (CHAR16 *)path,
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                         EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status)) return NULL;
    return file;
}

int fs_stream_read(EFI_FILE_HANDLE file, void *buf, UINTN *size) {
    EFI_STATUS status = file->Read(file, size, buf);
    return EFI_ERROR(status) ? -1 : 0;
}

int fs_stream_write(EFI_FILE_HANDLE file, const void *buf, UINTN size) {
    UINTN write_size = size;
    EFI_STATUS status = file->Write(file, &write_size, (void *)buf);
    return EFI_ERROR(status) ? -1 : 0;
}

void fs_stream_close(EFI_FILE_HANDLE file) {
    if (file) {
        file->Flush(file);
        file->Close(file);
    }
}

EFI_STATUS fs_delete_file(EFI_FILE_HANDLE root, const CHAR16 *path) {
    if (!root) root = s_root;
    if (!root) return EFI_NOT_READY;

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = root->Open(root, &file, (CHAR16 *)path,
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status)) return status;
    return file->Delete(file);  /* Delete also closes */
}

EFI_FILE_HANDLE fs_get_boot_root(void) {
    return s_boot_root;
}

EFI_STATUS fs_mkdir(const CHAR16 *path) {
    if (s_vol_type == FS_VOL_NTFS)
        return EFI_WRITE_PROTECTED;
    if (s_vol_type == FS_VOL_EXFAT && s_exfat) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return exfat_mkdir(s_exfat, apath) == 0
               ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }

    if (!s_root) return EFI_NOT_READY;
    EFI_FILE_HANDLE dir = NULL;
    EFI_STATUS status = s_root->Open(s_root, &dir, (CHAR16 *)path,
                                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                                      EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
    if (EFI_ERROR(status)) return status;
    dir->Close(dir);
    return EFI_SUCCESS;
}

EFI_STATUS fs_writefile(const CHAR16 *path, const void *data, UINTN size) {
    if (s_vol_type == FS_VOL_NTFS)
        return EFI_WRITE_PROTECTED;
    if (s_vol_type == FS_VOL_EXFAT && s_exfat) {
        char apath[512];
        path_to_ascii(path, apath, 512);
        return exfat_writefile(s_exfat, apath, data, size) == 0
               ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }

    /* SFS path */
    EFI_STATUS status;
    EFI_FILE_HANDLE file = NULL;

    /* Try to delete existing file first */
    status = s_root->Open(s_root, &file, (CHAR16 *)path,
                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(status)) {
        file->Delete(file);  /* Delete also closes the handle */
        file = NULL;
    }

    /* Create fresh file */
    status = s_root->Open(s_root, &file, (CHAR16 *)path,
                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                          EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status))
        return status;

    /* Write contents */
    UINTN write_size = size;
    status = file->Write(file, &write_size, (void *)data);
    if (EFI_ERROR(status)) {
        file->Close(file);
        return status;
    }

    file->Flush(file);
    file->Close(file);
    return EFI_SUCCESS;
}

/* ---- Custom volume support ---- */

int fs_set_custom_volume(enum fs_vol_type type, EFI_HANDLE handle) {
    /* Unmount any previous custom volume */
    unmount_custom();

    /* Get BlockIO from the handle */
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO *bio = NULL;
    EFI_STATUS st = g_boot.bs->HandleProtocol(
        handle, &bio_guid, (void **)&bio);
    if (EFI_ERROR(st) || !bio || !bio->Media)
        return -1;

    /* Set up BlockIO context for callbacks */
    s_bio_ctx.bio = bio;
    s_bio_ctx.media_id = bio->Media->MediaId;
    UINT32 block_size = bio->Media->BlockSize;

    if (type == FS_VOL_EXFAT) {
        s_exfat = exfat_mount(bio_read_cb, bio_write_cb,
                               &s_bio_ctx, block_size);
        if (!s_exfat) return -1;
        s_vol_type = FS_VOL_EXFAT;
    } else if (type == FS_VOL_NTFS) {
        s_ntfs = ntfs_mount(bio_read_cb, &s_bio_ctx, block_size);
        if (!s_ntfs) return -1;
        s_vol_type = FS_VOL_NTFS;
    } else {
        return -1;
    }

    s_custom_handle = handle;
    return 0;
}

int fs_is_read_only(void) {
    return (s_vol_type == FS_VOL_NTFS) ? 1 : 0;
}

enum fs_vol_type fs_get_vol_type(void) {
    return s_vol_type;
}

/* Check if a handle has a valid FAT32 boot sector */
int fs_has_valid_fat32(EFI_HANDLE handle) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO *bio = NULL;
    EFI_STATUS st = g_boot.bs->HandleProtocol(
        handle, &bio_guid, (VOID **)&bio);
    if (EFI_ERROR(st) || !bio || !bio->Media)
        return 0;

    UINT32 bs = bio->Media->BlockSize;
    if (bs < 512) return 0;

    unsigned char *sec = mem_alloc(bs);
    if (!sec) return 0;

    st = bio->ReadBlocks(bio, bio->Media->MediaId, 0, bs, sec);
    if (EFI_ERROR(st)) {
        mem_free(sec);
        return 0;
    }

    /* Check boot signature 0x55AA at offset 510 */
    int valid = (sec[510] == 0x55 && sec[511] == 0xAA);

    /* Check FAT32 filesystem type string at offset 82 */
    if (valid) {
        valid = (sec[82] == 'F' && sec[83] == 'A' && sec[84] == 'T'
                 && sec[85] == '3' && sec[86] == '2');
    }

    mem_free(sec);
    return valid;
}

/* Format size for label */
static void fs_format_label_size(UINT64 size_bytes, char *label, int *pos) {
    UINT64 size_mb = size_bytes / (1024 * 1024);
    if (size_mb == 0) return;

    label[(*pos)++] = ' ';
    label[(*pos)++] = '(';
    char tmp[12];
    int t = 0;
    if (size_mb >= 1024) {
        UINT64 size_gb = size_mb / 1024;
        UINT64 n = size_gb;
        if (n == 0) tmp[t++] = '0';
        else while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
        while (t > 0) label[(*pos)++] = tmp[--t];
        label[(*pos)++] = ' ';
        label[(*pos)++] = 'G';
        label[(*pos)++] = 'B';
    } else {
        UINT64 n = size_mb;
        if (n == 0) tmp[t++] = '0';
        else while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
        while (t > 0) label[(*pos)++] = tmp[--t];
        label[(*pos)++] = ' ';
        label[(*pos)++] = 'M';
        label[(*pos)++] = 'B';
    }
    label[(*pos)++] = ')';
    label[*pos] = '\0';
}

int fs_enumerate_custom_volumes(struct fs_custom_volume *vols, int max) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_STATUS status;
    UINTN handle_count = 0;
    EFI_HANDLE *handles = NULL;
    int count = 0;

    status = g_boot.bs->LocateHandleBuffer(
        ByProtocol, &bio_guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || !handles)
        return 0;

    for (UINTN i = 0; i < handle_count && count < max; i++) {
        /* Skip boot device */
        if (handles[i] == s_boot_device)
            continue;

        /* Skip handles that have SFS with valid FAT32 */
        void *sfs = NULL;
        status = g_boot.bs->HandleProtocol(
            handles[i], &sfs_guid, &sfs);
        if (!EFI_ERROR(status) && sfs && fs_has_valid_fat32(handles[i]))
            continue;

        /* Get BlockIO */
        EFI_BLOCK_IO *bio = NULL;
        status = g_boot.bs->HandleProtocol(
            handles[i], &bio_guid, (void **)&bio);
        if (EFI_ERROR(status) || !bio || !bio->Media)
            continue;
        if (!bio->Media->MediaPresent)
            continue;

        /* Read sector 0 to check filesystem signature */
        UINT32 bs = bio->Media->BlockSize;
        if (bs < 512) continue;

        unsigned char *sec = mem_alloc(bs);
        if (!sec) continue;

        status = bio->ReadBlocks(bio, bio->Media->MediaId, 0, bs, sec);
        if (EFI_ERROR(status)) {
            mem_free(sec);
            continue;
        }

        enum fs_vol_type type;
        int found = 0;

        /* Check for exFAT: "EXFAT   " at offset 3 */
        if (sec[3] == 'E' && sec[4] == 'X' && sec[5] == 'F' &&
            sec[6] == 'A' && sec[7] == 'T' && sec[8] == ' ' &&
            sec[9] == ' ' && sec[10] == ' ') {
            type = FS_VOL_EXFAT;
            found = 1;
        }
        /* Check for NTFS: "NTFS    " at offset 3 */
        else if (sec[3] == 'N' && sec[4] == 'T' && sec[5] == 'F' &&
                 sec[6] == 'S' && sec[7] == ' ' && sec[8] == ' ' &&
                 sec[9] == ' ' && sec[10] == ' ') {
            type = FS_VOL_NTFS;
            found = 1;
        }

        mem_free(sec);

        if (!found)
            continue;

        struct fs_custom_volume *v = &vols[count];
        v->handle = handles[i];
        v->type = type;
        v->size_bytes = (UINT64)(bio->Media->LastBlock + 1) *
                        (UINT64)bio->Media->BlockSize;

        /* Try to get label by temporarily mounting */
        int pos = 0;
        const char *type_name = (type == FS_VOL_EXFAT) ? "exFAT" : "NTFS";
        while (*type_name && pos < 30)
            v->label[pos++] = *type_name++;

        /* Attempt quick mount to get real label */
        struct bio_ctx tmp_ctx;
        tmp_ctx.bio = bio;
        tmp_ctx.media_id = bio->Media->MediaId;

        if (type == FS_VOL_EXFAT) {
            struct exfat_vol *ev = exfat_mount(bio_read_cb, bio_write_cb,
                                                &tmp_ctx, bs);
            if (ev) {
                const char *lbl = exfat_get_label(ev);
                if (lbl && lbl[0]) {
                    pos = 0;
                    while (*lbl && pos < 30)
                        v->label[pos++] = *lbl++;
                }
                exfat_unmount(ev);
            }
        } else {
            struct ntfs_vol *nv = ntfs_mount(bio_read_cb, &tmp_ctx, bs);
            if (nv) {
                const char *lbl = ntfs_get_label(nv);
                if (lbl && lbl[0]) {
                    pos = 0;
                    while (*lbl && pos < 30)
                        v->label[pos++] = *lbl++;
                }
                ntfs_unmount(nv);
            }
        }

        v->label[pos] = '\0';
        fs_format_label_size(v->size_bytes, v->label, &pos);
        count++;
    }

    g_boot.bs->FreePool(handles);
    return count;
}
