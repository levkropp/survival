#!/usr/bin/env python3
"""
pack_payload.py — Pack workstation images into a flashable payload binary.

Reads from build/{aarch64,x86_64}/esp/ and creates a single payload.bin
that gets flashed to the ESP32's payload partition.

Usage:
    python3 pack_payload.py [--build-dir ../build] [--output payload.bin]

Then flash with:
    esptool.py write_flash 0x170000 payload.bin

Payload format:
    Header (8 bytes):
        magic: "SURV" (4 bytes)
        version: 1 (1 byte)
        arch_count: N (1 byte)
        reserved: 0 (2 bytes)

    Arch table (24 bytes × N):
        name: 16 bytes (null-padded)
        offset: 4 bytes (from payload start to arch data)
        file_count: 4 bytes

    Per architecture:
        File manifest (136 bytes × file_count):
            path: 128 bytes (null-padded)
            compressed_size: 4 bytes (0 = stored uncompressed)
            original_size: 4 bytes

        File data:
            [compressed or raw bytes for each file, concatenated]
"""

import argparse
import os
import struct
import zlib
from pathlib import Path


# Files smaller than this are stored uncompressed (not worth the overhead)
COMPRESS_THRESHOLD = 4096


def collect_files(esp_dir):
    """Walk an ESP directory tree and return list of (relative_path, data)."""
    files = []
    esp_path = Path(esp_dir)
    if not esp_path.exists():
        return files

    for filepath in sorted(esp_path.rglob("*")):
        if filepath.is_file():
            rel = filepath.relative_to(esp_path)
            # Use forward slashes for paths
            rel_str = str(rel).replace("\\", "/")
            data = filepath.read_bytes()
            files.append((rel_str, data))

    return files


def compress_file(data):
    """Compress with raw deflate. Returns (compressed_data, compressed_size).
    Returns (data, 0) if compression isn't worthwhile."""
    if len(data) < COMPRESS_THRESHOLD:
        return data, 0  # 0 means stored uncompressed

    # Use raw deflate (wbits=-15, no zlib/gzip header)
    compressed = zlib.compress(data, 9)[2:-4]  # strip zlib header/trailer

    # Only use compression if it actually saves space
    if len(compressed) >= len(data):
        return data, 0

    return compressed, len(compressed)


def pack_arch(name, esp_dir):
    """Pack one architecture. Returns (manifest_bytes, data_bytes, file_count)."""
    files = collect_files(esp_dir)
    if not files:
        print(f"  Warning: no files found in {esp_dir}")
        return b"", b"", 0

    manifest = b""
    data = b""

    for rel_path, file_data in files:
        compressed, comp_size = compress_file(file_data)
        orig_size = len(file_data)

        # Manifest entry: 128-byte path + 4-byte compressed_size + 4-byte original_size
        path_bytes = rel_path.encode("utf-8")[:127] + b"\x00"
        path_bytes = path_bytes.ljust(128, b"\x00")
        manifest += struct.pack("<128s I I", path_bytes, comp_size, orig_size)

        data += compressed

        status = f"deflate {comp_size}B" if comp_size > 0 else "stored"
        print(f"  {rel_path}: {orig_size}B -> {status}")

    return manifest, data, len(files)


def main():
    parser = argparse.ArgumentParser(description="Pack survival workstation payload")
    parser.add_argument("--build-dir", default="../build",
                       help="Build directory containing arch subdirs")
    parser.add_argument("--output", default="payload.bin",
                       help="Output payload binary")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    arches = []

    for arch_name in ["aarch64", "x86_64"]:
        esp_dir = build_dir / arch_name / "esp"
        if esp_dir.exists():
            arches.append((arch_name, str(esp_dir)))
            print(f"Found {arch_name} ESP at {esp_dir}")

    if not arches:
        print("Error: no ESP directories found. Build first with 'make all-arches'.")
        return 1

    # Build header
    header = struct.pack("<4s B B H", b"SURV", 1, len(arches), 0)

    # Calculate offsets: header + arch_table, then per-arch data
    arch_table_size = 24 * len(arches)
    data_offset = len(header) + arch_table_size

    arch_table = b""
    arch_blobs = []

    for arch_name, esp_dir in arches:
        print(f"\nPacking {arch_name}:")
        manifest, data, file_count = pack_arch(arch_name, esp_dir)
        blob = manifest + data

        # Arch table entry: 16-byte name + 4-byte offset + 4-byte file_count
        name_bytes = arch_name.encode("utf-8")[:15] + b"\x00"
        name_bytes = name_bytes.ljust(16, b"\x00")
        arch_table += struct.pack("<16s I I", name_bytes, data_offset, file_count)

        arch_blobs.append(blob)
        data_offset += len(blob)

    # Assemble final payload
    payload = header + arch_table
    for blob in arch_blobs:
        payload += blob

    # Write output
    output_path = Path(args.output)
    output_path.write_bytes(payload)

    print(f"\nPayload written: {output_path} ({len(payload)} bytes, "
          f"{len(payload)/1024:.1f} KB)")

    # Check against partition size (2.6MB = 0x290000 = 2,686,976 bytes)
    max_size = 0x290000
    if len(payload) > max_size:
        print(f"WARNING: payload exceeds partition size "
              f"({len(payload)} > {max_size} bytes)")
        return 1
    else:
        pct = len(payload) * 100 / max_size
        print(f"Partition usage: {pct:.1f}% of {max_size/1024:.0f} KB")

    return 0


if __name__ == "__main__":
    exit(main())
