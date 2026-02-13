# Chapter 1: The Mission

## What Are We Building?

Imagine the world as you know it has ended. The internet is gone. The cloud is gone. Your phone is a brick. But you found a small single-board computer, a monitor, a keyboard, and a solar panel. Can you turn that into something useful?

That's the question this project answers.

We're building a **survival workstation** — a completely self-contained computer system that:

- Boots from a blank microSD card
- Does **not** require Linux, Windows, or any operating system
- Includes a C compiler (TinyCC) so you can write and run programs
- Has a graphical interface with a text editor and file browser
- Contains an offline library of survival knowledge — first aid, water purification, agriculture, electrical repair, radio communication, and more
- Fits the **entire system** in 8 megabytes (or a stripped-down version in 4 MB)
- Can be flashed onto an SD card by a tiny $7 ESP32 device

The goal is maximal apocalypse practicality with minimal complexity.

## Why No Operating System?

You might wonder: why not just run Linux? It's free, it's open source, it runs on ARM. Three reasons:

**1. Size.** A minimal Linux system — even a stripped-down BusyBox setup — takes tens of megabytes at minimum. A typical Linux kernel alone is 5-15 MB. We want our *entire system* including documentation to fit in 8 MB.

**2. Complexity.** Linux is millions of lines of code. If something breaks, you need deep kernel expertise to fix it. Our system is a few thousand lines of C. You can read every line, understand every line, and fix every line.

**3. Self-containment.** Our system includes its own compiler. If you want to change how the system works — add a feature, fix a bug, adapt it to different hardware — you can recompile it *on the device itself*. No internet, no package manager, no build server needed.

**4. Educational value.** Building from scratch teaches you how computers actually work at the lowest level. There's no better way to understand a machine than to program it with nothing between you and the hardware.

## The Hardware

### The Computer: Libre Computer Sweet Potato V2

Our target is the **Libre Computer AML-S905X-CC-V2**, affectionately called the "Sweet Potato." It's a single-board computer similar to a Raspberry Pi, but with some advantages for our use case.

```
┌──────────────────────────────────────────┐
│  Libre Computer Sweet Potato V2          │
│  ┌──────────────────────────────────┐    │
│  │  Amlogic S905X SoC               │    │
│  │  ┌───────────┐  ┌───────────┐   │    │
│  │  │ Cortex-A53│  │ Cortex-A53│   │    │
│  │  │  Core 0   │  │  Core 1   │   │    │
│  │  └───────────┘  └───────────┘   │    │
│  │  ┌───────────┐  ┌───────────┐   │    │
│  │  │ Cortex-A53│  │ Cortex-A53│   │    │
│  │  │  Core 2   │  │  Core 3   │   │    │
│  │  └───────────┘  └───────────┘   │    │
│  └──────────────────────────────────┘    │
│                                          │
│  RAM: 2 GB DDR3                          │
│                                          │
│  [HDMI]  [USB]  [USB]  [microSD]  [USB-C]│
│   video   kbd   mouse   storage    power │
└──────────────────────────────────────────┘
```

Let's break down what matters:

**SoC: Amlogic S905X.** "SoC" stands for System-on-Chip. Instead of having a separate CPU, memory controller, and I/O chips like a desktop PC, everything is integrated into one chip. The S905X contains:

- Four ARM Cortex-A53 CPU cores (64-bit, ARMv8-A architecture)
- A Mali-450 GPU (which we won't use)
- HDMI output circuitry
- USB controllers
- An SD/MMC card interface
- A UART (serial port) for debugging

**ARM Cortex-A53.** This is the CPU core. "ARM" is a processor architecture — a different language of machine instructions than the x86 chips in your laptop. Where x86 uses instructions like `MOV EAX, 42`, ARM uses instructions like `MOV X0, #42`. Same idea, different encoding. The "A53" is a specific implementation designed for efficiency. The "64-bit" part (ARMv8-A) means it can address large amounts of memory and work with 64-bit values natively.

**2 GB RAM.** More than enough. Our entire system fits in 8 MB, so we have roughly 250 times more RAM than we need for the code. The rest is available for compiling programs, editing files, and other runtime work.

**HDMI output.** This is how we display our GUI. The HDMI controller sends video data to any standard monitor or TV.

**microSD slot.** This is our "hard drive." The entire system lives on an SD card.

**USB ports.** For keyboard and mouse input.

**USB-C power.** The board draws about 5 watts. A small solar panel can power it.

### The Flasher: ESP32

The ESP32 is a tiny microcontroller — much simpler than the Sweet Potato. Think of it as a programmable gadget rather than a computer. We use it for one job: writing the survival workstation image onto a blank SD card.

Why not just use a laptop to write the SD card? Because in our scenario, you might not have a laptop. The ESP32 is cheap ($7), tiny, and can be pre-programmed with our system image. You insert a blank SD card, press a button, and it writes the image. Then you put that SD card in the Sweet Potato and boot.

The ESP32 also serves as a standalone survival reference — its small screen can display essential survival instructions even without the Sweet Potato.

### The Full Kit

Here's what the complete survival workstation looks like:

```
┌─────────────────────────────────────────────┐
│                                             │
│   [Solar Panel] ──── [USB-C] ──── [Sweet    │
│                                    Potato]  │
│                                      │      │
│                                    [HDMI]   │
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

## The 8 MB Budget

One of the most interesting constraints of this project is size. Our full system must fit in 8 MB. Here's our budget:

```
Component              Target Size
─────────────────────  ───────────
Boot + runtime code       ~1 MB
TinyCC compiler          ~1.2 MB
GUI system               ~800 KB
Filesystem + support     ~500 KB
Survival documentation   ~3-4 MB
                        ─────────
Total                   ~6.5-7.5 MB
```

For the 4 MB "minimal" build, we cut the documentation to the five most essential categories and remove the search index.

For context: a single high-resolution photo on your phone is typically 3-5 MB. We're fitting an entire computer system with a compiler and a survival library in less space than two photos.

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
