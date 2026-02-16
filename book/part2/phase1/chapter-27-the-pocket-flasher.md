---
layout: default
title: "Chapter 27: The Pocket Flasher"
parent: "Phase 1: The Pocket Flasher"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 1
---

# Chapter 27: The Pocket Flasher

## The Distribution Problem

Part 1 gave us a survival workstation — a bare-metal UEFI application that boots on any computer, includes a C compiler, a text editor, a file browser, and a library of survival knowledge. All of it fits in under 4 MB. All of it can rebuild itself from source.

But we skipped a question. How does the workstation get onto an SD card in the first place?

In the chapters so far, we cross-compiled from a Linux machine, copied the binary to a FAT32 partition, and booted. That works fine when you have a laptop with a working OS. In a survival scenario, you might not. The whole point of the project is to work when infrastructure has collapsed. If you need infrastructure to deploy it, you've built a lifeboat that can only be launched from a yacht.

We need a device that:

- Stores the complete workstation image
- Writes it to a blank SD card without any other computer
- Is cheap enough to stockpile
- Is small enough to carry in a pocket
- Runs on minimal power

## The ESP32 CYD

The answer is the **ESP32-2432S028R**, known informally as the "Cheap Yellow Display" or CYD. It costs about $7, ships from dozens of vendors, and packs everything we need onto a board the size of a credit card:

```
┌──────────────────────────────────────────────────┐
│                                                  │
│   ┌──────────────────────────┐  ┌──────────┐    │
│   │                          │  │ micro-USB │    │
│   │   2.8" ILI9341 Display   │  └──────────┘    │
│   │      320 × 240 pixels    │                   │
│   │      RGB565 (16-bit)     │  ┌──────────┐    │
│   │                          │  │ SD card   │    │
│   │   XPT2046 Resistive      │  │ slot      │    │
│   │   Touchscreen (overlay)  │  └──────────┘    │
│   │                          │                   │
│   └──────────────────────────┘                   │
│                                                  │
│   ESP32 (Xtensa dual-core, 240 MHz)             │
│   4 MB SPI flash, 520 KB SRAM                   │
│   Wi-Fi + Bluetooth (unused)                     │
│                                                  │
└──────────────────────────────────────────────────┘
```

Four components matter:

**The ESP32 MCU.** A dual-core 240 MHz processor with 520 KB of SRAM and 4 MB of external SPI flash. It runs FreeRTOS (a real-time operating system for microcontrollers) and has an excellent SDK called ESP-IDF. The Wi-Fi and Bluetooth radios are present but we never turn them on — they are irrelevant to our task and would waste power.

**The ILI9341 display.** A 2.8-inch TFT LCD running at 320x240 pixels in RGB565 (16-bit color). It connects to the ESP32 via SPI and can be driven at 40 MHz — fast enough to update the screen in real time. We use it to show a menu, a progress bar, and status messages.

**The XPT2046 touchscreen.** A resistive touch controller overlaid on the display. It reads finger position as analog voltages and converts them to coordinates. We use it for one thing: detecting which button the user tapped. No gestures, no multitouch, no scrolling — just "where did the finger land?"

**The SD card slot.** A standard micro-SD socket wired to the ESP32's SPI bus. We write the workstation image here. SD cards speak SPI natively (it's their slow but simple interface), so no special hardware is needed.

## The Constraints

The ESP32 is orders of magnitude less powerful than even a basic laptop. Understanding the constraints shapes every design decision:

**4 MB flash.** The ESP32's external flash holds everything: the firmware (our flasher application), FreeRTOS, ESP-IDF runtime libraries, and the compressed workstation payload. The partition layout splits this into:

```
Address     Size      Purpose
──────────  ────────  ──────────────────────────────
0x009000    24 KB     NVS (non-volatile storage, ESP-IDF bookkeeping)
0x010000    1.4 MB    Application firmware
0x170000    2.6 MB    Workstation payload (compressed images)
```

The application gets 1.4 MB. The payload gets 2.6 MB. The workstation binaries — both aarch64 and x86_64, with all their supporting files — must fit in that 2.6 MB after compression.

**520 KB SRAM.** This is all the working memory we have. FreeRTOS and ESP-IDF claim about 340 KB of it for their stacks, heaps, and driver buffers. That leaves roughly 180 KB for our code's data. We need to decompress files, build FAT32 structures, compute CRC32 checksums, and manage directory entries — all within that budget. Every buffer allocation is a conscious decision.

**SPI for everything.** The display is on SPI2. The SD card is on SPI3. The touchscreen would need a third SPI bus, but the ESP32 only has two hardware SPI controllers available (the third is reserved for the flash chip). So the touch controller uses bit-banged SPI — we toggle GPIO pins manually in software. This is slow (~50 Hz polling rate) but the touchscreen only needs to detect taps, not track smooth motion.

## What We're Building

Over the next seven chapters, we build the complete flasher:

- **Chapter 28:** Set up the ESP-IDF toolchain, configure the project, and get the display and touch working
- **Chapter 29:** Drive the ILI9341 display — SPI configuration, landscape mode, drawing text and rectangles, reading touch input
- **Chapter 30:** Talk to the SD card — SPI3 configuration, sector-level read and write
- **Chapter 31:** Create a GPT partition table — protective MBR, primary and backup headers, CRC32
- **Chapter 32:** Build a FAT32 filesystem — BPB, FAT tables, directories, long filenames, streaming writes
- **Chapter 33:** Pack the payload — compression, multi-architecture support, the binary format, the Python packing script
- **Chapter 34:** Flash! — orchestrate the whole sequence, stream-decompress files, verify, and boot

The architecture mirrors Part 1's approach: each chapter builds one layer, each layer depends only on the layers below it.

```
┌──────────────────────────────────────┐
│  Chapter 34: Flasher (orchestrator)  │
├──────────┬──────────┬────────────────┤
│ Ch.33    │ Ch.32    │ Ch.31          │
│ Payload  │ FAT32    │ GPT            │
├──────────┴──────────┴────────────────┤
│  Chapter 30: SD Card (sector I/O)    │
├──────────────────────────────────────┤
│  Chapter 29: Display + Touch (UI)    │
├──────────────────────────────────────┤
│  Chapter 28: ESP-IDF (toolchain)     │
└──────────────────────────────────────┘
```

By Chapter 34, the flow is: power on the ESP32 → splash screen → tap an architecture → insert SD card → GPT → FAT32 → decompress and write files → verify → done. Pull the card out, put it in any UEFI computer, boot, and you have a survival workstation that can rebuild itself.

## The Source Files

The complete flasher is about 2,000 lines of C across ten source files, plus a 180-line Python script for packing the payload:

```
esp32/main/
  main.c        111 lines   Entry point and main loop
  display.c     157 lines   ILI9341 display driver
  display.h      45 lines   Display interface
  touch.c       168 lines   XPT2046 touch driver
  touch.h        21 lines   Touch interface
  sdcard.c      131 lines   SD card sector I/O
  sdcard.h       33 lines   SD card interface
  gpt.c         217 lines   GPT partition table creation
  gpt.h          22 lines   GPT interface
  fat32.c       893 lines   FAT32 filesystem (largest file)
  fat32.h        49 lines   FAT32 interface
  flasher.c     173 lines   Flash sequence orchestrator
  flasher.h      14 lines   Flasher interface
  payload.c     161 lines   Payload manifest reader
  payload.h      52 lines   Payload interface
  ui.c          166 lines   Touch UI (menus, progress, done/error)
  ui.h           32 lines   UI interface
  font.c        202 lines   8x16 VGA font data
  font.h         20 lines   Font interface

esp32/scripts/
  pack_payload.py  179 lines   Payload packer
```

FAT32 is the largest module by far — building a filesystem from scratch takes real work, as we learned in Part 1. But the ESP32 version benefits from having already solved it once. The code is a port of Part 1's `src/fat32.c`, adapted to use standard C types instead of UEFI types and to write through the SD card SPI driver instead of UEFI's block I/O protocol.

## Why Not Just dd an Image?

You might wonder: why not create a complete disk image on the Linux build machine and have the ESP32 write it sector by sector? No GPT creation, no FAT32 formatting, no file-by-file decompression — just a raw byte-for-byte copy.

Two reasons.

**Size.** A raw disk image of even a small FAT32 partition is mostly zeros. A 64 MB partition with 2 MB of files has 62 MB of empty space. Even compressed, the image carries overhead from the filesystem metadata structure. Packing individual files with per-file deflate compression is more space-efficient because we only store the data that matters.

**Flexibility.** The flasher adapts to whatever SD card you insert. A 2 GB card gets a 2 GB partition. A 32 GB card gets a 32 GB partition. The GPT and FAT32 structures are computed at flash time based on the card's actual size. With a raw image, you'd need a different image for every card size — or waste most of the card.

The file-by-file approach is more code (we have to implement GPT and FAT32 creation), but it produces a better result: the entire SD card is usable, the filesystem is clean, and the payload compresses better.

## Let's Begin

In the next chapter, we set up the ESP-IDF toolchain, create the project skeleton, and get our first signs of life from the display. The ESP32 is a different world from UEFI — we have an RTOS, a proper SDK, and a C standard library. But the constraint is tighter: 520 KB of RAM instead of gigabytes, and SPI buses instead of memory-mapped framebuffers.

The survival workstation is about to get a pocket-sized deployment device.
