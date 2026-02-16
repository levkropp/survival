---
layout: default
title: "Chapter 18: x86_64 Self-Hosting"
parent: "Phase 3: The C Compiler"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 5
---

# Chapter 18: x86_64 Self-Hosting

## The Crash

Chapters 15 and 16 got self-hosting working on ARM64. Press F6, TCC compiles every source file, writes a PE binary to disk, reboot, and the new workstation runs. The chain is self-sustaining.

On x86_64, the same F6 rebuild completes without errors. TCC compiles all fourteen source files, links them, writes `BOOTX64.EFI` to the FAT32 filesystem. "BUILD COMPLETE!" appears in green. Press R to reboot.

The screen goes black. Then:

```
!!!! X64 Exception Type - 06(#UD - Invalid Opcode) ...
```

Invalid Opcode. The CPU encountered bytes it doesn't recognize as instructions. The rebuilt binary is garbage.

This chapter is the debugging story. Six fixes, discovered one at a time, each revealing the next problem underneath.

## Fix 1: The Root Cause -- TCC_TARGET_PE

The ARM64 port required fourteen patches to TCC (Chapter 15). The deepest was GOT relaxation in `arm64-link.c` -- converting GOT-indirect global variable access to direct addressing, because PE output has no GOT.

On x86_64, the same GOT problem exists, but it's worse. Without `TCC_TARGET_PE` defined, TCC's x86_64 code generator produces:

- **GOT-based global variable access**. Every global variable load goes through `mov r11, [rip+disp]` to fetch an address from the GOT, then a second load through that address. But the PE linker never calls `build_got_entries()`, so every GOT slot contains zero. Every global variable read returns garbage.

- **System V calling convention**. On Linux, function arguments go in `rdi`, `rsi`, `rdx`, `rcx`. On UEFI x86_64, which uses the Microsoft ABI, arguments go in `rcx`, `rdx`, `r8`, `r9`. Every function call passes arguments in the wrong registers.

Either one of these alone would crash the binary. Together, they make the output completely non-functional.

The ARM64 GOT relaxation approach -- patching the linker to convert GOT references to direct references -- won't help here. The calling convention mismatch is a code generation problem, not a linking problem. TCC generates the wrong instructions. The fix has to happen in the code generator.

TCC already knows how to do all of this correctly. When `TCC_TARGET_PE` is defined, the x86_64 code generator:

- Uses direct RIP-relative addressing for globals (no GOT)
- Uses Microsoft ABI calling convention (`rcx`, `rdx`, `r8`, `r9`)
- Generates proper PE/COFF output through `tccpe.c`

On ARM64, we couldn't define `TCC_TARGET_PE` because TCC's PE code generator didn't support ARM64 at all -- we had to widen individual guards and add GOT relaxation. On x86_64, `TCC_TARGET_PE` is the right answer. TCC's x86_64 PE support is mature -- it's the same code path that builds Windows executables.

In `tools/tinycc/config.h`, after the architecture detection:

```c
/* x86_64 UEFI uses PE/COFF output and MS ABI calling convention.
   TCC_TARGET_PE enables: no GOT, MS ABI in code generator, PE linker. */
#ifdef TCC_TARGET_X86_64
# define TCC_TARGET_PE 1
#endif
```

One define. It fixes both the GOT problem and the calling convention. But enabling `TCC_TARGET_PE` pulls in Windows-specific code that doesn't exist on UEFI, so the build immediately breaks.

There's a second problem. TCC auto-detects whether it's a "native" compiler -- one that can compile and run code on the same platform. The detection logic in `tcc.h` checks whether the host platform matches the target: if the host is `_WIN32` and the target is `TCC_TARGET_PE`, it's native. But we're on UEFI, not Windows. `_WIN32` is not defined. TCC concludes it's a cross-compiler and disables `tcc_relocate()` -- the function F5 needs to compile and run code in memory.

Force native mode:

```c
/* Force native support (F5 in-memory compile+run via tcc_relocate).
   tcc.h's auto-detection fails because _WIN32 != TCC_TARGET_PE on UEFI. */
#define TCC_IS_NATIVE 1
```

Now build. The compiler chokes on four separate Windows-isms that `TCC_TARGET_PE` drags in.

## Fix 2: pe_create_pdb Calls system()

First build error:

```
tccpe.c:555: error: 'system' undeclared
```

`tccpe.c` has a function `pe_create_pdb()` that generates Windows PDB debug symbols by calling `system("cv2pdb.exe ...")` -- spawning an external process. UEFI has no `system()`. There are no processes to spawn.

The function definition and its call site both need guards:

```c
/* Around line 552 -- the function definition */
#if defined(TCC_TARGET_PE) && !defined(__UEFI__)
static void pe_create_pdb(TCCState *s1, const char *exename)
{
    char buf[300]; int r;
    snprintf(buf, sizeof buf, "cv2pdb.exe %s", exename);
    r = system(buf);
    ...
}
#endif

/* Around line 834 -- the call site */
#if defined(TCC_TARGET_PE) && !defined(__UEFI__)
    if (s1->do_debug & 16)
        pe_create_pdb(s1, pe->filename);
#endif
```

The `__UEFI__` define comes from our Makefile's `TCC_CFLAGS` (set up in Chapter 15). The pattern is the same one we used for `chmod()` and `sys/stat.h` -- guard Windows-host-specific code so it compiles out on UEFI.

## Fix 3: tcc_add_systemdir Calls GetSystemDirectoryA

Second build error:

```
libtcc.c:123: error: 'GetSystemDirectoryA' undeclared
```

In `libtcc.c`, there's a function `tcc_add_systemdir()` that calls the Windows API `GetSystemDirectoryA()` to find `C:\Windows\System32` and add it as a library search path. This makes sense on Windows -- you want to link against system DLLs. On UEFI, there is no System32 directory.

The function is defined inside an `#ifdef TCC_IS_NATIVE` block, so it only exists when TCC thinks it's a native compiler. We just forced native mode. Now it tries to call Windows API functions.

Guard the call site:

```c
#ifdef TCC_TARGET_PE
# if defined(TCC_IS_NATIVE) && !defined(__UEFI__)
    /* allow linking with system dll's directly */
    tcc_add_systemdir(s);
# endif
```

The function definition itself is also inside `#ifdef TCC_IS_NATIVE`, but since it's only called from this one place, guarding the call site is sufficient. The function compiles but is never called -- the linker strips dead code.

## Fix 4: _WIN32 Predefine Poisons F6 Rebuild

The build succeeds. The workstation boots. Press F6 to rebuild. TCC starts compiling the workstation source files and immediately hits:

```
/src/shim.c:1: error: #include <windows.h> -- file not found
```

Wait -- our code doesn't include `windows.h`. What's happening?

When `TCC_TARGET_PE` is defined, TCC automatically predefines `_WIN32` as a built-in macro -- the same way GCC predefines `__linux__` on Linux. This makes sense for a Windows compiler: Windows programs test `#ifdef _WIN32` everywhere.

But during the F6 rebuild, TCC is compiling our UEFI workstation source code. Some of TCC's own internal headers have `#ifdef _WIN32` guards that pull in Windows-specific includes. With `_WIN32` predefined, these guards activate, and the compile fails looking for Windows headers that don't exist.

The fix is in `tccpp.c`, where TCC defines its target OS macros:

```c
static const char * const target_os_defs =
#if defined(TCC_TARGET_PE) && !defined(__UEFI__)
    "_WIN32\0"
```

When `__UEFI__` is defined (our `-D__UEFI__` in the Makefile), TCC skips the `_WIN32` predefine. The workstation source compiles cleanly. The rebuilt TCC inside the new binary also won't predefine `_WIN32`, because the F6 rebuild passes `-D__UEFI__` via `tcc_define_symbol()`.

This is a subtle chain: the `__UEFI__` define must propagate through two levels of compilation. First, GCC uses it when building the original workstation binary (via Makefile's `TCC_CFLAGS`). Second, TCC uses it when the F6 rebuild compiles `libtcc.c` (via `tcc_define_symbol(tcc, "__UEFI__", "1")` in the rebuild handler). Both levels must suppress the `_WIN32` predefine, or the chain breaks.

## Fix 5: LLP64 Type Sizes

F6 rebuild compiles further now. But it crashes during the link phase, or produces a binary that crashes at boot. The symptoms are inconsistent -- sometimes a page fault, sometimes Invalid Opcode, sometimes the screen stays black.

The problem is architectural. With `TCC_TARGET_PE` enabled, TCC uses the **LLP64** data model -- the same model Windows x86_64 uses:

| Type | LP64 (Linux) | LLP64 (Windows/PE) |
|------|-------------|-------------------|
| `int` | 4 bytes | 4 bytes |
| `long` | 8 bytes | **4 bytes** |
| `long long` | 8 bytes | 8 bytes |
| `void *` | 8 bytes | 8 bytes |

On LP64, `long` is 8 bytes. On LLP64, `long` is 4 bytes. Every typedef in our code that uses `unsigned long` for a 64-bit quantity is now wrong.

### efi.h

The stub `efi.h` in `src/tcc-headers/` originally used `unsigned long` for 64-bit UEFI types:

```c
/* BROKEN under LLP64 -- these are 4 bytes, not 8 */
typedef unsigned long UINT64;
typedef signed long   INT64;
typedef unsigned long UINTN;
typedef signed long   INTN;
```

`UINTN` is supposed to be pointer-width -- 8 bytes on a 64-bit platform. With LLP64, `unsigned long` is 4 bytes. Every `UINTN` value is truncated. `EFI_STATUS` is a `UINTN`. Pointers stored as `UINTN` lose their upper 32 bits.

Fix: use `long long` for all 64-bit types:

```c
/* TCC PE/x86_64 uses LLP64 (long=32-bit), so use long long for 64-bit.
   aarch64 LP64 (long=64-bit) also works fine with long long. */
typedef unsigned long long UINT64;
typedef signed long long   INT64;
typedef unsigned long long UINTN;
typedef signed long long   INTN;
```

This is safe on LP64 too. On both data models, `long long` is 8 bytes. Using `long long` everywhere makes the types correct regardless of which data model TCC uses.

### shim.h

The same problem exists in `shim.h`, which defines standard C types for TCC:

```c
/* BROKEN under LLP64 */
typedef unsigned long uint64_t;
typedef signed long   int64_t;
typedef unsigned long uintptr_t;
typedef signed long   intptr_t;
```

And most dangerously, `jmp_buf`:

```c
#elif defined(__x86_64__)
typedef long jmp_buf[8];
```

The x86_64 `setjmp` implementation saves 8 registers, each 8 bytes -- 64 bytes total. With LP64, `long` is 8 bytes, so `long jmp_buf[8]` is 64 bytes. Correct.

With LLP64, `long` is 4 bytes. `long jmp_buf[8]` is 32 bytes. But `setjmp_x86_64.S` still stores 8 registers at 8 bytes each -- 64 bytes. It writes 64 bytes into a 32-byte buffer. The remaining 32 bytes overwrite whatever sits after `jmp_buf` on the stack -- return addresses, saved registers, local variables. When `longjmp` restores these corrupted values, the CPU jumps to garbage.

This is particularly insidious because `jmp_buf` is used for TCC's error recovery. Every TCC compilation wraps critical code in `setjmp`/`longjmp`. The corruption might not manifest immediately -- it depends on what the overwritten stack bytes happen to be. Sometimes the binary boots and runs for a while before crashing. Sometimes it crashes instantly.

Fix everything in `shim.h`:

```c
/* Use long long for 64-bit: safe on both LP64 and LLP64 (TCC PE x86_64) */
typedef unsigned long long uint64_t;
typedef signed   long long int64_t;
typedef unsigned long long uintptr_t;
typedef signed   long long intptr_t;

...

#elif defined(__x86_64__)
typedef long long jmp_buf[8];   /* 64 bytes: rbx, rbp, r12-r15, rsp, ret addr */
#endif
```

Now `jmp_buf` is 64 bytes on both data models. The assembly stores exactly 64 bytes. No overflow.

## Fix 6: The __chkstk Saga

F6 rebuild now compiles cleanly and produces a PE binary. Reboot into it. The screen shows the workstation UI for a fraction of a second, then:

```
!!!! X64 Exception Type - 06(#UD - Invalid Opcode)  CPU Apic ID - 00000000 !!!!
RIP  - 00000000000E0001
```

Invalid Opcode at a suspiciously low address. `0xE0001` looks like an offset from the PE image base, but it's inside the first page -- nowhere near any real function code.

This crash only happens in the TCC-rebuilt binary, not the GCC-built one. And it doesn't happen for small functions. It happens when the workstation calls a function with a large stack frame -- specifically, any function that allocates more than 4096 bytes of local variables.

### How TCC Generates Stack Frames

Looking at `x86_64-gen.c`, TCC has two paths for function prologues:

When the stack frame is **less than 4096 bytes**:

```c
o(0xe5894855);  /* push %rbp; mov %rsp, %rbp */
o(0xec8148);    /* sub rsp, stacksize */
ov(v);
```

Normal prologue. Push the frame pointer, set up the new frame, allocate stack space. Straightforward.

When the stack frame is **4096 bytes or more**:

```c
oad(0xb8, v);   /* mov stacksize, %eax */
oad(0xe8, 0);   /* call __chkstk, (does the stackframe too) */
greloca(cur_text_section, sym, ind-4, R_X86_64_PLT32, -4);
o(0x90);        /* nop -- fill for FUNC_PROLOG_SIZE = 11 bytes */
```

TCC loads the stack size into `eax` and calls `__chkstk`. The comment is revealing: "does the stackframe too." This means `__chkstk` is responsible not just for probing the stack pages (its original Windows purpose), but for the entire frame setup -- `push rbp`, `mov rbp, rsp`, `sub rsp, rax`.

On Windows, Microsoft's `__chkstk` probes each page of stack one at a time to trigger the guard page mechanism, then returns. But Microsoft's `__chkstk` does NOT set up the frame -- the compiler emits the frame setup separately. TCC's x86_64 backend is different. When the stack is large, TCC emits ONLY the `call __chkstk` -- no `push rbp`, no `mov rbp, rsp`, no `sub rsp`. The called function must do all of it.

We don't have Microsoft's `__chkstk`. We don't have any `__chkstk`. The symbol is unresolved. During the F6 rebuild, TCC's PE linker resolves it to address 0 (or some small garbage offset), and the `call` jumps there. Invalid Opcode.

We need to provide our own `__chkstk` that does what TCC expects: set up the entire stack frame.

### First Attempt: Just Return

The simplest possible implementation:

```c
void __chkstk(void) { }
```

This compiles. The binary boots. Then immediately shuts down -- no crash message, no error, just a black screen and power off. The function returns without setting up a frame, so the caller's `rsp` is wrong, `rbp` is wrong, every local variable access corrupts memory.

### Second Attempt: Set Up the Frame, Then Return

Think harder. `__chkstk` receives the desired stack size in `rax`. It needs to set up the frame:

```asm
__chkstk:
    push %rbp
    mov  %rsp, %rbp
    sub  %rax, %rsp
    ret
```

This looks right. But consider what happens step by step:

1. Before `call __chkstk`: the return address is at `[rsp]`, and `rax` holds the stack size (say, 8192).
2. `call __chkstk` pushes the return address. Now `rsp` points to the return address inside `__chkstk`.
3. `push %rbp` pushes the old frame pointer. `rsp` moves down 8 bytes.
4. `mov %rsp, %rbp` sets the frame pointer.
5. `sub %rax, %rsp` subtracts 8192 from `rsp`. Now `rsp` is 8192 bytes below `rbp`.
6. `ret` pops 8 bytes from `[rsp]` and jumps there.

But `[rsp]` is now 8192 bytes below where the return address was pushed. The `ret` pops whatever garbage happens to be at that stack location. The CPU jumps to `0xE0001`. Invalid Opcode.

The problem: `call` pushes the return address, and `sub rsp, rax` moves the stack pointer far away from it. A `ret` instruction looks for the return address at `[rsp]`, but the return address is 8192 bytes above.

### Third Attempt: Pop, Set Up, Jump

The return address is on top of the stack when `__chkstk` is entered. Pop it before moving the stack pointer:

```asm
__chkstk:
    pop  %r10           ; save return address
    push %rbp           ; save old frame pointer
    mov  %rsp, %rbp     ; set new frame pointer
    sub  %rax, %rsp     ; allocate stack space
    jmp  *%r10          ; return (can't use ret -- rsp has moved)
```

Step by step:

1. `call __chkstk` pushes return address. `rsp` points to it.
2. `pop %r10` retrieves the return address. `rsp` moves back up to where the caller's `rsp` was before `call`.
3. `push %rbp` saves the old frame pointer. This is at the correct position -- exactly where the caller's prologue would have pushed it.
4. `mov %rsp, %rbp` sets the frame pointer.
5. `sub %rax, %rsp` allocates the local variable space.
6. `jmp *%r10` returns to the caller. We can't use `ret` because `rsp` is now deep in the stack. `jmp` through `r10` goes back to the instruction after `call __chkstk` without touching the stack.

The caller continues with `rbp` pointing to the saved frame pointer and `rsp` pointing to the bottom of the allocated stack frame -- exactly the state a normal function prologue would produce.

Why `r10`? On the Microsoft x86_64 ABI, `r10` and `r11` are volatile (caller-saved) scratch registers. The caller doesn't expect them to survive across a call. Using `r10` for the return address is safe -- we're not clobbering anything the caller needs.

Stack probing -- the original purpose of `__chkstk` on Windows -- is unnecessary. Windows uses guard pages: the OS allocates stack memory one page at a time, and touching a guard page triggers allocation of the next page. Skipping pages could miss the guard page and corrupt the heap. UEFI doesn't use guard pages. The stack is a flat allocation from `AllocatePool`. We can skip pages freely.

### The Implementation

The `__chkstk` function must be written in assembly because it manipulates the stack in ways C can't express. But it only needs to exist in TCC-compiled code -- GCC handles large stack frames differently and never calls `__chkstk`.

In `src/shim.c`, at the very top of the file:

```c
/* TCC PE x86_64 generates __chkstk calls for large stack frames (>4096).
   Unlike Microsoft's __chkstk, TCC's version must set up the entire frame:
   push rbp; mov rbp, rsp; sub rsp, rax  (see x86_64-gen.c line 1044).
   The call pushes a return address; we pop it, set up the frame, then jmp back
   (can't use ret since rsp has moved). Stack probing is unnecessary on UEFI. */
#if defined(__x86_64__) && defined(__TINYC__)
__asm__(
    ".globl __chkstk\n"
    "__chkstk:\n"
    "  pop %r10\n"
    "  push %rbp\n"
    "  mov %rsp, %rbp\n"
    "  sub %rax, %rsp\n"
    "  jmp *%r10\n"
);
#endif
```

The `__TINYC__` guard is essential. This code compiles only when TCC compiles `shim.c` -- during the F6 rebuild. When GCC compiles `shim.c` for the initial build, `__TINYC__` is not defined and the `__asm__` block disappears entirely. GCC never generates `call __chkstk`, so the symbol doesn't need to exist in the GCC-built binary.

If we omitted the guard, GCC would assemble the `__chkstk` function into the initial binary. It wouldn't cause harm -- nothing would call it -- but it would be dead code. More importantly, if GCC's assembler syntax differed from TCC's even slightly, the build would break for no reason. The guard keeps GCC and TCC concerns separate.

## Fix 7: PE Import Resolution for In-Memory Execution

F6 rebuild works. F5 self-test works — `"int main(void) { return 42; }"` compiles and runs in memory, returns 42. Open `hello.c`, press F5:

```
!!!! X64 Exception Type - 06(#UD - Invalid Opcode)  CPU Apic ID - 00000000 !!!!
RIP  - 00000000FFC00066
```

Invalid Opcode, but the RIP address is `0xFFC00066` — that's in the OVMF firmware flash region, not in our code or the JIT buffer. The CPU jumped into firmware and hit garbage.

The self-test passed because `return 42` has no external function calls. `hello.c` calls `fb_print` and `fb_rect` — symbols registered via `tcc_add_symbol()`. Those calls go through the PE Import Address Table (IAT). Something is wrong with the IAT.

### How PE Imports Work in Memory

When TCC compiles code with `TCC_OUTPUT_MEMORY` and `TCC_TARGET_PE`, it builds a PE import table just like it would for a file on disk. For each imported symbol, `tccpe.c`'s `pe_build_imports` function writes an address into the IAT. The generated code calls imported functions indirectly: `call [rip+offset]` reads the function address from the IAT and jumps there.

For symbols added via `tcc_add_symbol()`, TCC stores the actual function address in the symbol's `st_value` field. During import building, this address should be written directly into the IAT — the function already exists in memory at a known address, no loading needed.

The import resolution code has three paths:

```c
/* address from tcc_add_symbol() */
ordinal = 0, v = imp_sym->st_value;

#if defined(TCC_IS_NATIVE) && defined(_WIN32)
if (pe->type == PE_RUN) {
    if (dllref) {
        /* resolve via LoadLibrary/GetProcAddress */
        v = (ADDR3264)GetProcAddress(...);
    }
    if (!v) tcc_error_noabort("could not resolve '%s'");
} else
#endif
if (ordinal) {
    v = ordinal | (ADDR3264)1 << (sizeof(ADDR3264)*8 - 1);
} else {
    v = pe->thunk->data_offset + rva_base;  /* <-- overwrites v */
    put_elf_str(pe->thunk, name);
}
```

The critical path is the `PE_RUN` block — guarded by `TCC_IS_NATIVE && _WIN32`. On Windows, this block fires for in-memory execution: it keeps the address from `tcc_add_symbol()` (the correct function pointer) and skips the import-name-building code below. The `} else` means the `if (ordinal)` block only runs for file output, not for `PE_RUN`.

On UEFI, `_WIN32` is not defined. The entire `#if` block compiles out. The code falls through to:

```c
v = pe->thunk->data_offset + rva_base;
```

This **overwrites** `v` — which held the real function address — with an RVA pointing to the import name string in the thunk section. The IAT gets this garbage value. When `hello.c` calls `fb_print`, the CPU follows the IAT entry to a nonsense address. `0xFFC00066` happens to be in OVMF firmware flash. Invalid Opcode.

### The Fix

The `PE_RUN` guard must activate for any native platform, not just Windows. The `LoadLibraryA`/`GetProcAddress` calls inside it are Windows-specific, but the outer flow control — "for PE_RUN, keep the address and skip import name building" — is platform-independent.

In `tccpe.c`:

```c
#if defined(TCC_IS_NATIVE)
                if (pe->type == PE_RUN) {
#ifdef _WIN32
                    if (dllref) {
                        if ( !dllref->handle )
                            dllref->handle = LoadLibraryA(dllref->name);
                        v = (ADDR3264)GetProcAddress(dllref->handle,
                                ordinal?(char*)0+ordinal:name);
                    }
#endif
                    if (!v)
                        tcc_error_noabort("could not resolve symbol '%s'", name);
                } else
#endif
```

Two changes: the outer `#if` drops `&& defined(_WIN32)`, and the `LoadLibraryA`/`GetProcAddress` block gets its own `#ifdef _WIN32` inside. Now on UEFI:

- `PE_RUN` mode: `v` keeps the function address from `tcc_add_symbol()`. The import name building is skipped. IAT entries contain correct pointers.
- File output mode: import names are built normally for the PE import directory.

On Windows, behavior is unchanged — the `LoadLibraryA` path still resolves DLL symbols for `PE_RUN`, and file output still builds import tables.

## The Native Test Harness

Iterating on these fixes through QEMU boot cycles is slow. Each cycle takes 15-20 seconds: rebuild the workstation with GCC, create a disk image, boot QEMU, wait for firmware, press F6, wait for TCC to compile, reboot, watch it crash. For the `__chkstk` debugging alone, we went through dozens of cycles.

To iterate faster, we built a native test harness. Compile `libtcc.c` as a regular Linux shared library, then write a small Linux program that uses the `libtcc` API to replicate exactly what the F6 rebuild handler does: create a TCC state, set the same options (`-nostdlib -nostdinc -Werror -Wl,-subsystem=efiapp -Wl,-e=efi_main`), compile the same source files, and write the PE output. Run it on the host machine in under a second.

The native test doesn't prove the PE binary boots -- you still need QEMU for that. But it catches compilation errors, link errors, and PE structure problems instantly. The workflow becomes: fix a TCC issue, run the native test (1 second), if it produces a PE file, boot it in QEMU (15 seconds). Most iterations end at the native test. Only the final verification needs QEMU.

## What We Have

The x86_64 self-hosting rebuild now works. Press F6 on x86_64, the workstation recompiles itself, reboots into the new binary. The same F6 already works on ARM64 (Chapter 16). Both architectures are fully self-hosting.

The seven fixes, in order:

| Fix | File | Problem |
|-----|------|---------|
| `TCC_TARGET_PE` | `config.h` | GOT-based globals, wrong calling convention |
| `pe_create_pdb` guard | `tccpe.c` | `system()` doesn't exist on UEFI |
| `tcc_add_systemdir` guard | `libtcc.c` | `GetSystemDirectoryA` doesn't exist on UEFI |
| `_WIN32` predefine guard | `tccpp.c` | Predefined `_WIN32` breaks F6 rebuild |
| LLP64 type sizes | `efi.h`, `shim.h` | `long` is 4 bytes under PE, corrupts 64-bit types and `jmp_buf` |
| `__chkstk` | `shim.c` | Large stack frames need custom frame setup |
| PE import resolution | `tccpe.c` | `PE_RUN` guard excluded UEFI, IAT got wrong addresses for F5 |

The first fix -- `TCC_TARGET_PE` -- is the keystone. Without it, TCC generates the wrong code for x86_64 UEFI. Fixes 2 through 4 are the collateral damage from enabling it: Windows-specific code that must be guarded out. Fix 5 is the data model mismatch that PE mode introduces. Fix 6 is the runtime support function that PE mode requires. Fix 7 is the subtlest -- the PE import resolution assumed that in-memory execution only happens on Windows, so on UEFI it silently wrote wrong addresses into the IAT, breaking every call to a registered symbol.

On ARM64, self-hosting required GOT relaxation -- a deep linker patch because ARM64's code generator always uses GOT-indirect addressing. On x86_64, `TCC_TARGET_PE` activates an existing, well-tested code path. The x86_64 fixes are smaller and more surgical: guard out Windows-isms, fix type sizes, provide one runtime function, fix one platform assumption. Different architecture, different path to the same result.

---

**Next:** [Chapter 17: Syntax Highlighting](../phase3_5/chapter-17-syntax-highlighting)
