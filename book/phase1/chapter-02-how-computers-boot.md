# Chapter 2: How Computers Boot

## The Cold Start Problem

When you press the power button on a computer, the CPU wakes up with no memory of anything. RAM is empty. There's no operating system loaded. The CPU doesn't even know what kind of storage devices are attached or how to read from them.

So how does anything happen?

The answer is a chain of increasingly capable programs, each one loading the next. Think of it like starting a fire: you can't light a log with a match. You light a match, use it to ignite kindling, use the kindling to start small sticks, and use the sticks to light the log. Each step bootstraps the next — and in fact, this is where the term **"booting"** comes from. It's short for "bootstrapping," from the old phrase "pulling yourself up by your bootstraps."

## The Boot Chain

On our Sweet Potato board, the boot chain looks like this:

```
Step 1: ROM Code (BL1)
  Built into the S905X chip. Cannot be changed.
  Knows how to read from SD card or SPI flash.
  Loads Step 2 into RAM.
          ↓
Step 2: Secondary Bootloader (BL2)
  Amlogic proprietary code.
  Initializes DDR RAM (the 2 GB of main memory).
  Without this, you only have a tiny amount of on-chip SRAM.
  Loads Step 3.
          ↓
Step 3: U-Boot (BL3)
  Open-source bootloader.
  Initializes USB, SD card, HDMI, etc.
  Provides UEFI services.
  Loads and runs our application.
          ↓
Step 4: Our Code (survival.efi)
  Uses UEFI services to access screen, keyboard, filesystem.
  This is what we're building.
```

Let's walk through each step.

### Step 1: The ROM Code

Every S905X chip has a small program burned into it at the factory. This program is stored in ROM (Read-Only Memory) — it literally cannot be modified. It's etched into the silicon.

This ROM code is extremely simple. It knows how to:
1. Initialize the CPU's basic clocks
2. Look at specific locations on the SD card (or SPI flash) for the next stage
3. Load a small amount of code into the chip's internal SRAM (a tiny, fast memory — only about 64 KB)
4. Jump to that code

That's it. The ROM code doesn't know about filesystems, HDMI, USB, or anything else. It just loads a blob of bytes from a fixed location and jumps to it.

### Step 2: DDR Initialization

The code that the ROM loads is called BL2 (Boot Loader stage 2). Its primary job is one of the most critical and complex operations in the entire boot process: **initializing DDR RAM**.

DDR (Double Data Rate) RAM is the 2 GB of main memory on our board. But here's the thing — you can't just start reading and writing to DDR. The memory controller needs to be configured with precise timing parameters: how fast the clock runs, how many cycles to wait between operations, what voltage to use. Get any of this wrong and the RAM produces garbage or doesn't respond at all.

This is why BL2 is proprietary (closed-source) Amlogic code. DDR initialization requires intimate knowledge of the specific memory chips on the board. Amlogic provides this as a binary blob.

After BL2 runs, we have access to the full 2 GB of RAM. Before BL2, we only had ~64 KB of SRAM to work with.

### Step 3: U-Boot

With RAM available, the next stage can be much more sophisticated. On the Sweet Potato, this is **U-Boot** — a popular open-source bootloader used on many ARM boards.

U-Boot is almost like a tiny operating system. It can:
- Read files from FAT32 filesystems on SD cards
- Display images on HDMI
- Accept keyboard input via USB
- Download files over the network
- Run scripts
- Load and execute other programs

Most importantly for us, U-Boot provides **UEFI services**. This is the interface we'll use.

### Step 4: Our Code

Our application is a UEFI executable. U-Boot loads it from the SD card and runs it. We'll explain what that means next.

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

The beauty of UEFI is that it's the same interface regardless of the underlying hardware. Whether the firmware is U-Boot on an ARM board, EDK2 on an Intel server, or something else entirely — if it implements UEFI, our code runs on it.

### UEFI vs. BIOS

If you've heard of BIOS, UEFI is its modern replacement. The old PC BIOS (Basic Input/Output System) dates back to 1981 and has severe limitations: 16-bit mode, 1 MB memory limit, no standard for graphics beyond text mode. UEFI was designed from scratch to support modern hardware: 64-bit processors, large displays, multiple storage devices, and more.

### Why UEFI and Not Full Bare-Metal?

We could write code that talks directly to the S905X hardware registers — configuring the HDMI transmitter, programming the USB controller, reading SD card sectors. This is called "full bare-metal" programming. We plan to do this eventually (see `docs/bare-metal-roadmap.md`), but there are good reasons to start with UEFI:

**1. Amlogic doesn't publish full documentation.** The S905X doesn't have a complete public datasheet. To program the HDMI output directly, you'd need to reverse-engineer the register interface from Linux kernel drivers. That's a fascinating project (and we've documented how to do it), but it could take months just for video output.

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

When the Sweet Potato boots, U-Boot looks for a UEFI application on the SD card. It follows a standard path convention:

```
SD Card (FAT32)
└── EFI/
    └── BOOT/
        └── BOOTAA64.EFI     ← Our application
```

The filename `BOOTAA64.EFI` is special. "BOOT" means "default boot application" and "AA64" means "AArch64" (64-bit ARM). This is defined by the UEFI specification — the firmware knows to look for this specific filename.

When U-Boot finds this file, it:
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

In the next chapter, we'll set up the tools we need to compile code for the Sweet Potato from our development machine.
