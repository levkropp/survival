---
layout: default
title: "Chapter 1: The Mission"
parent: "Phase 1: Boot & Input"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 1
---

# Chapter 1: The Mission

## What Are We Building?

Imagine the world as you know it has ended. The internet is gone. The cloud is gone. Your phone is a brick. But you found a small single-board computer, a monitor, a keyboard, and a solar panel. Can you turn that into something useful?

That's the question this project answers.

We're building a **survival workstation** — a completely self-contained computer system that:

- Boots on any computer with UEFI firmware — x86_64 or ARM64
- Does **not** require Linux, Windows, or any operating system
- Includes a C compiler (TinyCC) so you can write and run programs
- Has a graphical interface with a text editor and file browser
- Contains an offline library of survival knowledge — first aid, water purification, agriculture, electrical repair, radio communication, and more
- Fits the **entire system** in 4 megabytes — small enough to store on an ESP32's flash
- Can be flashed onto an SD card by a tiny $7 ESP32 device

The goal is maximal apocalypse practicality with minimal complexity.

## Why No Operating System?

You might wonder: why not just run Linux? It's free, it's open source, it runs on ARM. Three reasons:

**1. Size.** A minimal Linux system — even a stripped-down BusyBox setup — takes tens of megabytes at minimum. A typical Linux kernel alone is 5-15 MB. We want our *entire system* including documentation to fit in 8 MB.

**2. Complexity.** Linux is millions of lines of code. If something breaks, you need deep kernel expertise to fix it. Our system is a few thousand lines of C. You can read every line, understand every line, and fix every line.

**3. Self-containment.** Our system includes its own compiler. If you want to change how the system works — add a feature, fix a bug, adapt it to different hardware — you can recompile it *on the device itself*. No internet, no package manager, no build server needed.

**4. Educational value.** Building from scratch teaches you how computers actually work at the lowest level. There's no better way to understand a machine than to program it with nothing between you and the hardware.

## The Hardware

### The Computer: Any UEFI Machine

Our workstation is a standard UEFI application. It runs on any computer with UEFI firmware — x86_64 laptops, desktops, servers, ARM64 single-board computers, or anything else that implements the UEFI specification. We build for two architectures:

- **aarch64** (ARM64) — single-board computers, ARM servers, phones repurposed as computers
- **x86_64** — any modern PC, laptop, or server built in the last 15 years

What the computer needs:

**UEFI firmware.** Nearly all modern computers have this. It's the standard firmware interface that replaced the old PC BIOS. On ARM boards, bootloaders like U-Boot or EDK2 provide UEFI. On PCs, the motherboard firmware is UEFI natively.

**A display.** HDMI, DisplayPort, or any output the firmware can drive. We use UEFI's Graphics Output Protocol (GOP) to paint pixels, so any display the firmware supports will work.

**A USB keyboard.** For input. The firmware provides a keyboard protocol — we don't need to write USB drivers.

**Removable storage.** An SD card, USB drive, or any FAT32-formatted boot device. The entire system lives on this.

**Power.** A solar panel, wall outlet, car battery — whatever you have. Most single-board computers draw under 5 watts.

### The Flasher: ESP32

The ESP32 is a tiny microcontroller with 4 MB of flash memory — cheap ($7), the size of a postage stamp, and can run for weeks on a battery. We use it for one job: writing the survival workstation image onto a blank SD card.

Why not just use a laptop to write the SD card? Because in our scenario, you might not have a laptop. The ESP32 stores the complete workstation image in its 4 MB of flash. You insert a blank SD card, press a button, and it writes the image. Then you put that SD card in any UEFI computer and boot.

This is our hard size constraint: **the entire workstation — binary, compiler, and survival documentation — must fit in 4 MB**.

The ESP32 also serves as a standalone survival reference — its small screen can display essential survival instructions even without a computer.

### The Full Kit

Here's what the complete survival workstation looks like:

```
┌─────────────────────────────────────────────┐
│                                             │
│   [Solar Panel] ──── [Power] ──── [Any UEFI │
│                                    Computer]│
│                                      │      │
│                                   [Display] │
│                                      │      │
│                                  [Monitor]  │
│                                             │
│   [USB Keyboard] ──── [USB] ──┘             │
│                                             │
│   [ESP32 Flasher] ── writes ── [SD Card]    │
│                                             │
└─────────────────────────────────────────────┘
```

## What "Bare Metal" Means

Throughout this book, we'll use the term "bare metal." It means our code runs directly on the hardware with no operating system underneath. When you run a normal program — say, a Python script — there are many layers between your code and the hardware:

```
Your Python Script
       ↓
Python Interpreter
       ↓
C Runtime Library (libc)
       ↓
System Calls (read, write, mmap, ...)
       ↓
Linux Kernel
       ↓
Device Drivers
       ↓
Hardware
```

Each layer provides abstractions. `print("hello")` in Python eventually becomes electrical signals on a wire, but you don't have to think about that.

In our system, the stack is much shorter:

```
Our C Code
       ↓
UEFI Boot Services
       ↓
Hardware
```

We talk to the hardware through UEFI — a thin firmware layer that provides basic services like "give me access to the screen" and "read from the keyboard." We'll explain UEFI in detail in Chapter 2.

## The 4 MB Budget

One of the most interesting constraints of this project is size. Our entire system must fit in 4 MB — the flash capacity of an ESP32. Here's our budget:

```
Component              Target Size
─────────────────────  ───────────
UEFI binary (code)        ~650 KB
TinyCC (compiled in)      (included above)
Survival documentation   ~2-3 MB
Overhead (FAT32, etc.)    ~200 KB
                        ─────────
Total                   ~3-4 MB
```

Both architecture binaries are well under 1 MB each — aarch64 is about 653 KB, x86_64 is about 522 KB. That leaves most of our 4 MB budget for survival documentation.

For context: a single high-resolution photo on your phone is typically 3-5 MB. We're fitting an entire computer system with a compiler and a survival library in less space than one photo.

## The Road Ahead

This book is organized by phase. In Phase 1 (these chapters), we build the foundation:

- **Chapter 2:** How computers boot, and why we chose UEFI
- **Chapter 3:** Setting up the cross-compilation toolchain
- **Chapter 4:** Writing our first UEFI application — "Hello, Firmware"
- **Chapter 5:** Memory management without an OS
- **Chapter 6:** Painting pixels — framebuffers and font rendering
- **Chapter 7:** Reading keyboard input
- **Chapter 8:** The main loop — wiring it all together

By the end of Phase 1, we'll have a working system that boots on real hardware (or in an emulator), displays text on screen, and accepts keyboard input. It's not much to look at — just a blinking cursor on a black screen — but it represents something remarkable: a computer running *your* code with nothing else between you and the machine.

Let's begin.

---

**Next:** [Chapter 2: How Computers Boot](chapter-02-how-computers-boot)
