---
layout: default
title: "Phase 1: The Pocket Flasher"
parent: "Part 2: The ESP32 That Saves the World"
nav_order: 1
has_children: true
---

# Phase 1: The Pocket Flasher

From a bare ESP32 board to a self-contained SD card flasher. We initialize the display and touchscreen, talk to the SD card over SPI, create a GPT partition table, format a FAT32 filesystem, decompress the workstation payload, and write a verified bootable card — all in under 2,000 lines of C on a microcontroller with 520 KB of RAM.
