/*
 * payload.h — Read compressed workstation images from flash partition
 *
 * The "payload" partition holds a binary blob created by pack_payload.py:
 *   Header: magic "SURV", version, arch count, arch table
 *   Per arch: file manifest (paths + sizes) + deflate-compressed file data
 */
#ifndef PAYLOAD_H
#define PAYLOAD_H

#include <stdint.h>

#define PAYLOAD_MAX_FILES 128

/* Per-file entry in the manifest */
struct payload_file {
    char     path[128];        /* e.g., "EFI/BOOT/BOOTX64.EFI" */
    uint32_t compressed_size;  /* size in payload (0 = stored uncompressed) */
    uint32_t original_size;    /* actual file size */
    uint32_t data_offset;      /* offset from start of arch data block */
};

/* Per-architecture entry */
struct payload_arch {
    char     name[16];         /* "aarch64" or "x86_64" */
    int      file_count;
    struct payload_file files[PAYLOAD_MAX_FILES];
    uint32_t data_start;       /* offset in mmap'd partition to file data */
};

#define PAYLOAD_MAX_ARCHES 2

/* Initialize: mmap the payload partition and parse the manifest.
 * Returns 0 on success, -1 if no valid payload found. */
int payload_init(void);

/* Get the number of architectures in the payload. */
int payload_arch_count(void);

/* Get architecture info by index. Returns NULL if index out of range. */
const struct payload_arch *payload_get_arch(int index);

/* Get architecture info by name. Returns NULL if not found. */
const struct payload_arch *payload_get_arch_by_name(const char *name);

/* Get a pointer to raw file data in the mmap'd partition.
 * The pointer is valid as long as the partition stays mapped. */
const uint8_t *payload_file_data(const struct payload_arch *arch,
                                  const struct payload_file *file);

#endif /* PAYLOAD_H */
