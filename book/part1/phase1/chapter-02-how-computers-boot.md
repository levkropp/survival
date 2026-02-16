---
layout: default
title: "Chapter 2: How Computers Boot"
parent: "Phase 1: Boot & Input"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 2
---

# Chapter 2: How Computers Boot

## The Cold Start Problem

When you press the power button on a computer, the CPU wakes up with no memory of anything. RAM is empty. There's no operating system loaded. The CPU doesn't even know what kind of storage devices are attached or how to read from them.

So how does anything happen?

The answer is a chain of increasingly capable programs, each one loading the next. Think of it like starting a fire: you can't light a log with a match. You light a match, use it to ignite kindling, use the kindling to start small sticks, and use the sticks to light the log. Each step bootstraps the next — and in fact, this is where the term **"booting"** comes from. It's short for "bootstrapping," from the old phrase "pulling yourself up by your bootstraps."

## The Boot Chain

Every computer goes through a boot chain — a series of increasingly capable programs, each loading the next. The details vary by hardware, but the pattern is universal:

```
Step 1: ROM Code / Reset Vector
  Burned into the chip at the factory. Cannot be changed.
  Knows how to find and load the next stage from flash or disk.
          ↓
Step 2: Platform Initialization
  Initializes RAM, clocks, and basic hardware.
  On PCs: the motherboard firmware (UEFI/BIOS).
  On ARM boards: vendor bootloaders (BL2/BL3).
          ↓
Step 3: UEFI Firmware
  Provides standard services: screen, keyboard, filesystem.
  On PCs: built into the motherboard firmware.
  On ARM boards: provided by U-Boot or EDK2.
          ↓
Step 4: Our Code (survival.efi)
  Uses UEFI services to access screen, keyboard, filesystem.
  This is what we're building.
```

Let's walk through each step.

### Steps 1–2: Hardware Initialization

When power is applied, the CPU starts executing code from a fixed address — either ROM burned into the chip or a flash chip on the board. This early code initializes the bare essentials: CPU clocks, DDR RAM, and basic I/O.

On an **x86_64 PC**, the motherboard firmware handles all of this. You press the power button and the firmware initializes everything — CPU, RAM, PCIe, USB, display — before presenting the UEFI interface. This firmware is stored in a flash chip on the motherboard.

On **ARM single-board computers**, the process is more fragmented. A ROM bootloader burned into the SoC loads vendor-specific code that initializes DDR RAM (this often requires proprietary blobs with precise timing parameters for the specific memory chips on the board). Then a bootloader like U-Boot takes over.

The details of this early boot stage are hardware-specific and not something we need to worry about. By the time our code runs, RAM is initialized, the display is ready, and UEFI services are available.

### Step 3: UEFI Firmware

This is where things become standardized. Regardless of the underlying hardware, UEFI firmware provides the same interface. On PCs, UEFI is built into the motherboard firmware. On ARM boards, it's typically provided by U-Boot or EDK2 (an open-source UEFI implementation).

The firmware initializes hardware, finds boot devices, and loads UEFI applications. It provides services for screen output, keyboard input, filesystem access, and memory management — everything we need.

### Step 4: Our Code

Our application is a UEFI executable. The firmware loads it from an SD card or USB drive and runs it. We'll explain what that means next.

## What is UEFI?

UEFI stands for **Unified Extensible Firmware Interface**. It's a standard — a specification document — that defines how software can talk to firmware (the low-level code that runs before an operating system).

Think of UEFI as a **contract**. The firmware says: "If you call these functions in this specific way, I will do these things for you." It defines:

- How to draw on the screen
- How to read from the keyboard
- How to read and write files on FAT32 filesystems
- How to allocate memory
- How to find out what hardware is available
- How to set the system clock
- How to shut down or reboot

The beauty of UEFI is that it's the same interface regardless of the underlying hardware. Whether the firmware is on a PC motherboard, U-Boot on an ARM board, or EDK2 on a server — if it implements UEFI, our code runs on it. We build for both aarch64 and x86_64, and the same C source compiles for both.

### UEFI vs. BIOS

If you've heard of BIOS, UEFI is its modern replacement. The old PC BIOS (Basic Input/Output System) dates back to 1981 and has severe limitations: 16-bit mode, 1 MB memory limit, no standard for graphics beyond text mode. UEFI was designed from scratch to support modern hardware: 64-bit processors, large displays, multiple storage devices, and more.

### Why UEFI and Not Full Bare-Metal?

We could write code that talks directly to hardware registers — configuring display controllers, programming USB host adapters, reading SD card sectors. This is called "full bare-metal" programming. There are good reasons to use UEFI instead:

**1. Hardware diversity.** Every SoC and chipset has different registers, different display controllers, different USB implementations. Writing bare-metal drivers locks you to one specific board. UEFI abstracts this away — our code runs on any UEFI machine.

**2. USB is incredibly complex.** A USB keyboard seems simple, but the USB protocol stack involves device enumeration, descriptor parsing, hub management, HID (Human Interface Device) class drivers, and interrupt transfer scheduling. Writing a USB stack from scratch is thousands of lines of tricky code.

**3. UEFI gives us all three for free.** Screen, keyboard, and filesystem — the three things we need most — are all provided by UEFI with simple function calls. We can focus on building our actual application instead of reinventing these wheels.

**4. It's still "no OS."** Even though we use UEFI, we're not running Linux or any operating system. Our code has complete control of the machine. We're just using UEFI as a hardware abstraction layer — a set of helper functions that the firmware provides.

## What is a UEFI Application?

A UEFI application is an executable file with a `.efi` extension. It's in the **PE (Portable Executable)** format — the same format that Windows uses for `.exe` files. This might seem strange for an ARM board, but the UEFI specification chose PE/COFF as its standard binary format for all architectures.

A UEFI application has an entry point function, just like a regular C program has `main()`. In UEFI, the entry point looks like this:

```c
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
```

The firmware calls this function when it loads your application. It passes two parameters:

**`ImageHandle`** — A handle (think of it as an ID number) that represents your loaded application. You'll pass this back to various UEFI functions that need to know who's asking.

**`SystemTable`** — A pointer to the **EFI System Table**. This is the master data structure. It contains pointers to every service UEFI provides:

```
EFI_SYSTEM_TABLE
├── ConIn          → Keyboard input protocol
├── ConOut         → Text console output protocol
├── BootServices   → Memory allocation, protocol lookup, events, timers
├── RuntimeServices → Clock, reset, shutdown, variables
└── ...
```

When we want to print text to the console, we call:
```c
SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Hello!\r\n");
```

When we want to allocate memory:
```c
SystemTable->BootServices->AllocatePool(EfiLoaderData, size, &pointer);
```

When we want to find the graphics protocol:
```c
SystemTable->BootServices->LocateProtocol(&GraphicsGuid, NULL, &graphics);
```

Every interaction with the hardware goes through the System Table.

## UEFI Protocols

UEFI organizes its services into **protocols**. A protocol is an interface — a collection of related functions that work together. Each protocol is identified by a GUID (Globally Unique Identifier), a 128-bit number that's guaranteed to be unique.

The protocols we'll use:

### Graphics Output Protocol (GOP)

```
GUID: 9042a9de-23dc-4a38-96fb-7aded080516a
```

GOP gives us access to the display. Through it, we can:
- Query available screen resolutions
- Set the screen mode
- Get the framebuffer address — a region of memory where each pixel is represented by a 32-bit value. Write to this memory and the pixels appear on screen.

### Simple Text Input Protocol

```
GUID: 387477c1-69c7-11d2-8e39-00a0c969723b
```

This gives us keyboard input. We can:
- Wait for a key press
- Read what key was pressed
- Get both the ASCII character (for normal keys) and a scan code (for special keys like arrows, function keys, etc.)

### Simple File System Protocol

```
GUID: 964e5b22-6459-11d2-8e39-00a0c969723b
```

This gives us FAT32 filesystem access. We can:
- Open the root directory of a partition
- List files and directories
- Open, read, write, and create files

We won't use this protocol until Phase 2, but it's good to know it exists.

## How Our Application Gets Loaded

When a UEFI machine boots, the firmware looks for a UEFI application on available boot devices (SD card, USB drive, hard disk). It follows a standard path convention:

```
Boot Device (FAT32)
└── EFI/
    └── BOOT/
        ├── BOOTAA64.EFI     ← ARM64 version
        └── BOOTX64.EFI      ← x86_64 version
```

These filenames are defined by the UEFI specification. "BOOT" means "default boot application," "AA64" means AArch64 (64-bit ARM), and "X64" means x86_64. The firmware picks the one matching its architecture.

When the firmware finds the file, it:
1. Loads the PE binary into RAM
2. Performs relocations (adjusting memory addresses — more on this in Chapter 3)
3. Calls our `efi_main()` function
4. Passes us the ImageHandle and SystemTable

From that point on, we're in control.

## The Contract We Must Follow

Using UEFI means following certain rules:

**1. Don't access hardware directly (yet).** While UEFI Boot Services are active, we should use UEFI protocols to access hardware. If we write directly to hardware registers, we might conflict with UEFI's own drivers.

**2. Memory is managed by UEFI.** We ask UEFI for memory using `AllocatePool()` or `AllocatePages()`. We don't just pick random addresses and start writing.

**3. The firmware is still running.** Unlike a traditional OS that takes over the machine completely, when our UEFI application runs, the firmware is still active in the background. It's handling interrupts, managing the display, and providing services. We coexist with it.

**4. We can "exit boot services" later.** If we eventually want full bare-metal control, we can call `ExitBootServices()`. This tells the firmware "I'm taking over now" — it shuts down all UEFI services and gives us exclusive access to the hardware. We won't do this in Phase 1 because we need UEFI's keyboard and filesystem support.

## Key Takeaways

- Computers boot through a chain of increasingly capable programs
- UEFI is a standard interface between software and firmware
- Our application is a UEFI executable loaded from an SD card
- UEFI provides protocols for screen, keyboard, and filesystem access
- We're "bare metal" in the sense that there's no OS, but we use UEFI as a hardware abstraction layer
- The `EFI_SYSTEM_TABLE` is our gateway to everything the firmware provides

In the next chapter, we'll set up the tools we need to cross-compile UEFI applications from our development machine.
