---
layout: default
title: "Chapter 33: Packing the Payload"
parent: "Phase 1: The Pocket Flasher"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 7
---

# Chapter 33: Packing the Payload

## The Compression Problem

The ESP32 has 2.625 MB of payload partition space. The workstation consists of files for two architectures — aarch64 and x86_64 — each containing a UEFI binary, TCC headers, and survival documentation. Uncompressed, the files for both architectures total roughly 2 MB. They fit, but barely — and the margin shrinks as we add more documentation.

Compression helps, but not all compression is equal. We need a format that:

1. **Decompresses on a microcontroller.** The ESP32 has 180 KB of free DRAM. Algorithms like zstd or brotli need megabytes of context. We need something lighter.
2. **Decompresses in a streaming fashion.** We can't buffer a 650 KB decompressed file in memory. We need to decompress in chunks and write each chunk to the SD card as it's produced.
3. **Has a compact decompressor.** Code space in the firmware is limited. A decompressor with a 50 KB code footprint is too expensive.

The answer is **raw deflate** — the same compression algorithm used by zlib and gzip, but without the wrapper headers. The miniz library, bundled with ESP-IDF's esp_rom component, provides `tinfl_decompress` — a streaming deflate decompressor that needs only a 32 KB dictionary buffer.

## The Payload Format

The `pack_payload.py` script creates a binary blob with a structured format:

```
┌────────────────────────────────────────┐
│ Header (8 bytes)                       │
│   magic: "SURV" (4)                    │
│   version: 1 (1)                       │
│   arch_count: N (1)                    │
│   reserved: 0 (2)                      │
├────────────────────────────────────────┤
│ Arch table (24 bytes × N)              │
│   name: "aarch64" (16, null-padded)    │
│   offset: uint32 (from payload start)  │
│   file_count: uint32                   │
├────────────────────────────────────────┤
│ Arch 0 data:                           │
│   File manifest (136 bytes × count)    │
│     path: 128 bytes (null-padded)      │
│     compressed_size: uint32 (0=raw)    │
│     original_size: uint32              │
│   File data (concatenated)             │
│     [compressed or raw bytes]          │
├────────────────────────────────────────┤
│ Arch 1 data:                           │
│   [same structure]                     │
└────────────────────────────────────────┘
```

Each architecture section is self-contained: a manifest listing every file's path, compressed size, and original size, followed by the concatenated file data. The offset in the arch table points from the start of the payload to the beginning of that architecture's manifest.

A `compressed_size` of 0 means the file is stored uncompressed. This happens for files smaller than 4 KB (not worth the overhead) or files where deflate doesn't save space (already-compressed data, random-looking binaries).

## The Packing Script

`pack_payload.py` runs on the build machine (the Linux host), not on the ESP32. It reads from the build output directory and produces a single `payload.bin`:

```python
#!/usr/bin/env python3
import argparse
import os
import struct
import zlib
from pathlib import Path

COMPRESS_THRESHOLD = 4096
```

The threshold is a pragmatic choice. Files under 4 KB gain very little from compression — the deflate header overhead can actually make them larger. And tiny files are already tiny. Skipping them reduces complexity in the on-device decompressor.

### Collecting Files

```python
def collect_files(esp_dir):
    """Walk an ESP directory tree and return list of (relative_path, data)."""
    files = []
    esp_path = Path(esp_dir)
    if not esp_path.exists():
        return files

    for filepath in sorted(esp_path.rglob("*")):
        if filepath.is_file():
            rel = filepath.relative_to(esp_path)
            rel_str = str(rel).replace("\\", "/")
            data = filepath.read_bytes()
            files.append((rel_str, data))

    return files
```

`rglob("*")` recursively finds all files under the ESP directory. The sort ensures deterministic ordering — the payload is bit-for-bit reproducible across builds. Paths use forward slashes regardless of the host OS.

The ESP directory structure mirrors what goes on the SD card:

```
build/aarch64/esp/
  EFI/BOOT/BOOTAA64.EFI      The UEFI binary
  tcc-headers/                TCC compiler headers
  docs/                       Survival documentation
```

### Compression

```python
def compress_file(data):
    """Compress with raw deflate. Returns (compressed_data, compressed_size).
    Returns (data, 0) if compression isn't worthwhile."""
    if len(data) < COMPRESS_THRESHOLD:
        return data, 0

    # Use raw deflate (wbits=-15, no zlib/gzip header)
    compressed = zlib.compress(data, 9)[2:-4]  # strip zlib header/trailer

    if len(compressed) >= len(data):
        return data, 0

    return compressed, len(compressed)
```

`zlib.compress(data, 9)` produces a zlib-wrapped deflate stream. The `[2:-4]` slice strips the 2-byte zlib header and 4-byte Adler-32 checksum, leaving raw deflate. This is the format `tinfl_decompress` expects on the ESP32 — no wrapper, just compressed data.

Compression level 9 (maximum) is used because pack time on the build machine doesn't matter — we compress once, decompress many times on the microcontroller. The extra compression saves payload space, which is our scarcest resource.

If compression doesn't save space (the compressed output is as large as or larger than the input), we store the file uncompressed. This happens with files that are already compressed or contain high-entropy data.

### Building the Payload

```python
def pack_arch(name, esp_dir):
    """Pack one architecture. Returns (manifest_bytes, data_bytes, file_count)."""
    files = collect_files(esp_dir)
    manifest = b""
    data = b""

    for rel_path, file_data in files:
        compressed, comp_size = compress_file(file_data)
        orig_size = len(file_data)

        path_bytes = rel_path.encode("utf-8")[:127] + b"\x00"
        path_bytes = path_bytes.ljust(128, b"\x00")
        manifest += struct.pack("<128s I I", path_bytes, comp_size, orig_size)

        data += compressed

        status = f"deflate {comp_size}B" if comp_size > 0 else "stored"
        print(f"  {rel_path}: {orig_size}B -> {status}")

    return manifest, data, len(files)
```

Each file produces a 136-byte manifest entry (128-byte path + 4-byte compressed size + 4-byte original size) and a variable-length data block. The manifest and data are kept separate until final assembly, so the offsets are correct.

```python
def main():
    # ... argument parsing ...

    arches = []
    for arch_name in ["aarch64", "x86_64"]:
        esp_dir = build_dir / arch_name / "esp"
        if esp_dir.exists():
            arches.append((arch_name, str(esp_dir)))

    header = struct.pack("<4s B B H", b"SURV", 1, len(arches), 0)

    arch_table_size = 24 * len(arches)
    data_offset = len(header) + arch_table_size

    arch_table = b""
    arch_blobs = []

    for arch_name, esp_dir in arches:
        manifest, data, file_count = pack_arch(arch_name, esp_dir)
        blob = manifest + data

        name_bytes = arch_name.encode("utf-8")[:15] + b"\x00"
        name_bytes = name_bytes.ljust(16, b"\x00")
        arch_table += struct.pack("<16s I I", name_bytes, data_offset, file_count)

        arch_blobs.append(blob)
        data_offset += len(blob)

    payload = header + arch_table
    for blob in arch_blobs:
        payload += blob

    output_path = Path(args.output)
    output_path.write_bytes(payload)

    max_size = 0x290000  # 2.625 MB partition
    if len(payload) > max_size:
        print(f"WARNING: payload exceeds partition size")
        return 1
```

The offset calculation is critical. `data_offset` starts after the header + arch table and advances by the size of each architecture's blob. The arch table entry for "aarch64" might say `offset=56`, meaning the aarch64 manifest starts at byte 56 of the payload. The "x86_64" entry's offset accounts for the entire aarch64 blob.

The size check against 0x290000 (2,686,976 bytes) catches payloads that won't fit in the partition before you waste time flashing them.

## Flashing the Payload

The payload binary gets flashed to the ESP32's payload partition at offset 0x170000:

```bash
esptool.py --port /dev/ttyUSB0 write_flash 0x170000 payload.bin
```

This is separate from flashing the firmware (`idf.py flash`). The firmware goes to 0x10000, the payload goes to 0x170000. You can update one without touching the other — rebuild the workstation, repack the payload, flash just the payload partition.

The address 0x170000 matches the partition table from [Chapter 28](chapter-28-setting-up-the-esp32): the payload partition starts at offset 0x170000 in the ESP32's 4 MB flash.

## Reading the Payload on the ESP32

The payload module (`payload.c`) reads the manifest at startup using `esp_partition_mmap`, which memory-maps the flash partition into the ESP32's address space:

```c
int payload_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, NULL);
    if (!part) {
        ESP_LOGE(TAG, "Payload partition not found");
        return -1;
    }

    esp_partition_mmap_handle_t mmap_handle;
    esp_err_t ret = esp_partition_mmap(part, 0, part->size,
                                        ESP_PARTITION_MMAP_DATA,
                                        (const void **)&payload_base,
                                        &mmap_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(ret));
        return -1;
    }
```

`esp_partition_find_first` locates the partition by type (data) and subtype (0x40 — our custom "payload" subtype). `esp_partition_mmap` maps the entire partition into memory, making every byte accessible as a pointer dereference. No read calls, no buffers — the compressed data is accessed directly from flash through the SPI cache.

This is one of the ESP32's best features for our use case. The payload is 2.6 MB — far too large to copy into RAM. But memory mapping lets us treat it like RAM-resident data. The hardware SPI cache fetches pages on demand, transparently. When the decompressor reads compressed bytes, the cache loads the corresponding flash page. The programmer sees a flat pointer; the hardware handles the I/O.

### Parsing the Manifest

```c
    const struct payload_header *hdr = (const struct payload_header *)payload_base;

    if (memcmp(hdr->magic, "SURV", 4) != 0) {
        ESP_LOGE(TAG, "Bad magic");
        return -1;
    }
    if (hdr->version != 1) {
        ESP_LOGE(TAG, "Unknown version: %d", hdr->version);
        return -1;
    }

    s_arch_count = hdr->arch_count;
```

The magic check prevents the flasher from treating random flash contents as a valid payload. A freshly flashed ESP32 with no payload will have the flash partition filled with 0xFF bytes — the "erased" state of NOR flash. The magic check catches this immediately.

```c
    const struct payload_arch_entry *arch_table =
        (const struct payload_arch_entry *)(payload_base + sizeof(struct payload_header));

    for (int a = 0; a < s_arch_count; a++) {
        memcpy(s_arches[a].name, arch_table[a].name, 16);
        s_arches[a].file_count = (int)arch_table[a].file_count;

        const uint8_t *arch_data = payload_base + arch_table[a].offset;
        const struct payload_file_entry *file_entries =
            (const struct payload_file_entry *)arch_data;

        uint32_t data_offset = (uint32_t)(s_arches[a].file_count
                                          * sizeof(struct payload_file_entry));

        for (int f = 0; f < s_arches[a].file_count; f++) {
            memcpy(s_arches[a].files[f].path, file_entries[f].path, 128);
            s_arches[a].files[f].compressed_size = file_entries[f].compressed_size;
            s_arches[a].files[f].original_size = file_entries[f].original_size;
            s_arches[a].files[f].data_offset = data_offset;

            uint32_t stored = file_entries[f].compressed_size > 0
                            ? file_entries[f].compressed_size
                            : file_entries[f].original_size;
            data_offset += stored;
        }

        s_arches[a].data_start = arch_table[a].offset
            + (uint32_t)(s_arches[a].file_count * sizeof(struct payload_file_entry));
    }
```

For each architecture, the manifest is parsed into the `s_arches` array. Each file gets its path, sizes, and data offset. The `data_offset` advances past each file's stored data (compressed size if compressed, original size if not).

### Providing Data Pointers

```c
const uint8_t *payload_file_data(const struct payload_arch *arch,
                                  const struct payload_file *file)
{
    if (!payload_base || !arch || !file) return NULL;
    return payload_base + (arch->data_start
           - (uint32_t)(arch->file_count * sizeof(struct payload_file_entry)))
         + file->data_offset;
}
```

This function returns a pointer directly into the memory-mapped flash. The flasher passes this pointer to the decompressor (for compressed files) or to `fat32_write_file` (for uncompressed files). No copying, no buffering — the data flows from flash through the decompressor to the SD card.

## The Complete Payload Interface

```c
#ifndef PAYLOAD_H
#define PAYLOAD_H

#include <stdint.h>

#define PAYLOAD_MAX_FILES 128

struct payload_file {
    char     path[128];
    uint32_t compressed_size;  /* 0 = stored uncompressed */
    uint32_t original_size;
    uint32_t data_offset;
};

struct payload_arch {
    char     name[16];
    int      file_count;
    struct payload_file files[PAYLOAD_MAX_FILES];
    uint32_t data_start;
};

#define PAYLOAD_MAX_ARCHES 2

int payload_init(void);
int payload_arch_count(void);
const struct payload_arch *payload_get_arch(int index);
const struct payload_arch *payload_get_arch_by_name(const char *name);
const uint8_t *payload_file_data(const struct payload_arch *arch,
                                  const struct payload_file *file);

#endif /* PAYLOAD_H */
```

The interface is read-only. The payload is packed once on the build machine and never modified on the ESP32. All functions return const pointers — the flash data is immutable.

## What We Have

```
payload.c          161 lines   Manifest reader: mmap, parse, provide pointers
payload.h           52 lines   Interface: init, get arch, get file data
scripts/pack_payload.py  179 lines   Payload packer: collect, compress, pack
```

The packing pipeline is split across two worlds: Python on the build machine (collect files, compress, produce the binary blob) and C on the ESP32 (memory-map the blob, parse the manifest, provide data pointers to the flasher). The binary format bridges them — structured enough to parse without a JSON library, compact enough to fit in 2.6 MB.

The payload is the last piece before we can flash. We have a GPT partition table (Chapter 31), a FAT32 filesystem (Chapter 32), and a compressed workstation image (this chapter). All that remains is wiring them together.

---

**Next:** [Chapter 34: Flash!](chapter-34-flash)
