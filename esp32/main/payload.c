/*
 * payload.c — Read compressed workstation images from flash partition
 *
 * Uses esp_partition_mmap() to memory-map the payload partition, then
 * parses the manifest header to find per-architecture file lists.
 */

#include "payload.h"

#include <string.h>
#include "esp_partition.h"
#include "esp_log.h"

static const char *TAG = "payload";

/* Payload header format (must match pack_payload.py) */
#pragma pack(1)
struct payload_header {
    uint8_t  magic[4];     /* "SURV" */
    uint8_t  version;      /* 1 */
    uint8_t  arch_count;
    uint16_t reserved;
};

struct payload_arch_entry {
    char     name[16];
    uint32_t offset;       /* from start of payload to this arch's data */
    uint32_t file_count;
};

struct payload_file_entry {
    char     path[128];
    uint32_t compressed_size;
    uint32_t original_size;
};
#pragma pack()

static const uint8_t *payload_base = NULL;
static size_t payload_size = 0;
static int s_arch_count = 0;
static struct payload_arch s_arches[PAYLOAD_MAX_ARCHES];

int payload_init(void)
{
    /* Find the payload partition */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, NULL);
    if (!part) {
        ESP_LOGE(TAG, "Payload partition not found");
        return -1;
    }

    ESP_LOGI(TAG, "Payload partition: offset=0x%lx, size=0x%lx",
             (unsigned long)part->address, (unsigned long)part->size);
    payload_size = part->size;

    /* Memory-map the entire partition */
    esp_partition_mmap_handle_t mmap_handle;
    esp_err_t ret = esp_partition_mmap(part, 0, part->size,
                                        ESP_PARTITION_MMAP_DATA,
                                        (const void **)&payload_base,
                                        &mmap_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(ret));
        return -1;
    }

    /* Parse header */
    if (payload_size < sizeof(struct payload_header)) return -1;
    const struct payload_header *hdr = (const struct payload_header *)payload_base;

    if (memcmp(hdr->magic, "SURV", 4) != 0) {
        ESP_LOGE(TAG, "Bad magic: %02x%02x%02x%02x",
                 hdr->magic[0], hdr->magic[1], hdr->magic[2], hdr->magic[3]);
        return -1;
    }
    if (hdr->version != 1) {
        ESP_LOGE(TAG, "Unknown version: %d", hdr->version);
        return -1;
    }

    s_arch_count = hdr->arch_count;
    if (s_arch_count > PAYLOAD_MAX_ARCHES)
        s_arch_count = PAYLOAD_MAX_ARCHES;

    ESP_LOGI(TAG, "Payload: version %d, %d architectures", hdr->version, s_arch_count);

    /* Parse architecture table */
    const struct payload_arch_entry *arch_table =
        (const struct payload_arch_entry *)(payload_base + sizeof(struct payload_header));

    for (int a = 0; a < s_arch_count; a++) {
        memcpy(s_arches[a].name, arch_table[a].name, 16);
        s_arches[a].name[15] = '\0';
        s_arches[a].file_count = (int)arch_table[a].file_count;
        if (s_arches[a].file_count > PAYLOAD_MAX_FILES)
            s_arches[a].file_count = PAYLOAD_MAX_FILES;

        /* Parse file manifest for this arch */
        const uint8_t *arch_data = payload_base + arch_table[a].offset;
        const struct payload_file_entry *file_entries =
            (const struct payload_file_entry *)arch_data;

        uint32_t data_offset = (uint32_t)(s_arches[a].file_count * sizeof(struct payload_file_entry));

        for (int f = 0; f < s_arches[a].file_count; f++) {
            memcpy(s_arches[a].files[f].path, file_entries[f].path, 128);
            s_arches[a].files[f].path[127] = '\0';
            s_arches[a].files[f].compressed_size = file_entries[f].compressed_size;
            s_arches[a].files[f].original_size = file_entries[f].original_size;
            s_arches[a].files[f].data_offset = data_offset;

            /* Advance past this file's data */
            uint32_t stored = file_entries[f].compressed_size > 0
                            ? file_entries[f].compressed_size
                            : file_entries[f].original_size;
            data_offset += stored;
        }

        s_arches[a].data_start = arch_table[a].offset +
            (uint32_t)(s_arches[a].file_count * sizeof(struct payload_file_entry));

        ESP_LOGI(TAG, "  %s: %d files", s_arches[a].name, s_arches[a].file_count);
    }

    return 0;
}

int payload_arch_count(void)
{
    return s_arch_count;
}

const struct payload_arch *payload_get_arch(int index)
{
    if (index < 0 || index >= s_arch_count) return NULL;
    return &s_arches[index];
}

const struct payload_arch *payload_get_arch_by_name(const char *name)
{
    for (int i = 0; i < s_arch_count; i++) {
        if (strcmp(s_arches[i].name, name) == 0)
            return &s_arches[i];
    }
    return NULL;
}

const uint8_t *payload_file_data(const struct payload_arch *arch,
                                  const struct payload_file *file)
{
    if (!payload_base || !arch || !file) return NULL;
    /* data_start is the base of file data for this arch (absolute in payload),
     * file->data_offset is relative to that */
    /* Actually, we stored data_offset as offset from arch data block start,
     * and data_start already accounts for the manifest. Let's just use
     * the arch table offset + manifest size + per-file offset. */
    return payload_base + (arch->data_start - (uint32_t)(arch->file_count * sizeof(struct payload_file_entry)))
         + file->data_offset;
}
