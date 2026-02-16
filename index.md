---
layout: default
title: Home
nav_order: 0
---

# Survival Workstation

A bare-metal UEFI application that boots from a microSD card with no operating system — just a C compiler, text editor, file browser, and the knowledge to rebuild.

This book walks through building the entire system from scratch, chapter by chapter.

---

## The Book

### Part 1: The Bare-Metal Workstation

#### [Phase 1: Boot & Input](book/part1/phase1/)

| # | Chapter |
|---|---------|
| 1 | [The Mission](book/part1/phase1/chapter-01-the-mission) |
| 2 | [How Computers Boot](book/part1/phase1/chapter-02-how-computers-boot) |
| 3 | [Setting Up the Workshop](book/part1/phase1/chapter-03-setting-up-the-workshop) |
| 4 | [Hello, UEFI](book/part1/phase1/chapter-04-hello-uefi) |
| 5 | [Memory Without an OS](book/part1/phase1/chapter-05-memory-without-an-os) |
| 6 | [Painting Pixels](book/part1/phase1/chapter-06-painting-pixels) |
| 7 | [Hearing Keystrokes](book/part1/phase1/chapter-07-hearing-keystrokes) |
| 8 | [The Main Loop](book/part1/phase1/chapter-08-the-main-loop) |

#### [Phase 2: Filesystem & Editor](book/part1/phase2/)

| # | Chapter |
|---|---------|
| 9 | [Reading the Disk](book/part1/phase2/chapter-09-reading-the-disk) |
| 10 | [Browsing Files](book/part1/phase2/chapter-10-browsing-files) |
| 11 | [Writing to Disk](book/part1/phase2/chapter-11-writing-to-disk) |
| 12 | [The Text Editor](book/part1/phase2/chapter-12-the-text-editor) |

#### [Phase 3: The C Compiler](book/part1/phase3/)

| # | Chapter |
|---|---------|
| 13 | [The C Compiler](book/part1/phase3/chapter-13-the-c-compiler) |
| 14 | [Write, Run, Repeat](book/part1/phase3/chapter-14-write-run-repeat) |
| 15 | [Teaching TCC to Output UEFI](book/part1/phase3/chapter-15-teaching-tcc-to-output-uefi) |
| 16 | [The Self-Hosting Rebuild](book/part1/phase3/chapter-16-the-self-hosting-rebuild) |
| 18 | [x86_64 Self-Hosting](book/part1/phase3/chapter-18-x86-64-self-hosting) |

#### [Phase 3.5: Syntax Highlighting](book/part1/phase3_5/)

| # | Chapter |
|---|---------|
| 17 | [Syntax Highlighting](book/part1/phase3_5/chapter-17-syntax-highlighting) |

#### [Phase 4: USB & Cloning](book/part1/phase4/)

| # | Chapter |
|---|---------|
| 19 | [USB Volumes](book/part1/phase4/chapter-19-usb-volumes) |
| 20 | [Cloning to USB](book/part1/phase4/chapter-20-cloning-to-usb) |

#### [Phase 4.5: Self-Self-Hosting](book/part1/phase4_5/)

| # | Chapter |
|---|---------|
| 21 | [Eating Your Own Dogfood](book/part1/phase4_5/chapter-21-eating-your-own-dogfood) |

#### [Phase 5: Streaming I/O & Tools](book/part1/phase5/)

| # | Chapter |
|---|---------|
| 22 | [Streaming I/O](book/part1/phase5/chapter-22-streaming-io) |
| 23 | [The ISO Writer](book/part1/phase5/chapter-23-the-iso-writer) |
| 24 | [The Format Tool](book/part1/phase5/chapter-24-the-format-tool) |

#### [Phase 5.5: exFAT & NTFS](book/part1/phase5_5/)

| # | Chapter |
|---|---------|
| 25 | [Reading and Writing exFAT](book/part1/phase5_5/chapter-25-reading-and-writing-exfat) |
| 26 | [Reading NTFS](book/part1/phase5_5/chapter-26-reading-ntfs) |

### [Part 2: The ESP32 That Saves the World](book/part2/)

#### [Phase 1: The Pocket Flasher](book/part2/phase1/)

| # | Chapter |
|---|---------|
| 27 | [The Pocket Flasher](book/part2/phase1/chapter-27-the-pocket-flasher) |
| 28 | [Setting Up the ESP32](book/part2/phase1/chapter-28-setting-up-the-esp32) |
| 29 | [Driving the Display](book/part2/phase1/chapter-29-driving-the-display) |
| 30 | [Talking to the SD Card](book/part2/phase1/chapter-30-talking-to-the-sd-card) |
| 31 | [Creating a GPT](book/part2/phase1/chapter-31-creating-a-gpt) |
| 32 | [Building FAT32](book/part2/phase1/chapter-32-building-fat32) |
| 33 | [Packing the Payload](book/part2/phase1/chapter-33-packing-the-payload) |
| 34 | [Flash!](book/part2/phase1/chapter-34-flash) |
