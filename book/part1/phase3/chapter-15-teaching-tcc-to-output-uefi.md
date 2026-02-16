---
layout: default
title: "Chapter 15: Teaching TCC to Output UEFI"
parent: "Phase 3: The C Compiler"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 3
---

# Chapter 15: Teaching TCC to Output UEFI

## The Problem

In Chapter 14, TCC compiles C code to machine instructions in memory. The user presses F5, TCC generates ARM64 or x86_64 code into an allocated buffer, and we call it through a function pointer. That's `TCC_OUTPUT_MEMORY` — JIT compilation.

But JIT compilation doesn't help us rebuild the workstation itself. The workstation is a PE/COFF binary on disk — `BOOTAA64.EFI` or `BOOTX64.EFI`. To make the workstation self-hosting, TCC needs to output PE/COFF files, not just in-memory code.

TCC already knows how to produce PE files. It's how TCC works on Windows — `tccpe.c` handles PE/COFF output for Windows executables and DLLs. But TCC's PE support is gated behind `#ifdef TCC_TARGET_PE`, which is only defined when building TCC as a Windows compiler. Our TCC targets ARM64 or x86_64 on UEFI — not Windows.

UEFI uses PE/COFF too. Same file format. But TCC doesn't know that.

In Chapter 13, we didn't modify TCC's source code. That's about to change. To make self-hosting work, we need to patch TCC in three areas: wide character handling, PE output routing, and ARM64 linker relaxation.

## Wide Characters: TCC_WCHAR_16

UEFI loves UTF-16. Every string the firmware handles — file paths, console output, protocol names — uses 16-bit `CHAR16` characters. In C, wide string literals like `L"Hello"` produce `wchar_t` arrays.

The problem: on Linux, `wchar_t` is 32 bits. TCC follows this convention for non-Windows targets. When our UEFI code writes:

```c
con_print(L"\r\n");
```

TCC generates a 32-bit-per-character array. But `con_print` expects `CHAR16 *` — 16-bit characters. The firmware reads the first 16 bits, sees the character, then reads the next 16 bits expecting the next character but gets the upper half of the first 32-bit value (zero). The string appears truncated or garbled.

On Windows, TCC uses 16-bit `wchar_t` because that's what the Windows API expects. The logic is gated behind `#ifdef TCC_TARGET_PE`. We need the same behavior without pulling in all of Windows PE support.

The fix: a new define, `TCC_WCHAR_16`, that enables 16-bit wide characters independently of the target platform.

In `tools/tinycc/config.h`, add:

```c
/* UEFI uses UTF-16 -- make wchar_t / L"..." 16-bit like PE */
#define TCC_WCHAR_16 1
```

Then widen the guards in three files. In `tcc.h`, the internal `nwchar_t` type:

```c
#if defined(TCC_TARGET_PE) || defined(TCC_WCHAR_16)
typedef unsigned short nwchar_t;
#else
typedef int nwchar_t;
#endif
```

In `tccgen.c`, where TCC decides the type of wide string literals (two locations):

```c
#if defined(TCC_TARGET_PE) || defined(TCC_WCHAR_16)
    /* L"..." produces unsigned short[] */
#else
    /* L"..." produces int[] */
#endif
```

In `tccpp.c`, where TCC encodes wide characters:

```c
#if defined(TCC_TARGET_PE) || defined(TCC_WCHAR_16)
    /* encode as UTF-16 */
#else
    /* encode as UTF-32 */
#endif
```

And in `tccdefs_.h` (the generated predefined macros), the `__WCHAR_TYPE__` macro must reflect the actual size:

```c
#if defined(TCC_TARGET_PE) || defined(TCC_WCHAR_16)
    "__WCHAR_TYPE__\0" "unsigned short\0"
#else
    "__WCHAR_TYPE__\0" "int\0"
#endif
```

After these changes, `L"Hello"` produces 16-bit UTF-16 arrays on our UEFI target, matching what the firmware expects.

## Routing PE Output for ARM64 and x86_64

TCC's output pipeline has a fork. In `tccelf.c`, the `tcc_output_file` function checks the target:

```c
#ifdef TCC_TARGET_PE
    return pe_output_file(s, filename);
#else
    return elf_output_file(s, filename);
#endif
```

Our ARM64/x86_64 UEFI builds need PE output, but `TCC_TARGET_PE` isn't defined. The fix: widen the guard:

```c
#if defined(TCC_TARGET_PE) || defined(TCC_TARGET_ARM64) || defined(TCC_TARGET_X86_64)
    return pe_output_file(s, filename);
#else
    return elf_output_file(s, filename);
#endif
```

This alone causes a cascade of compilation errors. `pe_output_file` is defined in `tccpe.c`, which is only included when `TCC_TARGET_PE` is defined. The PE-related struct fields in `tcc.h` — `pe_subsystem`, `pe_imagebase`, PE function declarations — are similarly gated. We need to widen all these guards.

In `libtcc.c`, include `tccpe.c` for ARM64/x86_64:

```c
#if defined(TCC_TARGET_PE) || defined(TCC_TARGET_ARM64) || defined(TCC_TARGET_X86_64)
# include "tccpe.c"
#endif
```

In `tcc.h`, make PE struct fields available:

```c
#if defined(TCC_TARGET_PE) || defined(TCC_TARGET_ARM64) || defined(TCC_TARGET_X86_64)
    int pe_subsystem;
    unsigned pe_characteristics;
    unsigned pe_file_align;
    addr_t pe_imagebase;
    /* ... other PE fields ... */
#endif
```

And the PE function declarations and linker option parsing — same pattern. Each `#ifdef TCC_TARGET_PE` that touches PE output or PE options gets widened to include ARM64 and x86_64.

## Guarding Windows-Specific Code

Including `tccpe.c` for non-Windows builds pulls in code that assumes a Windows host. Several spots need guards.

`tccpe.c` includes `<sys/stat.h>` for `chmod()` — unavailable on UEFI:

```c
#ifndef _WIN32
# ifndef __UEFI__
#  include <sys/stat.h>
# endif
#endif
```

The PDB debug symbol generator calls `system()` to invoke an external tool — only meaningful on Windows:

```c
#ifdef TCC_TARGET_PE
static int pe_create_pdb(TCCState *s1, const char *exefn) {
    /* ... calls system() ... */
}
#endif
```

`LoadLibraryA` and `GetProcAddress` for runtime DLL loading — native Windows only:

```c
#if defined(TCC_IS_NATIVE) && defined(_WIN32)
    /* dynamic library loading */
#endif
```

The `chmod()` call after writing the PE file:

```c
#if !defined(_WIN32) && !defined(__UEFI__)
    chmod(filename, 0755);
#endif
```

We also add `-D__UEFI__` to `TCC_CFLAGS` in the Makefile so these guards work:

```makefile
TCC_CFLAGS := ... -D__UEFI__ ...
```

## The ARM64 GOT Problem

Here's where it gets interesting. We widen the PE guards, fix the Windows-specific code, build successfully. Then we try to compile the workstation with TCC (the F6 rebuild). TCC compiles all source files, starts linking... and crashes.

The crash is in `arm64-link.c`, line 283:

```c
case R_AARCH64_ADR_GOT_PAGE: {
    uint64_t off =
        (((s1->got->sh_addr +    /* <-- CRASH: s1->got is NULL */
```

TCC's ARM64 code generator always produces **GOT-indirect** code for accessing global variables. GOT stands for Global Offset Table — a standard mechanism in ELF shared libraries where global addresses are stored in a table, and code loads addresses through the table rather than using direct references. This is how position-independent code (PIC) works on Linux.

For every global variable or function pointer access, TCC's ARM64 backend emits:

```asm
adrp x0, :got:symbol     ; load page of GOT entry
ldr  x0, [x0, :got_lo12:symbol]  ; load address from GOT
```

Two instructions, two relocations: `R_AARCH64_ADR_GOT_PAGE` and `R_AARCH64_LD64_GOT_LO12_NC`.

The ELF linker creates the GOT section and fills it with addresses. But PE output doesn't have a GOT. There's no such concept in PE/COFF — Windows uses import tables for DLL functions and direct references for everything else. When the PE linker encounters these GOT relocations, `s1->got` is NULL. Dereferencing it crashes.

This is a fundamental mismatch between TCC's code generator (which always emits GOT-indirect code on ARM64) and the PE linker (which has no GOT).

## GOT Relaxation

The solution is **linker relaxation** — a technique used by GNU's linker (`ld`) where GOT-indirect references are converted to direct references when the symbol is defined in the same binary. The same idea applies here.

The GOT-indirect pattern:

```asm
adrp x0, :got:symbol        ; page of GOT entry
ldr  x0, [x0, :got_lo12:symbol]  ; load address from GOT
```

The direct pattern:

```asm
adrp x0, symbol             ; page of symbol itself
add  x0, x0, :lo12:symbol   ; add page offset
```

Both compute the address of `symbol`. The first loads it indirectly through a table. The second computes it directly. The ADRP instruction is the same — only the target changes (GOT entry vs. symbol). The second instruction changes from LDR (load from memory) to ADD (compute offset).

In `arm64-link.c`, patch the two relocation handlers:

```c
case R_AARCH64_ADR_GOT_PAGE: {
    uint64_t off;
    if (s1->got) {
        off = (((s1->got->sh_addr +
               get_sym_attr(s1, sym_index, 0)->got_offset) >> 12) - (addr >> 12));
    } else {
        /* PE mode: no GOT, relax to direct ADRP */
        off = (val >> 12) - (addr >> 12);
    }
    if ((off + ((uint64_t)1 << 20)) >> 21)
        tcc_error_noabort("R_AARCH64_ADR_GOT_PAGE relocation failed");
    write32le(ptr, ((read32le(ptr) & 0x9f00001f) |
                    (off & 0x1ffffc) << 3 | (off & 3) << 29));
    return;
}
case R_AARCH64_LD64_GOT_LO12_NC:
    if (s1->got) {
        write32le(ptr,
                  ((read32le(ptr) & 0xfff803ff) |
                   ((s1->got->sh_addr +
                     get_sym_attr(s1, sym_index, 0)->got_offset) & 0xff8) << 7));
    } else {
        /* PE mode: no GOT, relax LDR to ADD with direct offset */
        uint32_t insn = read32le(ptr);
        uint32_t rd = insn & 0x1f;
        uint32_t rn = (insn >> 5) & 0x1f;
        write32le(ptr, 0x91000000 | rd | (rn << 5) |
                  ((val & 0xfff) << 10));
    }
    return;
```

When `s1->got` exists (ELF output), the original GOT-based logic runs. When it's NULL (PE output), the relaxation kicks in:

For `R_AARCH64_ADR_GOT_PAGE`: instead of computing the page of the GOT entry, compute the page of the symbol directly. The ADRP instruction encoding is identical — only the offset value changes.

For `R_AARCH64_LD64_GOT_LO12_NC`: the original instruction is `ldr xN, [xN, #offset]` which loads an 8-byte value from the GOT. We replace it with `add xN, xN, #offset` which adds the low 12 bits of the symbol address. The instruction encoding changes from LDR (opcode `0xF940xxxx`) to ADD (opcode `0x9100xxxx`). We extract the register numbers from the existing instruction and build a new ADD instruction.

This is the deepest patch we make to TCC. It bridges two fundamentally different worlds: ELF's GOT-based position-independent code model and PE's direct-addressing code model. Without it, TCC cannot generate ARM64 PE binaries that access global variables, function pointers, or string literals.

(On x86_64, TCC's code generator is more flexible and doesn't always use GOT-indirect addressing, so this problem doesn't arise as severely.)

## Missing Headers and Stubs

With the PE output working, TCC can link. But the self-hosting rebuild (compiling the workstation's own source code with TCC) reveals more gaps. The workstation source itself — and TCC's own internal headers — have dependencies that our stub header directory must satisfy.

### UEFI Type Stubs: efi.h and efilib.h

The first gap is the most fundamental. Every workstation source file includes `boot.h`, and `boot.h` starts with:

```c
#include <efi.h>
#include <efilib.h>
```

When GCC cross-compiles the workstation, those headers come from gnu-efi — a large, mature library with thousands of lines of type definitions, macros, inline functions, and gcc-specific constructs like `__attribute__((ms_abi))`. TCC can't parse gnu-efi's headers. They're too complex, use compiler extensions TCC doesn't support, and pull in dozens of sub-headers.

But our code doesn't use most of what gnu-efi provides. We need the UEFI types that our code actually references — `EFI_STATUS`, `EFI_HANDLE`, `EFI_SYSTEM_TABLE`, the protocol structs for GOP and filesystem and Block IO — and nothing else.

The solution: stub headers in `src/tcc-headers/` that TCC's `-I` path finds before it would look for the real ones.

**efilib.h is trivial** — a one-line include guard wrapper around `efi.h`. The workstation doesn't use any of efilib's utility functions (`Print`, `StrCpy`, `LibLocateHandle`, etc.). We call UEFI services directly through the system table.

```c
/* efilib.h — Minimal stub for TCC self-hosting */
#ifndef _TCC_EFILIB_STUB_H
#define _TCC_EFILIB_STUB_H
#include "efi.h"
#endif
```

**efi.h is roughly 400 lines** — substantial, but a fraction of the real gnu-efi headers. It's organized in layers:

First, the **qualifier macros** that appear throughout UEFI headers: `IN`, `OUT`, `OPTIONAL`, `EFIAPI`. All defined as empty — they're documentation annotations in the UEFI spec, not functional keywords.

Then the **base types**: `UINT8` through `UINT64`, `UINTN` (pointer-width unsigned), `CHAR16` (the ubiquitous UTF-16 character type), `EFI_STATUS`, `EFI_HANDLE`, `VOID`. These are typedefs to standard C types — `unsigned long` for `UINTN` on LP64, `unsigned short` for `CHAR16`.

**Status codes** come next: `EFI_SUCCESS`, `EFI_NOT_FOUND`, `EFI_OUT_OF_RESOURCES`, and the `EFIERR()` macro that sets the high bit to indicate an error. Then **EFI_GUID** — the 128-bit structure that UEFI uses to identify everything.

After the basics, the **protocol structs**. This is where it gets interesting. The stub defines `SIMPLE_INPUT_INTERFACE`, `SIMPLE_TEXT_OUTPUT_INTERFACE`, `EFI_BOOT_SERVICES`, `EFI_RUNTIME_SERVICES`, and `EFI_SYSTEM_TABLE` — the core system table hierarchy. Then the protocol-specific structs: `EFI_GRAPHICS_OUTPUT_PROTOCOL` for the framebuffer, `EFI_FILE_PROTOCOL` and `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` for filesystem access, `EFI_BLOCK_IO_PROTOCOL` for raw disk I/O, and `EFI_LOADED_IMAGE_PROTOCOL` for querying the running image.

Finally, the **well-known GUIDs** — `EFI_LOADED_IMAGE_PROTOCOL_GUID`, `EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID`, `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID`, `EFI_BLOCK_IO_PROTOCOL_GUID` — that Boot Services needs when locating protocol handles.

#### Function Pointers vs. void *

The most important design decision in the stub is how to declare protocol struct fields. UEFI protocol structs are essentially vtables — structs full of function pointers. `EFI_BOOT_SERVICES` alone has over 40 entries.

Our workstation code doesn't call most of them. We never call `RaiseTPL`, `CreateEvent`, `LoadImage`, or `CalculateCrc32`. For fields we never call, `void *` works fine as a placeholder — it occupies the right amount of space (one pointer) and keeps the struct layout correct.

But for functions we *do* call, TCC is strict: you can't call through a `void *`. The compiler needs the full function pointer type to generate the calling sequence. Attempting `BS->AllocatePool(...)` when `AllocatePool` is declared as `void *` produces:

```
error: function pointer expected
```

So the stub uses a mix:

```c
/* Functions we don't call — void * is fine */
void *RaiseTPL; void *RestoreTPL;
/* Functions we DO call — must be proper function pointers */
EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, VOID **);
EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *,
                                   UINTN *, UINTN *, UINT32 *);
```

The `EFI_BOOT_SERVICES` struct in the stub has about 15 properly-typed function pointer fields interleaved with about 25 `void *` placeholders. Every field must be present and in the correct order — UEFI protocol structs are position-dependent, not name-dependent. Skip a field or swap two, and every subsequent function pointer points to the wrong entry.

The same pattern applies to other protocol structs. `EFI_FILE_PROTOCOL` has function pointers for `Open`, `Close`, `Read`, `Write`, `GetInfo`, `SetInfo`, and `Flush` — all of which the filesystem code calls — plus `void *` for the Rev2 async extensions we don't use.

#### Growing the Stub

The stub isn't static. As the workstation gains features, it needs new UEFI types or calls new Boot Services functions, and the stub must grow to match.

When we added `get_total_memory_mb()` — which calls `GetMemoryMap` to sum physical memory — we needed two changes: the `EFI_MEMORY_DESCRIPTOR` struct definition (with its `Type`, `PhysicalStart`, `NumberOfPages`, and `Attribute` fields), and changing the `GetMemoryMap` entry in `EFI_BOOT_SERVICES` from `void *` to a proper function pointer with the correct five-parameter signature.

When we added `fs_volume_info()` to show free disk space, we needed `EFI_FILE_SYSTEM_INFO` and its GUID `EFI_FILE_SYSTEM_INFO_ID`.

When we added `fs_rename()`, we needed the `SIZE_OF_EFI_FILE_INFO` macro — which computes the offset of the `FileName` field at the end of the struct, used when allocating a buffer for `SetInfo`.

Each addition follows the same process: the workstation code calls a new UEFI function or references a new type, TCC reports the error, and we add the minimum necessary declaration to the stub. The stub grows only as the workstation's UEFI surface area grows.

#### The Rule

If the workstation code calls a UEFI function through a protocol struct pointer, the stub must declare that field as a proper function pointer with the correct signature. If it just passes through or ignores a field, `void *` suffices. This keeps the stub small enough to understand while being precise enough for TCC to generate correct code.

### sys/mman.h

`tccrun.c` includes `<sys/mman.h>` for `mmap`, `munmap`, and `mprotect`. These are Linux memory mapping functions — UEFI has no equivalent. Create a minimal stub:

```c
/* src/tcc-headers/sys/mman.h */
/* stub -- UEFI has no mman.h, shim.h provides the actual declarations */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED  ((void *)-1)
#endif
```

Just the defines — no function declarations. Our `shim.h` already declares the stubs, and the actual implementations will never be called because we use UEFI's `AllocatePool` for all memory allocation.

### __clear_cache

`tccrun.c` calls `__clear_cache()` after writing generated code to memory. On ARM64, this flushes the instruction cache — necessary because ARM64 has separate instruction and data caches (Harvard architecture). If you write machine code to memory and try to execute it without flushing the I-cache, the CPU may execute stale instructions.

For the self-hosting rebuild, we're writing a PE file to disk, not executing JIT code. The cache flush is unnecessary. Add a no-op stub to `shim.c`:

```c
/* no-op on UEFI -- only needed for JIT, not PE file output */
void __clear_cache(void *beg, void *end) { (void)beg; (void)end; }
```

### Token and Definition Files

TCC's unity build pulls in many internal headers. `tcctok.h` includes architecture-specific token files:

```c
#if defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64
#include "i386-tok.h"
#endif

#if defined TCC_TARGET_ARM || defined TCC_TARGET_ARM64
#include "arm-tok.h"
#endif
```

These guards are already conditional — we just need to ship the right token file. Similarly, `stab.h` includes `stab.def`. The Makefile's `copy-sources` target must copy all required files to the FAT32 image, architecture-conditionally:

```makefile
copy-sources:
	@mkdir -p $(BUILDDIR)/esp/src/tcc-headers/sys
	@cp src/*.c src/*.h src/*.S $(BUILDDIR)/esp/src/
	@cp -r src/tcc-headers/* $(BUILDDIR)/esp/src/tcc-headers/
	@mkdir -p $(BUILDDIR)/esp/tools/tinycc
	@cp tools/tinycc/libtcc.c tools/tinycc/libtcc.h \
	    tools/tinycc/tcc.h tools/tinycc/config.h tools/tinycc/tccdefs_.h \
	    tools/tinycc/tccpp.c tools/tinycc/tccgen.c tools/tinycc/tccdbg.c \
	    tools/tinycc/tccasm.c tools/tinycc/tccelf.c tools/tinycc/tccrun.c \
	    tools/tinycc/tccpe.c \
	    tools/tinycc/elf.h tools/tinycc/stab.h tools/tinycc/stab.def \
	    tools/tinycc/dwarf.h tools/tinycc/tcctok.h \
	    $(BUILDDIR)/esp/tools/tinycc/
ifeq ($(ARCH),aarch64)
	@cp tools/tinycc/arm64-gen.c tools/tinycc/arm64-link.c tools/tinycc/arm64-asm.c \
	    tools/tinycc/arm-tok.h \
	    $(BUILDDIR)/esp/tools/tinycc/
else
	@cp tools/tinycc/x86_64-gen.c tools/tinycc/x86_64-link.c tools/tinycc/i386-asm.c \
	    tools/tinycc/i386-tok.h \
	    $(BUILDDIR)/esp/tools/tinycc/
endif
```

Only the architecture-specific code generator, linker, and token files are copied per ESP. This saves about 140KB — meaningful when we're targeting an 8MB SD card.

## The ARM64 Assembler Problem

With PE output working and all headers in place, TCC compiles every `.c` file in the workstation. Then it hits `setjmp_aarch64.S`:

```
/src/setjmp_aarch64.S:21: error: ARM asm not implemented.
```

TCC's ARM64 assembler (`arm64-asm.c`) is a stub. Every function in it calls `asm_error("ARM asm not implemented.")`. TCC can generate ARM64 machine code from C, but it cannot assemble ARM64 assembly language. (The x86 assembler, by contrast, is fully functional.)

We can't use inline assembly either — it goes through the same dummy assembler.

The workaround: pre-assemble the opcodes. We know exactly what instructions `setjmp` and `longjmp` need (Chapter 13). We can compile the `.S` file with GCC, extract the raw opcodes with `objdump`, and embed them as integer arrays in a `.c` file that TCC can compile.

First, check the exact opcodes from the GCC-assembled version:

```bash
$ aarch64-linux-gnu-objdump -d build/aarch64/setjmp.o

0000000000000000 <setjmp>:
   0:   a9005013        stp     x19, x20, [x0]
   4:   a9015815        stp     x21, x22, [x0, #16]
   8:   a9026017        stp     x23, x24, [x0, #32]
   ...
  30:   d2800000        mov     x0, #0x0
  34:   d65f03c0        ret
```

Every ARM64 instruction is exactly 4 bytes. We copy each hex value into a C array.

Create `src/setjmp_aarch64.c`:

```c
/*
 * setjmp/longjmp for ARM64 -- pre-assembled opcodes for TCC
 *
 * TCC's ARM64 assembler is a stub, so we embed raw machine code.
 * The arrays are named setjmp/longjmp directly so the linker resolves
 * calls to the opcode bytes themselves (not a pointer indirection).
 * Must be in .text so the memory is executable.
 */

__attribute__((section(".text")))
unsigned int setjmp[] = {
    0xa9005013, /* stp x19, x20, [x0]       */
    0xa9015815, /* stp x21, x22, [x0, #16]  */
    0xa9026017, /* stp x23, x24, [x0, #32]  */
    0xa9036819, /* stp x25, x26, [x0, #48]  */
    0xa904701b, /* stp x27, x28, [x0, #64]  */
    0xa905781d, /* stp x29, x30, [x0, #80]  */
    0x910003e2, /* mov x2, sp               */
    0xf9003002, /* str x2, [x0, #96]        */
    0x6d06a408, /* stp d8,  d9,  [x0, #104] */
    0x6d07ac0a, /* stp d10, d11, [x0, #120] */
    0x6d08b40c, /* stp d12, d13, [x0, #136] */
    0x6d09bc0e, /* stp d14, d15, [x0, #152] */
    0xd2800000, /* mov x0, #0               */
    0xd65f03c0, /* ret                       */
};

__attribute__((section(".text")))
unsigned int longjmp[] = {
    0xa9405013, /* ldp x19, x20, [x0]       */
    0xa9415815, /* ldp x21, x22, [x0, #16]  */
    0xa9426017, /* ldp x23, x24, [x0, #32]  */
    0xa9436819, /* ldp x25, x26, [x0, #48]  */
    0xa944701b, /* ldp x27, x28, [x0, #64]  */
    0xa945781d, /* ldp x29, x30, [x0, #80]  */
    0xf9403002, /* ldr x2, [x0, #96]        */
    0x9100005f, /* mov sp, x2               */
    0x6d46a408, /* ldp d8,  d9,  [x0, #104] */
    0x6d47ac0a, /* ldp d10, d11, [x0, #120] */
    0x6d48b40c, /* ldp d12, d13, [x0, #136] */
    0x6d49bc0e, /* ldp d14, d15, [x0, #152] */
    0xf100003f, /* cmp x1, #0               */
    0x9a9f1420, /* csinc x0, x1, xzr, ne    */
    0xd65f03c0, /* ret                       */
};
```

Two subtleties here:

**The arrays are named `setjmp` and `longjmp` directly.** Not `setjmp_code` with a function pointer. When other code calls `setjmp(buf)`, the compiler emits a branch to the symbol `setjmp`. If `setjmp` were a function pointer variable in `.data`, the branch would land on the variable — which is data, not executable code. By naming the arrays directly, the symbol `setjmp` points to the opcode bytes themselves.

**`__attribute__((section(".text")))` is mandatory.** Without it, the arrays would be placed in `.data` (they're non-const global variables) or `.rdata` (if const). Both sections are non-executable in UEFI PE files. The firmware marks them NX. Jumping to NX memory causes a Synchronous Exception — the same crash we saw with `EfiLoaderData` in Chapter 13. The `.text` section attribute ensures the opcodes live in executable memory.

The F6 rebuild handler in `edit.c` selects the right file per architecture:

```c
#ifdef __aarch64__
    tcc_add_file(tcc, "/src/setjmp_aarch64.c");
#else
    tcc_add_file(tcc, "/src/setjmp_x86_64.S");
#endif
```

ARM64 uses the pre-assembled C file. x86_64 uses the actual assembly — TCC's x86 assembler works fine.

## TCC PE Subsystem and Image Base

One more piece of the PE puzzle. UEFI PE files need specific header values:

- **Subsystem = 10** (`IMAGE_SUBSYSTEM_EFI_APPLICATION`) — not 2 (GUI) or 3 (Console).
- **Image Base = 0** — UEFI loads images at arbitrary addresses and applies relocations.
- **Entry Point = `efi_main`** — not `_start` or `WinMain`.

TCC's `tccpe.c` already handles UEFI subsystems. The `-Wl,-subsystem=efiapp` option sets subsystem 10, and there's special logic:

```c
if ((pe->subsystem >= 10) && (pe->subsystem <= 12))
    pe->imagebase = 0;
```

When the subsystem is EFI (10-12), TCC automatically sets the image base to 0. We just need to pass the right options:

```c
tcc_set_options(tcc, "-nostdlib -nostdinc -Werror"
                     " -Wl,-subsystem=efiapp -Wl,-e=efi_main");
tcc_set_output_type(tcc, TCC_OUTPUT_DLL);
```

`TCC_OUTPUT_DLL` is important — it produces a PE with relocations. `TCC_OUTPUT_EXE` sets `IMAGE_FILE_RELOCS_STRIPPED` in the PE characteristics, which strips all relocations. Without relocations, UEFI can't load the image at an arbitrary address — it would only work if loaded at exactly address 0, which never happens. DLL mode preserves relocations.

## The Patch Summary

Here's every TCC file we touched, and why:

| File | Change | Purpose |
|------|--------|---------|
| `config.h` | Add `TCC_WCHAR_16` | 16-bit wchar_t for UTF-16 |
| `tcc.h` | Widen `nwchar_t` guard | Use `unsigned short` for wide chars |
| `tcc.h` | Widen PE struct field guards | PE fields available on ARM64/x86_64 |
| `tccgen.c` | Widen 2 wchar guards | Wide string literal type |
| `tccpp.c` | Widen wchar encoding guard | UTF-16 wide char encoding |
| `tccdefs_.h` | Widen `__WCHAR_TYPE__` guard | Predefined macro matches reality |
| `libtcc.c` | Widen `tccpe.c` include | PE code compiled for ARM64/x86_64 |
| `libtcc.c` | Widen PE option parsing | `-Wl,-subsystem=` works |
| `tccelf.c` | Widen `pe_output_file` call | Route ARM64/x86_64 to PE output |
| `tccpe.c` | Guard `sys/stat.h` include | No stat on UEFI |
| `tccpe.c` | Guard `chmod` call | No chmod on UEFI |
| `tccpe.c` | Guard `system()` in PDB | No processes on UEFI |
| `tccpe.c` | Guard `LoadLibraryA`, widen `PE_RUN` | No DLL loading on UEFI; in-memory import resolution needs `TCC_IS_NATIVE` alone |
| `arm64-link.c` | GOT relaxation | ADRP+LDR to ADRP+ADD when no GOT |

Fourteen changes. The biggest — GOT relaxation — is about 15 lines of new code. The rest are one-line guard widenings. But without any single one of them, the self-hosting rebuild doesn't work.

---

**Next:** [Chapter 16: The Self-Hosting Rebuild](chapter-16-the-self-hosting-rebuild)
