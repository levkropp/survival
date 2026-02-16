# Survival Workstation

A bare-metal UEFI application that boots from a microSD card with no operating system. Just a C compiler, text editor, file browser, and the knowledge to rebuild.

Runs on **any UEFI machine** — aarch64 or x86_64. The entire system fits in 4 MB (ESP32 flash).

**[Read the book](https://levkropp.github.io/survival)** — 26 chapters walking through every line of code.

## What This Is

A self-contained development workstation that:

- Boots directly from UEFI firmware — no Linux, no POSIX, no OS
- Includes [TinyCC](https://bellard.org/tcc/) as an in-memory C compiler
- Has a framebuffer-based text editor with syntax highlighting
- Browses and manages files on FAT32, exFAT, and NTFS volumes
- Can rebuild itself from its own source code (self-hosting)
- Clones itself to USB drives
- Creates ISO 9660 images and formats FAT32 volumes
- Fits in 4 MB — small enough to store on an ESP32's flash

## Quick Start

```bash
# Install toolchain
./scripts/setup-toolchain.sh

# Build for ARM64 (default)
make

# Build for x86_64
make ARCH=x86_64

# Build both
make all-arches

# Test in QEMU
./scripts/run-qemu.sh          # aarch64
./scripts/run-qemu-x86_64.sh   # x86_64
```

## Hardware

Any computer with UEFI firmware:

- **aarch64**: ARM single-board computers (with U-Boot or EDK2), ARM servers
- **x86_64**: Any PC, laptop, or server built in the last 15 years
- **Display**: Any HDMI/DisplayPort monitor
- **Input**: USB keyboard
- **Storage**: microSD card or USB drive (FAT32)

A $7 ESP32 can store the complete system image in its 4 MB of flash and write it to a blank SD card — no laptop needed.

## Features

| Feature | Key | Description |
|---------|-----|-------------|
| File browser | | Navigate FAT32, exFAT, and NTFS volumes |
| Text editor | | Syntax-highlighted C editor |
| Save | F2 | Write file to disk |
| Select | F3 | Text selection mode |
| Copy/Paste | F3/F8 | Copy and paste files in browser |
| New file | F4 | Create new file or directory |
| Compile & run | F5 | Compile current .c file and execute it |
| Rebuild | F6 | Recompile the workstation from source |
| Linux installer | F7 | Write a Linux ISO to USB |
| Paste | F8 | Paste copied file |
| Rename | F9 | Rename file or directory |
| ISO writer | F10 | Stream an ISO image to a block device |
| Format | F11 | Format a disk as FAT32 |
| Clone | F12 | Clone the workstation to a USB drive |

## Project Structure

```
src/           Source code (UEFI application)
  main.c        Entry point, main loops
  fb.c          Framebuffer driver (GOP)
  kbd.c         Keyboard input (SimpleTextInputEx)
  mem.c         Memory allocator (UEFI AllocatePool)
  fs.c          FAT32 filesystem + volume abstraction
  exfat.c       exFAT read/write driver
  ntfs.c        NTFS read-only driver
  browse.c      File browser UI
  edit.c        Text editor
  tcc.c         TCC runtime wrapper
  shim.c        libc shim for TCC (~1100 lines)
  disk.c        BlockIO protocol + raw block I/O
  fat32.c       FAT32 format tool
  iso.c         ISO 9660 writer
  installer.c   Linux USB installer
  font.c        8x16 VGA bitmap font
book/          26 chapters documenting every line
scripts/       Build and test scripts
tools/tinycc/  TinyCC source (patched for UEFI)
```

## The Book

The entire project is documented as a narrative book — 26 chapters across 8 phases, from first boot to self-hosting compiler. Read it online at **[levkropp.github.io/survival](https://levkropp.github.io/survival)**.

| Phase | Chapters | Topic |
|-------|----------|-------|
| 1 | 1–8 | Boot, framebuffer, keyboard, memory |
| 2 | 9–12 | Filesystem, file browser, text editor |
| 3 | 13–16, 18 | TCC compiler, UEFI PE output, self-hosting |
| 3.5 | 17 | Syntax highlighting |
| 4 | 19–20 | USB volume browsing, cloning |
| 4.5 | 21 | Self-self-hosting, dogfooding |
| 5 | 22–24 | Streaming I/O, ISO writer, format tool |
| 5.5 | 25–26 | exFAT read/write, NTFS read-only |