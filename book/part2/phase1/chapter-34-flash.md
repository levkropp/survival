---
layout: default
title: "Chapter 34: Flash!"
parent: "Phase 1: The Pocket Flasher"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 8
---

# Chapter 34: Flash!

## The Orchestrator

Every layer is in place. The display shows a UI. The touchscreen reads taps. The SD card driver writes sectors. The GPT module creates partition tables. The FAT32 module builds filesystems. The payload module reads compressed files from flash. Now we wire them together.

`flasher.c` is the orchestrator — it runs the complete flash sequence in order:

1. Create a GPT partition table
2. Format the EFI System Partition as FAT32
3. Write each file from the payload (decompressing if needed)
4. Verify by reading back the boot sector

The whole file is 173 lines. Let's walk through it.

## Setup

```c
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
```

`miniz.h` provides the deflate decompressor. On ESP-IDF, miniz is part of the `esp_rom` component — it ships in the ESP32's ROM and is available without adding any external libraries. The include path is just `"miniz.h"`, not `"rom/miniz.h"` — a quirk of ESP-IDF's component system that will waste your time if you guess wrong.

## Streaming Decompression

This is the most technically interesting part of the flasher. Compressed files in the payload must be decompressed and written to the SD card without buffering the entire decompressed output in memory. The UEFI binary is 650 KB decompressed — 3.5x our available RAM.

```c
static tinfl_decompressor s_decomp;
static uint8_t s_dict[TINFL_LZ_DICT_SIZE];
```

Two pieces of state: the decompressor context and the dictionary buffer. `TINFL_LZ_DICT_SIZE` is 32,768 bytes (32 KB). The dictionary is where the decompressor stores recently output bytes so it can resolve back-references in the compressed stream.

Why 32 KB? Deflate's LZ77 algorithm can reference bytes up to 32 KB behind the current position. When the compressor finds a repeated sequence, it encodes it as "copy N bytes from M positions back." The decompressor needs those M positions available in the dictionary to resolve the reference. A smaller dictionary would fail to resolve long-distance references and produce corrupted output.

```c
static int decompress_and_write(int stream_handle,
                                 const uint8_t *compressed, uint32_t comp_size,
                                 uint32_t orig_size)
{
    tinfl_init(&s_decomp);
    const uint8_t *in_ptr = compressed;
    size_t in_remaining = comp_size;
    uint32_t total_out = 0;
    size_t dict_ofs = 0;
```

`dict_ofs` tracks our position in the circular dictionary buffer. As decompressed bytes are produced, they fill the dictionary from position 0 upward. When the buffer fills (at 32 KB), it wraps around to 0. This circular usage is why `TINFL_LZ_DICT_SIZE` must be a power of two — the wrap is handled with a bitwise AND.

```c
    for (;;) {
        size_t in_bytes = in_remaining;
        size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
        uint32_t flags = (in_remaining > 0) ? TINFL_FLAG_HAS_MORE_INPUT : 0;

        tinfl_status status = tinfl_decompress(&s_decomp,
            in_ptr, &in_bytes,
            s_dict, s_dict + dict_ofs, &out_bytes,
            flags);
```

`tinfl_decompress` is the core call. It takes:
- Input: a pointer to compressed data and how many bytes are available
- Output: a pointer into the dictionary buffer and how many bytes of space are available
- Flags: whether more input is coming

It returns how many input bytes it consumed (by updating `in_bytes`) and how many output bytes it produced (by updating `out_bytes`). The status indicates whether decompression is done, needs more input, needs more output space, or encountered an error.

```c
        if (out_bytes > 0) {
            if (fat32_stream_write(stream_handle, s_dict + dict_ofs,
                                   (uint32_t)out_bytes) != 0)
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
```

Each iteration may produce anywhere from 0 to 32,768 bytes of output. The decompressed bytes are immediately passed to `fat32_stream_write`, which buffers them into sector-sized chunks and writes them to the SD card. No intermediate buffering — the data flows directly from the decompressor to the filesystem.

The dictionary offset wraps with `& (TINFL_LZ_DICT_SIZE - 1)` — the power-of-two bitwise AND that's equivalent to modulo but faster. When `dict_ofs` reaches 32768, the AND resets it to 0, and the next output overwrites the oldest bytes in the dictionary. This is safe because the decompressor only references bytes within the 32 KB window.

After the loop, a size check:

```c
    if (total_out != orig_size) {
        ESP_LOGW(TAG, "Size mismatch: got %lu, expected %lu",
                 (unsigned long)total_out, (unsigned long)orig_size);
    }
    return 0;
}
```

A mismatch is a warning, not an error — the file may still be usable, but something unexpected happened. In practice, this never triggers with a correctly packed payload.

## Memory Budget

Let's add up the memory used during a flash operation:

```
Component                      Size
──────────────────────────────────────
s_decomp (decompressor state)    ~1 KB
s_dict (dictionary)             32 KB
s_zero (FAT zeroing buffer)     64 KB
s_buf (FAT/format sector)      512 B
s_dir (directory sector)        512 B
s_stream_buf (stream sector)    512 B
verify_buf                      512 B
──────────────────────────────────────
Total                          ~99 KB
```

About 99 KB of static buffers, out of roughly 180 KB available. The remaining 81 KB is used by FreeRTOS task stacks, ESP-IDF internal buffers, and the SPI DMA descriptors. The budget is tight but workable — every buffer size was chosen with this arithmetic in mind.

The largest single consumer is `s_zero` at 64 KB. It could be smaller (32 KB, 16 KB) at the cost of slower FAT zeroing — each batch write would transfer fewer sectors, requiring more SD card commands. 64 KB is the sweet spot: large enough for efficient batching, small enough to leave room for the dictionary and decompressor.

## The Flash Sequence

```c
int flasher_run(const char *arch)
{
    ESP_LOGI(TAG, "Starting flash sequence for %s", arch);

    const struct payload_arch *pa = payload_get_arch_by_name(arch);
    if (!pa) {
        ESP_LOGE(TAG, "Architecture '%s' not found in payload", arch);
        return -1;
    }

    uint64_t card_size = sdcard_size();
    ESP_LOGI(TAG, "SD card: %llu MB",
             (unsigned long long)(card_size / (1024 * 1024)));
```

First, find the architecture in the payload manifest. If the user tapped "Flash aarch64" but the payload only contains x86_64 files (because that's all the build produced), this fails cleanly.

### Step 1: GPT

```c
    ui_update_progress("Creating partition table...", 0, pa->file_count + 2);
    if (gpt_create(card_size) != 0) {
        ESP_LOGE(TAG, "GPT creation failed");
        return -1;
    }
```

The total progress count is `file_count + 2`: one step for GPT creation, one for FAT32 formatting, and one per file. The progress bar's total reflects the complete sequence.

`gpt_create` takes the card's byte size and computes the partition layout dynamically. A 2 GB card gets a ~2 GB EFI System Partition. A 32 GB card gets a ~32 GB one. The partition fills whatever card is inserted.

### Step 2: FAT32

```c
    ui_update_progress("Formatting FAT32...", 0, 100);
    uint32_t esp_start = gpt_esp_start_lba();
    uint32_t esp_sectors = gpt_esp_size_sectors();
    if (fat32_format(esp_start, esp_sectors, format_progress) != 0) {
        ESP_LOGE(TAG, "FAT32 format failed");
        return -1;
    }
```

The format step gets its own progress tracking because FAT zeroing is the longest single operation:

```c
static int s_last_format_pct = -1;

static void format_progress(int current, int total)
{
    if (total <= 0) return;
    int pct = current * 100 / total;
    if (pct == s_last_format_pct) return;
    s_last_format_pct = pct;
    ui_update_progress("Formatting FAT32...", pct, 100);
}
```

The percentage-change check (`pct == s_last_format_pct`) prevents updating the display on every batch write. Without it, the display would update thousands of times during formatting — each update takes ~5 ms (SPI transfer), so thousands of updates would add seconds of delay. By only updating when the visible percentage changes (at most 101 times), the overhead is negligible.

### Step 3: Write Files

```c
    for (int i = 0; i < pa->file_count; i++) {
        const struct payload_file *pf = &pa->files[i];
        ESP_LOGI(TAG, "Writing: %s (%lu bytes)",
                 pf->path, (unsigned long)pf->original_size);

        char msg[64];
        snprintf(msg, sizeof(msg), "Writing: %.40s", pf->path);
        ui_update_progress(msg, i + 2, pa->file_count + 2);

        const uint8_t *data = payload_file_data(pa, pf);
        if (!data) {
            ESP_LOGE(TAG, "Failed to get data for %s", pf->path);
            return -1;
        }
```

Each file's path is shown on the display (truncated to 40 characters to fit the screen). The progress bar advances for each file.

`payload_file_data` returns a pointer into the memory-mapped flash partition. For compressed files, this pointer is to the compressed bytes. For uncompressed files, it's the raw file content.

```c
        if (pf->compressed_size > 0) {
            /* Compressed — use streaming decompress + write */
            int handle = fat32_stream_open(pf->path, pf->original_size);
            if (handle < 0) {
                ESP_LOGE(TAG, "Stream open failed for %s", pf->path);
                return -1;
            }
            if (decompress_and_write(handle, data, pf->compressed_size,
                                      pf->original_size) != 0) {
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
```

Two code paths: compressed files use the streaming API (`stream_open` → `decompress_and_write` → `stream_close`), uncompressed files use the simpler `fat32_write_file` that takes a data pointer and size.

The streaming path handles the large UEFI binary and other big files. `stream_open` creates the directory path (e.g., `EFI/BOOT/`) and pre-allocates the cluster chain for the declared file size. `decompress_and_write` feeds decompressed chunks to `stream_write`. `stream_close` flushes the last partial sector and adds the directory entry.

The direct path handles small files — TCC headers, configuration files, small documents. These are small enough to fit in memory (the pointer from `payload_file_data` points to the already-available data in flash), so no streaming is needed.

### Step 4: Verification

```c
    ui_update_progress("Verifying...", pa->file_count + 2, pa->file_count + 2);

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
```

A minimal verification: read back the first sector of the EFI System Partition and check for the `0x55AA` boot signature. If this two-byte check passes, the BPB was written correctly. If it fails, something went wrong with the SD card write — the card may be defective or the SPI connection may be unreliable.

This isn't a full verification (we don't re-read every file and compare checksums), but it catches the most common failure mode: a card that wasn't written at all, or one where the boot sector was corrupted. A full verification would double the flash time by re-reading every sector, which isn't worth it for a process that already proved it can write and read the card.

## Testing in QEMU

Before trusting a flashed card with real hardware, you can test it in QEMU. The ESP32 creates a standard GPT + FAT32 disk that any UEFI firmware can boot:

```bash
# Create OVMF firmware files (split CODE and VARS)
cp /usr/share/edk2-ovmf/x64/OVMF_CODE.fd .
cp /usr/share/edk2-ovmf/x64/OVMF_VARS.fd .

# Boot the SD card image with QEMU
qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd \
    -drive if=pflash,format=raw,file=OVMF_VARS.fd \
    -drive file=/dev/mmcblk0,format=raw \
    -m 256M
```

Replace `/dev/mmcblk0` with the actual SD card device. QEMU boots OVMF (the open-source UEFI firmware), which reads the GPT, finds the EFI System Partition, locates `EFI/BOOT/BOOTX64.EFI`, and launches the workstation.

If the workstation boots, opens the editor, and F6 (rebuild) succeeds — the SD card is verified end-to-end: the GPT is valid, the FAT32 filesystem is correct, the binary was decompressed correctly, the compiler headers are present, and the self-hosting rebuild works.

## The Complete Flow

Here is the full sequence from power-on to a working survival workstation:

```
ESP32 powers on
    │
    ├── display_init()     → ILI9341 on, backlight on
    ├── touch_init()       → XPT2046 ready
    ├── payload_init()     → mmap payload partition, parse manifest
    ├── chip_detect()      → "ESP32, 2 cores, external flash"
    │
    ├── ui_show_splash()   → "SURVIVAL WORKSTATION / SD Card Flasher"
    │   (1.5 second delay)
    │
    └── Main loop:
        │
        ├── ui_show_menu() → "Flash aarch64" / "Flash x86_64"
        │   (blocks until button tap)
        │
        ├── sdcard_init()  → SPI3 bus, probe card, read capacity
        │
        ├── flasher_run(arch):
        │   ├── gpt_create()          → Protective MBR + GPT + backup
        │   ├── fat32_format()        → BPB, FSInfo, FAT tables, root dir
        │   ├── For each file:
        │   │   ├── fat32_stream_open()
        │   │   ├── decompress_and_write()  or  fat32_write_file()
        │   │   └── fat32_stream_close()
        │   └── Verify boot sector
        │
        ├── sdcard_deinit()
        │
        ├── ui_show_done() → "Flash Complete! Remove SD card and boot."
        │
        └── (tap to return to menu)

User removes SD card, inserts into UEFI computer, boots
    │
    ├── UEFI reads GPT     → finds EFI System Partition
    ├── UEFI mounts FAT32  → finds EFI/BOOT/BOOTAA64.EFI (or BOOTX64.EFI)
    ├── UEFI loads binary   → survival workstation starts
    │
    └── User opens editor, presses F6 → workstation rebuilds itself from source
```

## What We Built

The complete flasher:

```
Source File         Lines   Purpose
──────────────────  ─────   ─────────────────────────────
main.c              111     Entry point, main loop
display.c           157     ILI9341 display driver (SPI2)
display.h            45     Display interface + colors
touch.c             168     XPT2046 touch driver (bit-bang)
touch.h              21     Touch interface
sdcard.c            131     SD card driver (SPI3)
sdcard.h             33     SD card interface
gpt.c               217     GPT partition table creation
gpt.h                22     GPT interface
fat32.c             893     FAT32 filesystem (largest)
fat32.h              49     FAT32 interface
flasher.c           173     Flash sequence orchestrator
flasher.h            14     Flasher interface
payload.c           161     Payload manifest reader
payload.h            52     Payload interface
ui.c                166     Touch UI (splash/menu/progress)
ui.h                 32     UI interface
font.c              202     8x16 VGA bitmap font data
font.h               20     Font constants
──────────────────  ─────
Total C:          ~2,667 lines

pack_payload.py     179     Build-time payload packer
CMakeLists.txt        8     Top-level build
main/CMakeLists.txt  15     Component registration
partitions.csv        6     Flash partition layout
sdkconfig.defaults   19     ESP-IDF configuration
```

Under 2,700 lines of C. A Python script to pack the payload. A handful of configuration files. No external libraries beyond what ESP-IDF provides.

The flasher does one thing: write a bootable survival workstation to a blank SD card. It does it with a progress bar, error handling, multi-architecture support, and a verified boot sector. The entire thing runs on a $7 board the size of a credit card, powered by a USB cable or battery.

## The Milestone

Part 1 gave us the workstation: a bare-metal UEFI application that boots, edits code, compiles itself, and stores survival knowledge.

Part 2, Phase 1 gave us distribution: a pocket-sized device that writes the workstation to any SD card without needing a computer.

The survival kit is complete. A solar panel, a keyboard, a monitor, any UEFI-capable computer, and an ESP32 CYD with a blank SD card. That's everything you need to bootstrap a computing environment from nothing.

The ESP32 stores the workstation. The SD card carries it. The computer runs it. And the workstation can rebuild itself, modify itself, and grow — because it includes its own compiler and its own source code.

From here, the road opens up. Phase 2 could add ESP32-S3 USB boot (presenting the workstation as a bootable USB mass storage device). Phase 3 could turn the CYD's screen into a survival reference viewer. But those are future chapters. Right now, the flasher works. The card boots. The rebuild succeeds.

That's the milestone.
