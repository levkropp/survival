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
│       └── lib/                     # CRT, linker script, libraries
└── build/                           # Generated (not in source control)
    ├── survival.efi                 # The final binary (64 KB)
    └── esp/EFI/BOOT/BOOTAA64.EFI   # Copy for SD card
```

## Key Takeaways

- Cross-compilation lets us build ARM code on an x86 machine
- The build process is: compile → link → convert ELF to PE
- **`-ffreestanding`** is the most important flag — it tells GCC we have no OS
- **`.rodata` must be included** in the objcopy step or the binary will crash
- gnu-efi provides UEFI headers, CRT startup code, and helper libraries
- The CRT must be linked first — it contains the true entry point
- QEMU with `ramfb` provides a real framebuffer for testing

Now that we have our tools ready, let's write some actual code.
