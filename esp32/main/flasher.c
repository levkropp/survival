/*
 * flasher.c — Full flash sequence orchestrator
 *
 * Steps:
 *   1. Create GPT partition table on SD card
 *   2. Format EFI System Partition as FAT32
 *   3. For each file in the payload:
 *      a. If compressed: streaming decompress (4KB chunks) + write
 *      b. If uncompressed: direct write
 *      c. Update progress on display
 *   4. Verify by reading back the FAT32 boot sector
 */

#include "flasher.h"
#include "gpt.h"
#include "fat32.h"
#include "sdcard.h"
#include "payload.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "flasher";

/* Decompress a deflate stream and write via streaming FAT32 API.
 * Uses tinfl_decompress() with a 32KB dictionary buffer (TINFL_LZ_DICT_SIZE)
 * so LZ77 back-references resolve correctly. */
static tinfl_decompressor s_decomp;
static uint8_t s_dict[TINFL_LZ_DICT_SIZE];

static int decompress_and_write(int stream_handle,
                                 const uint8_t *compressed, uint32_t comp_size,
                                 uint32_t orig_size)
{
    tinfl_init(&s_decomp);
    const uint8_t *in_ptr = compressed;
    size_t in_remaining = comp_size;
    uint32_t total_out = 0;
    size_t dict_ofs = 0;

    for (;;) {
        size_t in_bytes = in_remaining;
        size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
        uint32_t flags = (in_remaining > 0) ? TINFL_FLAG_HAS_MORE_INPUT : 0;

        tinfl_status status = tinfl_decompress(&s_decomp,
            in_ptr, &in_bytes,
            s_dict, s_dict + dict_ofs, &out_bytes,
            flags);

        if (out_bytes > 0) {
            if (fat32_stream_write(stream_handle, s_dict + dict_ofs, (uint32_t)out_bytes) != 0)
                return -1;
            total_out += (uint32_t)out_bytes;
        }

        in_ptr += in_bytes;
        in_remaining -= in_bytes;
        dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);

        if (status == TINFL_STATUS_DONE) break;
        if (status < 0) {
            ESP_LOGE(TAG, "Decompression error: %d", status);
            return -1;
        }
    }

    if (total_out != orig_size) {
        ESP_LOGW(TAG, "Size mismatch: got %lu, expected %lu",
                 (unsigned long)total_out, (unsigned long)orig_size);
    }
    return 0;
}

static int s_last_format_pct = -1;

static void format_progress(int current, int total)
{
    if (total <= 0) return;
    int pct = current * 100 / total;
    if (pct == s_last_format_pct) return;
    s_last_format_pct = pct;
    ui_update_progress("Formatting FAT32...", pct, 100);
}

int flasher_run(const char *arch)
{
    ESP_LOGI(TAG, "Starting flash sequence for %s", arch);

    const struct payload_arch *pa = payload_get_arch_by_name(arch);
    if (!pa) {
        ESP_LOGE(TAG, "Architecture '%s' not found in payload", arch);
        return -1;
    }

    uint64_t card_size = sdcard_size();
    ESP_LOGI(TAG, "SD card: %llu MB", (unsigned long long)(card_size / (1024 * 1024)));

    /* Step 1: Create GPT */
    s_last_format_pct = -1;
    ui_update_progress("Creating partition table...", 0, pa->file_count + 2);
    if (gpt_create(card_size) != 0) {
        ESP_LOGE(TAG, "GPT creation failed");
        return -1;
    }

    /* Step 2: Format FAT32 */
    ui_update_progress("Formatting FAT32...", 0, 100);
    uint32_t esp_start = gpt_esp_start_lba();
    uint32_t esp_sectors = gpt_esp_size_sectors();
    if (fat32_format(esp_start, esp_sectors, format_progress) != 0) {
        ESP_LOGE(TAG, "FAT32 format failed");
        return -1;
    }

    /* Step 3: Write each file */
    for (int i = 0; i < pa->file_count; i++) {
        const struct payload_file *pf = &pa->files[i];
        ESP_LOGI(TAG, "Writing: %s (%lu bytes)", pf->path, (unsigned long)pf->original_size);

        char msg[64];
        snprintf(msg, sizeof(msg), "Writing: %.40s", pf->path);
        ui_update_progress(msg, i + 2, pa->file_count + 2);

        const uint8_t *data = payload_file_data(pa, pf);
        if (!data) {
            ESP_LOGE(TAG, "Failed to get data for %s", pf->path);
            return -1;
        }

        if (pf->compressed_size > 0) {
            /* Compressed — use streaming decompress + write */
            int handle = fat32_stream_open(pf->path, pf->original_size);
            if (handle < 0) {
                ESP_LOGE(TAG, "Stream open failed for %s", pf->path);
                return -1;
            }
            if (decompress_and_write(handle, data, pf->compressed_size, pf->original_size) != 0) {
                ESP_LOGE(TAG, "Decompress+write failed for %s", pf->path);
                return -1;
            }
            if (fat32_stream_close(handle) != 0) {
                ESP_LOGE(TAG, "Stream close failed for %s", pf->path);
                return -1;
            }
        } else {
            /* Uncompressed — direct write */
            if (fat32_write_file(pf->path, data, pf->original_size) != 0) {
                ESP_LOGE(TAG, "Write failed for %s", pf->path);
                return -1;
            }
        }
    }

    ui_update_progress("Verifying...", pa->file_count + 2, pa->file_count + 2);

    /* Quick verify: read back the BPB and check signature */
    static uint8_t __attribute__((aligned(4))) verify_buf[512];
    if (sdcard_read(esp_start, 1, verify_buf) == 0) {
        if (verify_buf[510] == 0x55 && verify_buf[511] == 0xAA) {
            ESP_LOGI(TAG, "Verification passed");
        } else {
            ESP_LOGW(TAG, "BPB signature mismatch");
        }
    }

    ESP_LOGI(TAG, "Flash complete: %d files written", pa->file_count);
    return 0;
}
