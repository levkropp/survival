---
layout: default
title: "Chapter 3: Setting Up the Workshop"
parent: "Phase 1: Boot & Input"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 3
---

# Chapter 3: Setting Up the Workshop

## The Cross-Compilation Problem

Here's a puzzle: we're writing code on a laptop that has an x86-64 processor, but our code needs to run on an ARM processor. These are completely different instruction sets. An x86 instruction like `48 89 c7` (move the RAX register to RDI) is meaningless gibberish to an ARM chip. And an ARM instruction like `AA0003E0` (move register X0 to X0) means nothing to an x86 chip.

We need a **cross-compiler** — a compiler that runs on one architecture (x86-64, our laptop) but produces code for a different architecture (AArch64, the Sweet Potato).

This is different from what you normally do when you type `gcc hello.c` on your laptop. That produces x86-64 code that runs on your laptop. We need to produce AArch64 code that runs on the Sweet Potato.

## The Tools We Need

Let's list every tool and explain what it does:

### 1. aarch64-linux-gnu-gcc

This is the cross-compiler. Despite having "linux" in its name (it was originally built to target Linux systems), it works fine for our purpose. The `-ffreestanding` flag tells it "I'm not running on any OS, don't assume anything about the environment."

The name breaks down as:
- `aarch64` — target architecture (64-bit ARM)
- `linux-gnu` — target OS convention (we override this with compiler flags)
- `gcc` — the GNU C Compiler

When we run this compiler, it reads our C source code and produces AArch64 machine code in an ELF (Executable and Linkable Format) object file.

### 2. aarch64-linux-gnu-ld

The **linker**. After the compiler produces individual object files (one per `.c` source file), the linker combines them into a single executable. It resolves references between files — if `main.c` calls a function defined in `fb.c`, the linker fills in the actual address of that function.

### 3. aarch64-linux-gnu-objcopy

The **object copy** tool. Our linker produces an ELF binary, but UEFI expects a PE (Portable Executable) binary. `objcopy` converts between formats. It extracts the important sections from the ELF file and packages them into a PE file that UEFI can load.

### 4. gnu-efi

A library that provides:
- **Header files** — C definitions for all the UEFI types, GUIDs, and structures (like `EFI_SYSTEM_TABLE`, `EFI_GRAPHICS_OUTPUT_PROTOCOL`, etc.)
- **CRT0 (C Runtime startup)** — A tiny assembly stub that UEFI calls first, which sets things up and then calls our `efi_main()` function
- **Relocation code** — Handles adjusting memory addresses when the binary is loaded at a different address than it was compiled for
- **Helper libraries** — Utility functions for working with UEFI protocols

### 5. qemu-system-aarch64

An emulator. QEMU can emulate an entire AArch64 system — CPU, RAM, UEFI firmware, display, keyboard, and storage. This lets us test our code without touching real hardware. Invaluable during development.

### 6. qemu-efi-aarch64

UEFI firmware for QEMU. This is a pre-built EDK2 firmware that runs inside QEMU and provides the same UEFI services that U-Boot provides on real hardware.

### 7. mtools

Utilities for working with FAT filesystems without mounting them. We use `mcopy` to put our `.efi` file onto a FAT32 disk image without needing root permissions.

## Installing the Toolchain

The `scripts/setup-toolchain.sh` script installs everything. Let's walk through it:

```bash
#!/bin/bash
set -e
```

`set -e` tells bash to exit immediately if any command fails. Without this, a failed `apt-get install` would be silently ignored and we'd get confusing errors later.

```bash
if command -v apt-get &>/dev/null; then
    sudo apt-get update
    sudo apt-get install -y \
        gcc-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        gnu-efi \
        qemu-system-arm \
        qemu-efi-aarch64 \
        mtools
```

On Debian/Ubuntu systems, these are the package names. The script also supports Arch Linux (`pacman`) and Fedora (`dnf`).

### The gnu-efi Problem

Here's something we discovered the hard way: on x86-64 systems, the `gnu-efi` package only includes x86 targets. It has `crt0-efi-x86_64.o` and `elf_x86_64_efi.lds` but **not** the AArch64 equivalents. We need `crt0-efi-aarch64.o` and `elf_aarch64_efi.lds`.

The solution: build gnu-efi from source for AArch64.

```bash
git clone https://github.com/ncroxon/gnu-efi.git
cd gnu-efi
make ARCH=aarch64 CROSS_COMPILE=aarch64-linux-gnu-
```

This cross-compiles gnu-efi itself — using our AArch64 cross-compiler to build the library files we need. We then copy the results into our project at `tools/gnu-efi/`:

```
tools/gnu-efi/
├── include/           # UEFI header files
│   ├── efi.h          # Main UEFI definitions
│   ├── efilib.h       # Helper library
│   ├── eficon.h       # Console protocols
│   ├── aarch64/       # AArch64-specific definitions
│   │   └── efibind.h  # Type sizes, calling conventions
│   └── ...
└── lib/               # Compiled libraries
    ├── crt0-efi-aarch64.o    # Startup code
    ├── elf_aarch64_efi.lds   # Linker script
    ├── libefi.a              # UEFI helper functions
    └── libgnuefi.a           # gnu-efi support functions
```

## The Build Process

Let's trace what happens when you type `make`. Understanding the build process demystifies a lot of the toolchain.

### Step 1: Compile Each Source File

```
src/main.c  ──→  build/main.o
src/fb.c    ──→  build/fb.o
src/kbd.c   ──→  build/kbd.o
src/mem.c   ──→  build/mem.o
src/font.c  ──→  build/font.o
```

Each `.c` file is compiled independently into a `.o` (object) file. The compiler command looks like this:

```bash
aarch64-linux-gnu-gcc \
    -ffreestanding          # No OS — don't use standard library
    -fno-stack-protector    # No stack canaries (no OS to handle the signal)
    -fno-stack-check        # No stack checking
    -fshort-wchar           # wchar_t is 16-bit (UEFI uses UTF-16 strings)
    -mstrict-align          # Don't do unaligned memory access
    -fPIC                   # Position Independent Code
    -fPIE                   # Position Independent Executable
    -fno-strict-aliasing    # Allow type-punning through pointers
    -fno-merge-all-constants # Don't merge identical constants
    -Itools/gnu-efi/include        # Where to find UEFI headers
    -Itools/gnu-efi/include/aarch64 # Architecture-specific headers
    -Isrc                          # Our own headers
    -Wall -Wextra           # Enable all warnings
    -O2                     # Optimize for speed
    -c                      # Compile only, don't link
    -o build/main.o         # Output file
    src/main.c              # Input file
```

Let's explain the important flags:

**`-ffreestanding`** is crucial. It tells GCC: "This code doesn't have access to a standard C library. Don't assume `printf`, `malloc`, `memcpy`, or any other standard function exists. Don't insert calls to them behind my back." Without this flag, GCC might secretly insert a call to `memcpy` when it sees a struct assignment, which would fail because we don't have `memcpy`.

**`-fshort-wchar`** makes `wchar_t` a 16-bit type. UEFI uses UTF-16 encoded strings (where each character is at least 16 bits), so we need `wchar_t` to match. On Linux, `wchar_t` is normally 32 bits.

**`-fPIC` and `-fPIE`** generate position-independent code. This means the code doesn't assume it will be loaded at any particular memory address. This is essential because UEFI can load our binary anywhere in memory.

**`-fno-merge-all-constants`** prevents the compiler from combining identical constant values. This can cause problems with the relocation process in UEFI applications.

### Step 2: Link Everything Together

```bash
aarch64-linux-gnu-ld \
    -nostdlib               # Don't link standard libraries
    -Bsymbolic              # Bind symbols locally
    -pie                    # Position Independent Executable
    --no-dynamic-linker     # No dynamic linker needed
    -z common-page-size=4096 -z max-page-size=4096  # 4 KB pages
    -z norelro              # No read-only relocations
    -z nocombreloc          # Don't combine relocations
    -T tools/gnu-efi/lib/elf_aarch64_efi.lds   # Linker script
    -Ltools/gnu-efi/lib     # Library search path
    tools/gnu-efi/lib/crt0-efi-aarch64.o       # CRT startup (FIRST)
    build/main.o build/fb.o build/kbd.o ...      # Our object files
    -o build/survival.so                         # Output
    -lefi -lgnuefi          # UEFI libraries
    /usr/lib/.../libgcc.a   # GCC runtime support
```

A few critical details:

**The CRT comes first.** `crt0-efi-aarch64.o` must be the first object file. It contains the `_start` function — the true entry point that UEFI calls. This function handles relocations and then calls our `efi_main()`.

**Link order matters.** `-lefi -lgnuefi` come after our object files. The linker processes files left to right, so libraries must come after the code that references them.

**`libgcc.a` provides compiler builtins.** GCC sometimes generates calls to helper functions for operations the CPU doesn't directly support (like 128-bit division). `libgcc.a` provides these.

**The linker script** (`elf_aarch64_efi.lds`) tells the linker how to arrange the output file. It defines which sections go where and at what addresses. Sections include:
- `.text` — executable code
- `.rodata` — read-only data (string literals, constant arrays)
- `.data` — read-write data (global variables)
- `.dynamic` — dynamic linking information
- `.rela` — relocation entries

### Step 3: Convert ELF to PE

```bash
aarch64-linux-gnu-objcopy \
    -j .text                # Include code section
    -j .sdata               # Include small data
    -j .data                # Include data section
    -j .rodata              # Include read-only data  ← CRITICAL!
    -j .dynamic             # Include dynamic info
    -j .dynsym              # Include dynamic symbols
    -j .rel                 # Include relocations
    -j .rela                # Include relocations (with addend)
    -j .reloc               # Include PE relocations
    --target=efi-app-aarch64 # Output as UEFI application
    build/survival.so        # Input (ELF)
    build/survival.efi       # Output (PE/COFF)
```

This converts our ELF shared object into a PE executable that UEFI can load. The `-j` flags specify which sections to include.

**A lesson learned the hard way:** if you forget `-j .rodata`, your binary will compile and link without errors, but it will crash immediately at boot with a "Synchronous Exception." This is because `.rodata` contains all string literals (like `L"Hello\r\n"`) and constant data (like our font bitmap). Without it, any attempt to access a string or constant reads from unmapped memory and faults.

### Step 4: Copy to ESP

```bash
mkdir -p build/esp/EFI/BOOT
cp build/survival.efi build/esp/EFI/BOOT/BOOTAA64.EFI
```

We copy our binary to the EFI System Partition (ESP) directory structure. This is where UEFI firmware looks for boot applications.

## The Makefile

Let's look at our Makefile piece by piece. If you haven't used Makefiles before, they're a way to automate build processes. Each "rule" says: "to build *this target*, you need *these prerequisites*, and you run *these commands*."

```makefile
CROSS    := aarch64-linux-gnu-
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy
```

We define the cross-compilation prefix once, then use it to construct tool names. This makes it easy to switch toolchains.

```makefile
EFI_INC  := tools/gnu-efi/include
EFI_LIB  := tools/gnu-efi/lib
EFI_CRT  := $(EFI_LIB)/crt0-efi-aarch64.o
EFI_LDS  := $(EFI_LIB)/elf_aarch64_efi.lds
```

Paths to our locally-built gnu-efi files.

```makefile
SOURCES  := $(SRCDIR)/main.c $(SRCDIR)/fb.c $(SRCDIR)/kbd.c \
            $(SRCDIR)/mem.c $(SRCDIR)/font.c
OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
```

`SOURCES` lists our source files. `OBJECTS` transforms each `.c` path into a `.o` path using `patsubst` (pattern substitution): `src/main.c` becomes `build/main.o`.

```makefile
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<
```

The `%` is a wildcard. This rule says: "to build `build/anything.o`, you need `src/anything.c`." The `$@` is the target (output) and `$<` is the first prerequisite (input). The `@` before `mkdir` suppresses echoing the command.

```makefile
$(SO): $(OBJECTS)
	$(LD) $(LDFLAGS) -L$(EFI_LIB) $(EFI_CRT) $(OBJECTS) -o $@ \
	    -lefi -lgnuefi $(LIBGCC)
```

Link all objects plus the CRT and libraries into a shared object.

```makefile
$(TARGET): $(SO)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata ... \
	    --target=efi-app-aarch64 $< $@
```

Convert to PE format.

## Testing with QEMU

QEMU lets us test without hardware. Our `scripts/run-qemu.sh` script supports three modes:

### Graphical Mode (default)

```bash
./scripts/run-qemu.sh graphical
```

Opens a GTK window showing the actual framebuffer output. Uses the `ramfb` device, which provides a real linear framebuffer — memory-mapped pixels, just like real hardware.

### Console Mode

```bash
./scripts/run-qemu.sh console
```

No display window. All output goes to your terminal as serial text. Uses `virtio-gpu-pci` which provides GOP but no linear framebuffer, so our code falls back to console mode. Good for automated testing.

### VNC Mode

```bash
./scripts/run-qemu.sh vnc
```

Like graphical mode, but displays via VNC on port 5900. Useful for headless servers or remote development.

The QEMU command constructs a virtual machine:

```
-M virt              Virtual ARM machine (not emulating specific hardware)
-cpu cortex-a53      Same CPU as the Sweet Potato
-m 256M              256 MB RAM (plenty for testing)
-drive if=pflash...  UEFI firmware
-hda disk.img        Our SD card (FAT32 with BOOTAA64.EFI)
-device ramfb        Framebuffer display device
-device qemu-xhci    USB controller
-device usb-kbd      USB keyboard
-serial stdio        Serial port to terminal
```

## The File Layout

Here's what our project looks like now:

```
survival/
├── Makefile                         # Build system
├── README.md                        # Project overview
├── book/                            # This book
│   ├── chapter-01-the-mission.md
│   ├── chapter-02-how-computers-boot.md
│   └── chapter-03-setting-up-the-workshop.md
├── docs/
│   └── bare-metal-roadmap.md        # Future bare-metal research
├── scripts/
│   ├── setup-toolchain.sh           # Install build tools
│   └── run-qemu.sh                  # Test in emulator
├── src/
│   ├── boot.h                       # Global state definitions
│   ├── main.c                       # Entry point and main loop
│   ├── fb.c / fb.h                  # Framebuffer driver
│   ├── font.c / font.h              # Bitmap font data
│   ├── kbd.c / kbd.h                # Keyboard input
│   └── mem.c / mem.h                # Memory allocator
├── tools/
│   └── gnu-efi/                     # Cross-compiled gnu-efi
│       ├── include/                 # UEFI headers
│       └── lib-aarch64/             # ARM64 CRT, linker script, libraries
│       └── lib-x86_64/             # x86_64 CRT, linker script, libraries
└── build/                           # Generated (not in source control)
    ├── aarch64/survival.efi         # ARM64 binary
    ├── x86_64/survival.efi          # x86_64 binary
    └── ...
```

## The Second Architecture: x86_64

The workstation targets an ARM64 single-board computer. But UEFI abstracts the hardware. A UEFI application that talks to GOP, SimpleTextInput, and SimpleFileSystem can run on any machine with UEFI firmware, regardless of CPU architecture. Adding x86_64 means the workstation runs on essentially any modern PC — your laptop, a spare desktop, anything that boots UEFI.

It also means we can develop and test on the host machine without cross-compilation overhead. QEMU with OVMF (an x86_64 UEFI firmware) runs at near-native speed on an x86_64 Linux box.

### The Dual-Architecture Makefile

The build system needs to produce two different binaries from the same source. The approach: a single `ARCH` variable that selects everything architecture-specific.

At the top of the Makefile:

```makefile
ARCH     ?= aarch64
```

Then a conditional block that sets all arch-dependent variables:

```makefile
ifeq ($(ARCH),aarch64)
  CROSS       := aarch64-linux-gnu-
  EFI_ARCH    := aarch64
  EFI_BOOT    := BOOTAA64.EFI
  OBJCOPY_TGT := efi-app-aarch64
  TCC_TARGET  := -DTCC_TARGET_ARM64=1
  ARCH_CFLAGS := -mstrict-align
  SETJMP_SRC  := $(SRCDIR)/setjmp_aarch64.S
else ifeq ($(ARCH),x86_64)
  CROSS       :=
  EFI_ARCH    := x86_64
  EFI_BOOT    := BOOTX64.EFI
  OBJCOPY_TGT := efi-app-x86_64
  TCC_TARGET  := -DTCC_TARGET_X86_64=1
  ARCH_CFLAGS := -mno-red-zone -DGNU_EFI_USE_MS_ABI
  SETJMP_SRC  := $(SRCDIR)/setjmp_x86_64.S
else
  $(error Unsupported ARCH=$(ARCH). Use aarch64 or x86_64)
endif
```

Every variable that touches the architecture flows from this block:

- **`CROSS`** — the cross-compiler prefix. ARM64 needs `aarch64-linux-gnu-gcc`. x86_64 uses the native compiler (empty prefix).
- **`EFI_BOOT`** — the filename UEFI firmware looks for. ARM64 boots from `BOOTAA64.EFI`, x86_64 from `BOOTX64.EFI`.
- **`OBJCOPY_TGT`** — the PE/COFF target format for `objcopy`.
- **`TCC_TARGET`** — passed to TCC's unity build so it generates code for the right architecture.
- **`ARCH_CFLAGS`** — architecture-specific compiler flags.
- **`SETJMP_SRC`** — which assembly file provides `setjmp`/`longjmp`.

These plug into the rest of the Makefile via `$(ARCH_CFLAGS)` in the CFLAGS and TCC_CFLAGS definitions, and `$(SETJMP_SRC)` in the assembly build rule:

```makefile
$(BUILDDIR)/setjmp.o: $(SETJMP_SRC)
	@mkdir -p $(BUILDDIR)
	$(CC) -c -o $@ $<
```

Build outputs go to `build/$(ARCH)/`, so both architectures can coexist:

```makefile
BUILDDIR := build/$(ARCH)
```

Building both is one command:

```makefile
all-arches:
	$(MAKE) ARCH=aarch64
	$(MAKE) ARCH=x86_64
```

### x86_64 Compiler Flags

Two flags in `ARCH_CFLAGS` deserve explanation.

**The Red Zone.** The System V AMD64 ABI defines a "red zone" — 128 bytes below the stack pointer that functions can use without adjusting `rsp`. Leaf functions can skip the `sub rsp, N` / `add rsp, N` dance and just use the space below `rsp` directly. GCC exploits this aggressively.

The problem: interrupts and exceptions also use the stack. When a UEFI timer interrupt fires, the firmware pushes state onto the stack — right on top of the red zone data. The function resumes and its local variables are corrupt. `-mno-red-zone` tells GCC to never assume the space below `rsp` is safe. Linux kernels disable it too.

**The MS ABI.** UEFI was designed by Intel and uses Microsoft's calling convention, even on non-Windows systems. The key difference from System V: integer arguments go in rcx, rdx, r8, r9 (not rdi, rsi, rdx, rcx), and the caller must reserve 32 bytes of "shadow space" above the return address. gnu-efi handles this transparently when `GNU_EFI_USE_MS_ABI` is defined — it wraps every UEFI call with `__attribute__((ms_abi))` to switch calling conventions. Our own functions use the normal System V ABI.

On ARM64, there's no ABI split — UEFI uses the standard AAPCS64 calling convention. This is one way ARM64 UEFI is simpler than x86_64.

### gnu-efi for x86_64

Our local gnu-efi build at `tools/gnu-efi/` needs libraries for both architectures. The `lib-aarch64/` directory was built from source (the distro package only has x86 targets). For x86_64, the distro package works:

```bash
sudo apt install gnu-efi
mkdir -p tools/gnu-efi/lib-x86_64
cp /usr/lib/crt0-efi-x86_64.o tools/gnu-efi/lib-x86_64/
cp /usr/lib/elf_x86_64_efi.lds tools/gnu-efi/lib-x86_64/
cp /usr/lib/libefi.a tools/gnu-efi/lib-x86_64/
cp /usr/lib/libgnuefi.a tools/gnu-efi/lib-x86_64/
```

The include files are shared — ARM64 and x86_64 use the same gnu-efi headers (with arch-specific subdirectories for a few definitions).

### QEMU for x86_64

ARM64 QEMU uses `QEMU_EFI.fd` as firmware. x86_64 uses OVMF — Intel's open-source UEFI firmware for virtual machines:

```bash
sudo apt install ovmf
```

This provides `/usr/share/OVMF/OVMF_CODE_4M.fd` (firmware code) and `/usr/share/OVMF/OVMF_VARS_4M.fd` (NVRAM template).

Create `scripts/run-qemu-x86_64.sh`:

```bash
#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build/x86_64"
ESP_DIR="$BUILD_DIR/esp"

# OVMF firmware
FW_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
FW_VARS="/tmp/survival_ovmf_vars.fd"

# CRITICAL: copy the VARS template, do NOT create an empty file.
# OVMF_VARS contains pre-initialized NVRAM data. A zero-filled
# file causes an Invalid Opcode exception at boot.
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$FW_VARS"

# Create disk image
IMG="$BUILD_DIR/disk.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
mcopy -i "$IMG" -s "$ESP_DIR"/* ::/

qemu-system-x86_64 \
    -m 256M \
    -drive if=pflash,format=raw,file="$FW_CODE",readonly=on \
    -drive if=pflash,format=raw,file="$FW_VARS" \
    -hda "$IMG" \
    -device ramfb \
    -device qemu-xhci -device usb-kbd \
    -serial stdio
```

The OVMF_VARS trap deserves emphasis. The VARS file is a NVRAM template containing pre-initialized firmware variables. If you create it as a zero-filled file (like `dd if=/dev/zero`), OVMF tries to parse garbage NVRAM data and crashes with an Invalid Opcode exception before your application loads. Always copy from the template.

### setjmp for x86_64

ARM64 needed to save 22 registers (Chapter 13). x86_64 is simpler — the System V ABI has only 6 callee-saved general-purpose registers plus the stack pointer and return address:

```
rbx, rbp, r12, r13, r14, r15, rsp, return address = 8 slots
```

Create `src/setjmp_x86_64.S`:

```asm
    .text
    .align 16

    .global setjmp
    .type   setjmp, @function
setjmp:
    mov  %rbx,    (%rdi)
    mov  %rbp,   8(%rdi)
    mov  %r12,  16(%rdi)
    mov  %r13,  24(%rdi)
    mov  %r14,  32(%rdi)
    mov  %r15,  40(%rdi)
    lea  8(%rsp), %rax       /* caller's rsp (before call pushed rip) */
    mov  %rax,  48(%rdi)
    mov  (%rsp), %rax        /* return address */
    mov  %rax,  56(%rdi)
    xor  %eax, %eax          /* return 0 */
    ret

    .global longjmp
    .type   longjmp, @function
longjmp:
    mov  %esi, %eax          /* return value */
    test %eax, %eax
    jnz  1f
    inc  %eax                /* longjmp(buf, 0) returns 1 */
1:
    mov    (%rdi), %rbx
    mov   8(%rdi), %rbp
    mov  16(%rdi), %r12
    mov  24(%rdi), %r13
    mov  32(%rdi), %r14
    mov  40(%rdi), %r15
    mov  48(%rdi), %rsp
    jmp *56(%rdi)            /* jump to saved return address */
```

64 bytes of state versus ARM64's 176 bytes. x86_64's SSE registers (xmm0-xmm15) are all caller-saved, so `setjmp` doesn't touch them.

The `jmp_buf` typedef in `shim.h` is architecture-conditional:

```c
#ifdef __aarch64__
typedef long jmp_buf[22];
#else
typedef long jmp_buf[8];
#endif
```

### Conditional Code in C

Most of our C code is architecture-neutral — UEFI abstracts the hardware. But a few places need `#ifdef`:

```c
/* F6 rebuild: arch-specific output path */
#ifdef __aarch64__
    const char *out_path = "/EFI/BOOT/BOOTAA64.EFI";
#else
    const char *out_path = "/EFI/BOOT/BOOTX64.EFI";
#endif
```

These are rare. The UEFI protocols — GOP, SimpleTextInput, SimpleFileSystem — work identically on both architectures. A pixel on x86_64 is still a 32-bit BGRX value in a linear framebuffer. A keypress still arrives through `ReadKeyStroke`. A file still opens through `SimpleFileSystem`. That's the beauty of UEFI as a platform abstraction.

### Building and Testing Both Architectures

```bash
# ARM64 (default)
make
./scripts/run-qemu.sh graphical

# x86_64
make ARCH=x86_64
./scripts/run-qemu-x86_64.sh

# Both architectures
make all-arches
```

## Key Takeaways

- Cross-compilation lets us build ARM code on an x86 machine
- The build process is: compile → link → convert ELF to PE
- **`-ffreestanding`** is the most important flag — it tells GCC we have no OS
- **`.rodata` must be included** in the objcopy step or the binary will crash
- gnu-efi provides UEFI headers, CRT startup code, and helper libraries
- The CRT must be linked first — it contains the true entry point
- QEMU with `ramfb` provides a real framebuffer for testing
- The same Makefile builds for ARM64 and x86_64 — one `ARCH` variable controls everything
- x86_64 UEFI requires `-mno-red-zone` and MS ABI calling convention wrappers

Now that we have our tools ready, let's write some actual code.
