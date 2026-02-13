# Survival Workstation

Bare-metal ARM64 survival development terminal for the Libre Computer Sweet Potato V2 (Amlogic S905X).

No Linux. No POSIX. Just a UEFI application with a C compiler, text editor, and offline survival documentation.

## Quick Start

```bash
# Install toolchain
./scripts/setup-toolchain.sh

# Build
make

# Test in QEMU
./scripts/run-qemu.sh
```

## What This Is

A self-contained apocalypse-ready development + knowledge terminal that:
- Boots from a blank microSD card (via ESP32 flasher)
- Includes TinyCC (C compiler)
- Has a framebuffer-based GUI with text editor and file browser
- Contains offline survival documentation (first aid, water, agriculture, electrical, etc.)
- Fits in 8 MB (full) or 4 MB (minimal)

## Hardware

- **Board**: Libre Computer Sweet Potato V2 (AML-S905X-CC-V2)
- **SoC**: Amlogic S905X (ARM Cortex-A53, 64-bit ARMv8-A)
- **RAM**: 2 GB
- **Display**: Any HDMI monitor
- **Input**: USB keyboard + mouse
- **Storage**: microSD card (FAT32)
- **Bootstrap**: ESP32 device writes the SD image

## Project Structure

```
src/          Source code (UEFI application)
scripts/      Build and test scripts
build/        Build output (generated)
docs/         Documentation and roadmaps
esp32/        ESP32 flasher firmware (future)
```

## Current Status

**Phase 1**: Boot + framebuffer + keyboard input (in progress)
