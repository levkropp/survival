---
layout: default
title: "Chapter 21: Eating Your Own Dogfood"
parent: "Phase 4.5: Self-Self-Hosting"
nav_order: 1
---

# Chapter 21: Eating Your Own Dogfood

## The Question

For twenty chapters we have built the workstation with GCC. The Makefile invokes `aarch64-linux-gnu-gcc` or `x86_64-linux-gnu-gcc`, links against gnu-efi, runs `objcopy` to convert ELF to PE, and produces the UEFI binary. This works. It has always worked.

But since Chapter 16, the workstation has been self-hosting. Press F6, and TCC compiles every source file, links them into a PE binary, and writes it to disk. The workstation rebuilds itself using the compiler embedded in its own binary. No GCC. No gnu-efi. No `objcopy`. TCC produces PE/COFF directly.

So why is the host build still using GCC?

The F6 rebuild proves that TCC can compile the workstation. Every source file. Every header. Every line of code. If TCC is good enough to build the workstation on the workstation, it is good enough to build the workstation on the host machine. Using GCC for the host build while relying on TCC for the self-hosting rebuild means maintaining two build paths, two sets of assumptions, and two opportunities for things to diverge.

There is a practical problem too. On x86_64, GCC uses the System V calling convention (arguments in `rdi`, `rsi`, `rdx`, `rcx`). TCC with `TCC_TARGET_PE` uses the Microsoft ABI (arguments in `rcx`, `rdx`, `r8`, `r9`). When a user program compiled by TCC calls a workstation function compiled by GCC, the arguments arrive in the wrong registers. The function reads garbage. This is the ABI mismatch that caused F5 (compile-and-run) to crash on x86_64 -- TCC-compiled user code calling GCC-compiled workstation functions with incompatible calling conventions.

One solution is wrapper functions -- fifty thunks that translate between calling conventions. The better solution is to stop having two calling conventions. Build everything with TCC. One compiler, one ABI, one set of assumptions. If it works for F6, it works for `make`.

## Building the Cross-Compilers

TCC is a cross-compiler. The same source code, compiled with different `TCC_TARGET_*` defines, produces compilers for different architectures. We need two:

- `arm64-tcc` -- runs on the host (x86_64 Linux), produces ARM64 PE/COFF output
- `x86_64-win32-tcc` -- runs on the host, produces x86_64 PE/COFF output

Building them from our patched TCC source:

```bash
# ARM64 cross-compiler
gcc -O2 -w \
    -DTCC_TARGET_ARM64=1 \
    -DTCC_USING_DOUBLE_FOR_LDOUBLE=1 \
    -DCONFIG_TCC_CROSSPREFIX=\"arm64-\" \
    -DONE_SOURCE=1 \
    -o tools/tcc-host/arm64-tcc \
    tools/tinycc/tcc.c -lm -ldl -lpthread

# x86_64 cross-compiler
gcc -O2 -w \
    -DTCC_TARGET_X86_64=1 \
    -DTCC_TARGET_PE=1 \
    -DCONFIG_TCC_CROSSPREFIX=\"x86_64-win32-\" \
    -DONE_SOURCE=1 \
    -o tools/tcc-host/x86_64-win32-tcc \
    tools/tinycc/tcc.c -lm -ldl -lpthread
```

Yes, we use GCC to compile TCC itself. This is the one remaining use of GCC -- bootstrapping the cross-compilers. The cross-compilers are checked into `tools/tcc-host/` as pre-built binaries, so a clean build never needs GCC. Only regenerating the cross-compilers does, and that is a one-time operation.

Two things to note about the ARM64 compiler:

**`TCC_USING_DOUBLE_FOR_LDOUBLE=1`** -- ARM64 normally uses 128-bit `long double`, which requires libgcc builtins like `__extenddftf2` and `__trunctfdf2`. We do not have libgcc. This flag tells TCC to treat `long double` as 64-bit `double`, eliminating the need for 128-bit arithmetic entirely. The workstation does not use `long double` anywhere, so nothing is lost.

**No `TCC_TARGET_PE` for ARM64** -- Unlike x86_64, the ARM64 compiler does not define `TCC_TARGET_PE`. Chapter 15 widened the guards in `tccpe.c` and `libtcc.c` to include ARM64 PE output without the full `TCC_TARGET_PE` machinery, because TCC's PE support was x86-only and enabling it wholesale on ARM64 would pull in x86 code generation paths. The ARM64 compiler uses the same widened guards that the F6 rebuild uses.

The cross-compilers are small -- about 350K each. They are regular Linux executables that happen to produce UEFI PE binaries as output.

## The New Makefile

The old Makefile was a pipeline:

```
GCC → .o (ELF) → ld → .so (ELF) → objcopy → .efi (PE)
```

Seven tools: GCC, GNU as, GNU ld, objcopy, plus gnu-efi's `crt0-efi.o`, `libgnuefi.a`, and `libefi.a`. The ELF-to-PE conversion was the most fragile step -- getting the section flags, entry point, and relocations right required matching gnu-efi's linker script exactly.

The new Makefile is a straight line:

```
TCC → .o (COFF) → TCC → .efi (PE)
```

One tool. TCC compiles C to COFF objects and links them into a PE binary. No intermediate formats. No conversion steps. No linker scripts.

Here is the architecture selection:

```makefile
ARCH     ?= aarch64

ifeq ($(ARCH),aarch64)
  TCC         := tools/tcc-host/arm64-tcc
  EFI_BOOT    := BOOTAA64.EFI
  TCC_TARGET  := -DTCC_TARGET_ARM64=1
  SETJMP_SRC  := $(SRCDIR)/setjmp_aarch64.c
  TCC_UNDEF   :=
else ifeq ($(ARCH),x86_64)
  TCC         := tools/tcc-host/x86_64-win32-tcc
  EFI_BOOT    := BOOTX64.EFI
  TCC_TARGET  := -DTCC_TARGET_X86_64=1
  SETJMP_SRC  := $(SRCDIR)/setjmp_x86_64.S
  TCC_UNDEF   := -U_WIN32 -U_WIN64
endif
```

The x86_64 compiler predefines `_WIN32` and `_WIN64` for its target (it thinks it is building Windows programs). We suppress these with `-U` flags because our UEFI code is not Windows code.

Compilation is straightforward:

```makefile
CFLAGS   := -nostdlib -nostdinc \
            -Isrc/tcc-headers -Isrc -I$(TCC_DIR) \
            -Wall

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(TCC) $(CFLAGS) -c -o $@ $<
```

The include path starts with `src/tcc-headers/`, which contains our stub `efi.h` and `efilib.h`. When `boot.h` does `#include <efi.h>`, it gets our minimal UEFI type stubs -- not gnu-efi's sprawling header tree. The stub `efi.h` does not define `_EFI_INCLUDE_`, so `shim.h` provides all C standard types itself. gnu-efi is gone.

The TCC unity build (libtcc.c) needs special flags:

```makefile
TCC_CFLAGS := -nostdlib -nostdinc -w \
              $(TCC_UNDEF) \
              -Isrc/tcc-headers -I$(TCC_DIR) \
              -DONE_SOURCE=1 $(TCC_TARGET) -D__UEFI__
```

`-w` suppresses all warnings for TCC's own source. `$(TCC_UNDEF)` adds `-U_WIN32 -U_WIN64` on x86_64. `__UEFI__` gates out Windows-specific code paths.

There is one quirk with TCC's unity build. `libtcc.c` with `ONE_SOURCE=1` includes all TCC source files via `#include`. When TCC sees multiple `.c` files in a single compilation, it refuses to use `-c -o` together ("cannot specify output file with -c many files"). The workaround: compile without `-o` (the output goes to the current directory) and move it:

```makefile
$(BUILDDIR)/libtcc.o: $(TCC_DIR)/libtcc.c
	$(TCC) $(TCC_CFLAGS) -c $<
	@mv libtcc.o $@
```

Linking is the biggest simplification. The old Makefile had a hand-crafted link command with gnu-efi's CRT, two libraries, libgcc, and a custom linker script. The new one:

```makefile
$(TARGET): $(OBJECTS)
	$(TCC) -nostdlib -shared \
		-Wl,-subsystem=efiapp -Wl,-e=efi_main \
		-o $@ $(OBJECTS)
```

Three flags do all the work:

**`-shared`** tells TCC's PE linker to generate relocations. UEFI firmware requires a `.reloc` section in the PE binary -- it loads the binary at an arbitrary address and patches all absolute references. Without `-shared`, TCC produces a PE with no relocations, and the firmware drops to the UEFI shell without running the binary. This is the same `TCC_OUTPUT_DLL` that the F6 rebuild uses.

**`-Wl,-subsystem=efiapp`** sets the PE subsystem field to EFI Application (value 10). UEFI firmware checks this field to confirm the binary is a UEFI application, not a Windows executable.

**`-Wl,-e=efi_main`** sets the entry point to our `efi_main` function.

No linker script. No CRT object. No library dependencies. TCC's PE linker handles symbol resolution, relocation generation, and PE header construction internally.

## What Broke

Switching compilers always surfaces assumptions. Here are the fixes, in the order they appeared.

### Function Pointer Types in efi.h

The stub `efi.h` originally declared `AllocatePages` and `FreePages` as `void *`:

```c
/* Old -- void pointers */
void *AllocatePages;
void *FreePages;
```

GCC silently allows calling through a `void *` cast to a function pointer. TCC does not -- it reports "function pointer expected." The fix is proper function pointer types:

```c
typedef enum {
    AllocateAnyPages, AllocateMaxAddress, AllocateAddress, MaxAllocateType
} EFI_ALLOCATE_TYPE;

EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE,
                                    UINTN, EFI_PHYSICAL_ADDRESS *);
EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
```

This is more correct than the `void *` it replaces. TCC enforced what GCC let slide.

### windows.h Include Guard

When compiling `libtcc.c` for x86_64, TCC's own `tcc.h` has:

```c
#ifdef _WIN32
# include <windows.h>
#endif
```

The x86_64 cross-compiler predefines `_WIN32` for its target. Even though we pass `-U_WIN32` on the command line, TCC processes predefined macros before command-line undefines in some code paths. The reliable fix is a source-level guard:

```c
#if defined(_WIN32) && !defined(__UEFI__)
# include <windows.h>
#endif
```

Now `__UEFI__` (from our `-D__UEFI__` flag) permanently blocks the Windows header include.

### setjmp for MS ABI

The x86_64 `setjmp_x86_64.S` was written for the System V ABI (which GCC uses). On System V, the first argument goes in `rdi`. On the Microsoft ABI (which TCC PE uses), the first argument goes in `rcx`.

But the register difference goes deeper than just the argument. The two ABIs disagree about which registers are callee-saved:

| Register | System V | Microsoft ABI |
|----------|----------|---------------|
| `rdi`    | caller-saved | **callee-saved** |
| `rsi`    | caller-saved | **callee-saved** |
| `rbx`    | callee-saved | callee-saved |
| `rbp`    | callee-saved | callee-saved |
| `r12-r15`| callee-saved | callee-saved |

Under System V, `rdi` and `rsi` are scratch registers -- the caller expects them to be clobbered. Under Microsoft ABI, they must be preserved. If `setjmp` does not save them and `longjmp` does not restore them, any function that keeps values in `rdi` or `rsi` across a `setjmp`/`longjmp` pair will see corruption.

The updated `setjmp` saves 10 registers instead of 8:

```asm
setjmp:
    /* rcx = jmp_buf pointer (MS ABI first arg) */
    movq %rbx,   (%rcx)
    movq %rbp,  8(%rcx)
    movq %rdi, 16(%rcx)     /* MS ABI callee-saved */
    movq %rsi, 24(%rcx)     /* MS ABI callee-saved */
    movq %r12, 32(%rcx)
    movq %r13, 40(%rcx)
    movq %r14, 48(%rcx)
    movq %r15, 56(%rcx)
    leaq 8(%rsp), %rax      /* rsp before call */
    movq %rax, 64(%rcx)
    movq (%rsp), %rax        /* return address */
    movq %rax, 72(%rcx)
    xorl %eax, %eax
    ret
```

And `longjmp` restores all 10, with the return value in `edx` (MS ABI second arg):

```asm
longjmp:
    /* rcx = jmp_buf, edx = return value */
    movq   (%rcx), %rbx
    movq  8(%rcx), %rbp
    movq 16(%rcx), %rdi
    movq 24(%rcx), %rsi
    movq 32(%rcx), %r12
    movq 40(%rcx), %r13
    movq 48(%rcx), %r14
    movq 56(%rcx), %r15
    movq 64(%rcx), %rsp
    movq 72(%rcx), %r8
    movl %edx, %eax
    testl %eax, %eax
    jnz 1f
    movl $1, %eax
1:  jmp *%r8
```

The `jmp_buf` size in `shim.h` grows from 8 to 10 entries:

```c
#elif defined(__x86_64__)
typedef long long jmp_buf[10];  /* 80 bytes: rbx, rbp, rdi, rsi, r12-r15, rsp, ret */
```

The ARM64 `setjmp` needs no changes -- ARM64 has the same calling convention everywhere (AAPCS64). There is no System V vs. Microsoft split on ARM64.

### ARM64 Long Double

The first ARM64 build with TCC fails at link time with nineteen undefined symbols:

```
undefined symbol '__extenddftf2'
undefined symbol '__trunctfdf2'
undefined symbol '__multf3'
undefined symbol '__addtf3'
...
```

These are compiler builtins for 128-bit `long double` arithmetic. On ARM64, the C standard allows `long double` to be 128-bit IEEE quad-precision. GCC uses libgcc to provide these builtins. TCC does not have libgcc.

The workstation never uses `long double` explicitly, but TCC's own source code has a few `long double` constants and operations internally (in the constant folding and floating-point parsing code).

The fix is `TCC_USING_DOUBLE_FOR_LDOUBLE=1` when building the ARM64 cross-compiler. This flag tells TCC to treat `long double` as 64-bit `double` -- same size, same instructions, no builtins needed. On x86_64, `long double` is already 80-bit or 64-bit depending on the platform, so this is only needed for ARM64.

### PE Relocations

The first x86_64 binary that TCC links boots into the UEFI shell instead of the workstation. No error message, no crash -- the firmware simply doesn't run the binary.

Examining the PE headers with `objdump -x` reveals the problem:

```
The Data Directory
...
5 00000000 00000000 Base Relocation Directory [.reloc]
```

The relocation directory is empty. UEFI firmware loads PE binaries at arbitrary addresses -- it needs relocations to patch absolute references. A PE with no relocations is assumed to be position-dependent, and the firmware cannot guarantee loading it at its preferred base address. So it falls back to the shell.

The fix is `-shared` in the link command. In TCC's PE linker, `-shared` (which maps to `TCC_OUTPUT_DLL`) triggers relocation generation. Without it, TCC produces an executable with no `.reloc` section. This is the same distinction that Chapter 16's F6 rebuild discovered -- `TCC_OUTPUT_DLL` is required for any PE that UEFI will load.

## What We Dropped

The old build system required:

- **GCC** (architecture-specific cross-compiler)
- **GNU binutils** (as, ld, objcopy)
- **gnu-efi** (headers, CRT, libraries -- built from source for ARM64)
- **libgcc** (compiler builtins)

The dependency graph was:

```
survival.efi
  ← objcopy (ELF → PE conversion)
    ← ld (linking with gnu-efi linker script)
      ← gcc (compilation)
      ← crt0-efi-aarch64.o (gnu-efi CRT)
      ← libefi.a (gnu-efi library)
      ← libgnuefi.a (gnu-efi library)
      ← libgcc.a (compiler builtins)
```

The new build requires:

- **TCC cross-compiler** (pre-built, checked in at `tools/tcc-host/`)

That is it. The dependency graph:

```
survival.efi
  ← tcc (compilation + linking)
```

The stub headers in `src/tcc-headers/` replace gnu-efi's header tree. They define exactly the UEFI types the workstation uses -- about 400 lines versus gnu-efi's thousands. TCC's built-in PE linker replaces GNU ld plus the ELF-to-PE objcopy step. The `-shared` flag replaces gnu-efi's linker script for generating relocations.

Building the workstation on a fresh machine requires only `make`. No package installation. No cross-compiler setup. The TCC binaries in `tools/tcc-host/` are self-contained Linux executables with no external dependencies beyond libc.

## The Consistency Guarantee

Before this change, there were two ways to build the workstation:

1. **Host build** (GCC) -- used for development, tested in QEMU
2. **F6 rebuild** (TCC) -- used on the workstation itself

These two paths could diverge. GCC and TCC have different type sizes under different data models, different warning behaviors, different optimization strategies. A change that compiles under GCC might fail under TCC, and the developer would not discover this until pressing F6 on the actual workstation.

Now there is one path. `make` uses TCC. F6 uses TCC. The same compiler, the same flags, the same type sizes, the same calling convention. If `make` succeeds, F6 will succeed. If `make` produces a working binary, F6 will produce a working binary.

This also means the ABI mismatch on x86_64 vanishes. When GCC built the workstation, workstation functions used System V ABI. When TCC compiled user programs (F5), they used Microsoft ABI. Calling across the boundary crashed. Now everything is Microsoft ABI -- the workstation, the embedded TCC, and the code TCC compiles. Arguments go in `rcx`, `rdx`, `r8`, `r9` everywhere. No wrappers. No thunks. No boundary.

## The Build

```
$ make ARCH=aarch64
tools/tcc-host/arm64-tcc -nostdlib -nostdinc -Isrc/tcc-headers -Isrc ... -c -o build/aarch64/main.o src/main.c
...
tools/tcc-host/arm64-tcc -nostdlib -shared -Wl,-subsystem=efiapp -Wl,-e=efi_main -o build/aarch64/survival.efi ...

=== Built: build/aarch64/survival.efi (aarch64) ===
-rw-r--r-- 1 user user 570K build/aarch64/survival.efi

$ make ARCH=x86_64
tools/tcc-host/x86_64-win32-tcc -nostdlib -nostdinc -Isrc/tcc-headers -Isrc ... -c -o build/x86_64/main.o src/main.c
...
tools/tcc-host/x86_64-win32-tcc -nostdlib -shared -Wl,-subsystem=efiapp -Wl,-e=efi_main -o build/x86_64/survival.efi ...

=== Built: build/x86_64/survival.efi (x86_64) ===
-rw-r--r-- 1 user user 447K build/x86_64/survival.efi
```

Both binaries are smaller than the GCC-built versions (which were 665K and 638K). TCC does not add the padding, alignment, and section overhead that GCC's toolchain introduces. The workstation is the same code, doing the same things, in less space.

## Eating Your Own Dogfood

The phrase "eating your own dogfood" comes from software companies that use their own products internally. If your email client crashes every hour, you fix it fast when you are the one losing email.

By building the workstation with TCC, we are eating our own dogfood. Every `make` invocation exercises the same code paths that the F6 rebuild uses. Every compilation tests the stub headers, the PE linker, the calling convention, the type sizes. If something is wrong with TCC's output for UEFI, we discover it immediately -- not after deploying to hardware and pressing F6.

The workstation now has a single toolchain from development machine to bare metal. The same TCC source code, compiled two ways:

1. **As a host tool** (`tools/tcc-host/arm64-tcc`) -- a Linux executable that cross-compiles for ARM64 UEFI
2. **As an embedded library** (`tools/tinycc/libtcc.c` compiled into the workstation) -- runs on bare metal, compiles user programs (F5) and rebuilds the workstation (F6)

Both come from the same patched TCC source in `tools/tinycc/`. Both use the same stub headers in `src/tcc-headers/`. Both produce the same PE/COFF output format. The only difference is where they run.

This is the logical conclusion of self-hosting. Not just "the system can rebuild itself" but "the system is built by itself, always." The development build and the production build use the same compiler. The host machine and the target machine use the same compiler. There is no privileged external tool that the system depends on but cannot reproduce.

The workstation carries its own compiler. Now its build system uses that same compiler. The circle is complete.
