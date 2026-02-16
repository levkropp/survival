---
layout: default
title: "Chapter 5: Memory Without an OS"
parent: "Phase 1: Boot & Input"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 5
---

# Chapter 5: Memory Without an OS

## The Missing Floor

At the end of Chapter 4, we have a working UEFI application — it boots, echoes keystrokes, and shuts down. But it draws text using UEFI's built-in console, which gives us no control over colors, fonts, or layout. To build a real workstation UI, we need to draw our own pixels on a framebuffer.

A framebuffer is just a chunk of memory where each address corresponds to a pixel. To draw, we write values to memory. To scroll, we copy blocks of memory around. To clear the screen, we fill a region of memory with a single value.

So before we can draw anything, we need to be able to do three things: allocate memory, fill memory, and copy memory.

On a normal system, the C standard library gives you `malloc`, `memset`, and `memcpy`. But we compiled with `-ffreestanding` — the standard library doesn't exist. We need to build these from scratch.

## Filling Memory

The simplest memory operation is filling a region with a single byte value. We'll need this constantly — zeroing buffers, clearing screen regions, padding strings with spaces. Let's write it:

```c
void mem_set(void *dst, UINT8 val, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    for (UINTN i = 0; i < size; i++)
        d[i] = val;
}
```

`dst` is a `void *` — a pointer to "anything." C doesn't let you do arithmetic on void pointers because it doesn't know how big "anything" is. So we cast it to `UINT8 *` — a pointer to unsigned bytes — which lets us index individual bytes with `d[i]`.

`val` is the byte to fill with. `UINT8` is an unsigned 8-bit integer (0-255). For zeroing, you pass `0`. For filling with spaces, you pass `' '` (which is `0x20`).

`size` is how many bytes to fill. `UINTN` is UEFI's "unsigned integer, pointer-sized" type — 64 bits on our ARM64 system, big enough to address any amount of memory.

The loop is simple: set each byte, one at a time. A production `memset` would use 64-bit writes for speed and only byte-write the edges. But our version is correct and fast enough. Our biggest fill operation will be clearing a framebuffer — a few megabytes. On a 1.5 GHz Cortex-A53, that takes milliseconds.

## Copying Memory

The other primitive operation is copying bytes from one place to another:

```c
void mem_copy(void *dst, const void *src, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINTN i = 0; i < size; i++)
        d[i] = s[i];
}
```

Same pattern: cast to byte pointers, loop and copy. The `const` on `src` is a promise that we won't modify the source data.

There's a subtle trap here. If `dst` and `src` overlap and `dst` comes *after* `src`, we'll overwrite source bytes before we've copied them. The standard library has `memmove` that handles this by copying backward when needed.

We only copy forward. In practice, this is fine — the one place where overlap matters is framebuffer scrolling, where we copy screen rows upward (lower addresses to higher addresses). Since the destination comes *before* the source in memory, our forward copy is correct for that case. If we ever needed backward copying, we'd add it then.

## Allocating Memory

Now for the interesting part. We have `mem_set` and `mem_copy`, but where do we *get* memory from?

On a normal system:

```
Your code calls malloc(1024)
       ↓
C library (glibc) manages a heap
  - Tracks free/used blocks
  - Splits and coalesces regions
  - Calls the OS when it needs more
       ↓
OS kernel (Linux) manages virtual memory
  - Each process gets its own address space
  - Pages mapped to physical RAM on demand
  - Swaps to disk when RAM is full
       ↓
Physical RAM chips
```

We have none of this. No C library. No kernel. No virtual memory. No swap. We have 2 GB of physical RAM and a firmware that knows about it.

UEFI Boot Services provides `AllocatePool` — the firmware's equivalent of `malloc`:

```c
EFI_STATUS AllocatePool(
    EFI_MEMORY_TYPE PoolType,   // What kind of memory
    UINTN Size,                 // How many bytes
    VOID **Buffer               // Where to store the pointer
);
```

You ask for a number of bytes, and the firmware finds a free block and gives you a pointer. The `PoolType` tells the firmware what you're using it for — we always use `EfiLoaderData`, which means "general-purpose data for a boot application."

Let's wrap this in a function that's easier to use:

```c
void *mem_alloc(UINTN size) {
    void *ptr = NULL;
    EFI_STATUS status = g_boot.bs->AllocatePool(EfiLoaderData, size, &ptr);
    if (EFI_ERROR(status))
        return NULL;
    mem_set(ptr, 0, size);
    return ptr;
}
```

Let's trace through this carefully.

First, we declare `ptr` and initialize it to `NULL`. Always initialize pointers — an uninitialized pointer contains whatever garbage was in memory at that location.

Then we call `AllocatePool`. The firmware looks through its internal free list, finds a block of at least `size` bytes, marks it as used, and writes the address into `ptr` through the double-pointer `&ptr`. That double-pointer is important — `AllocatePool` needs a *pointer to our pointer* so it can modify our pointer.

We check the return status with `EFI_ERROR()`, a macro that evaluates to true if something went wrong. If allocation fails (most likely `EFI_OUT_OF_RESOURCES` — no memory left), we return `NULL`. The caller must check for this.

Then — and this is crucial — we **zero the memory** with `mem_set`. Unlike the standard library's `calloc`, `AllocatePool` does NOT guarantee zeroed memory. The returned block could contain leftover data from a previous allocation, from the firmware's internal operations, or from whatever happened to be in RAM at boot. If your code assumes freshly allocated memory is all zeros (and it will), you need this.

Finally, we return the pointer.

## Freeing Memory

The counterpart to `AllocatePool` is `FreePool`:

```c
void mem_free(void *ptr) {
    if (ptr)
        g_boot.bs->FreePool(ptr);
}
```

We check for `NULL` before calling `FreePool`. In UEFI, calling `FreePool(NULL)` is undefined behavior — some implementations crash, some silently ignore it. The `NULL` check makes `mem_free` safe to call with any pointer, mirroring how the standard library's `free(NULL)` is defined to be a no-op.

## String Length

One more utility. We'll need to know the length of strings when rendering text — to center text, to right-align numbers, to truncate filenames that don't fit:

```c
UINTN str_len(const CHAR8 *s) {
    UINTN len = 0;
    while (s[len])
        len++;
    return len;
}
```

`CHAR8` is UEFI's type for a single-byte character. This function counts characters until it hits a zero byte (the null terminator) — identical to the standard `strlen`. We use `CHAR8` instead of `char` to be explicit about the type width, since UEFI code deals with both 8-bit and 16-bit strings.

## The Header

Now let's package these functions into a module. Create `src/mem.h`:

```c
#ifndef MEM_H
#define MEM_H

#include "boot.h"

void mem_init(void);

void *mem_alloc(UINTN size);
void mem_free(void *ptr);

void mem_set(void *dst, UINT8 val, UINTN size);
void mem_copy(void *dst, const void *src, UINTN size);
UINTN str_len(const CHAR8 *s);

#endif /* MEM_H */
```

We include `boot.h` so that any file including `mem.h` gets access to the UEFI types (`UINTN`, `UINT8`, `CHAR8`) and to `g_boot`.

There's one function we haven't discussed yet: `mem_init`. Here it is:

```c
void mem_init(void) {
    /* Nothing to do — we use UEFI AllocatePool directly */
}
```

An empty function. Why declare it? Because the pattern of having an `_init` function for each module gives us a clean startup sequence in `main.c`: `mem_init()`, `fb_init()`, `fs_init()`, etc. If we later want to pre-allocate an arena or set up allocation tracking for debugging, we add that code here — without changing any calling code.

## The Implementation

Create `src/mem.c` with everything we've built:

```c
#include "mem.h"

void mem_init(void) {
    /* Nothing to do — we use UEFI AllocatePool directly */
}

void *mem_alloc(UINTN size) {
    void *ptr = NULL;
    EFI_STATUS status = g_boot.bs->AllocatePool(EfiLoaderData, size, &ptr);
    if (EFI_ERROR(status))
        return NULL;
    mem_set(ptr, 0, size);
    return ptr;
}

void mem_free(void *ptr) {
    if (ptr)
        g_boot.bs->FreePool(ptr);
}

void mem_set(void *dst, UINT8 val, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    for (UINTN i = 0; i < size; i++)
        d[i] = val;
}

void mem_copy(void *dst, const void *src, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINTN i = 0; i < size; i++)
        d[i] = s[i];
}

UINTN str_len(const CHAR8 *s) {
    UINTN len = 0;
    while (s[len])
        len++;
    return len;
}
```

Six functions, 30 lines. Every byte of this code exists because something in our system needs it:
- `mem_set` — zeroing allocations, clearing framebuffer regions, padding display lines
- `mem_copy` — scrolling the framebuffer, copying pixel data
- `mem_alloc` — allocating buffers for file data, string building
- `mem_free` — releasing those buffers
- `str_len` — calculating text positions for rendering

## A Note on GCC and -ffreestanding

There's a subtle trap with `-ffreestanding` that's worth knowing about. Even with that flag, GCC sometimes **generates implicit calls** to `memcpy` or `memset`. For example:

```c
struct big_thing a = b;  // Struct copy — GCC may emit a memcpy call
char buffer[256] = {0};  // Array init — GCC may emit a memset call
```

With `-ffreestanding`, GCC is supposed to avoid this, but it doesn't always. If you get mysterious "undefined reference to memcpy" linker errors, that's why. Linking with `libgcc.a` (which we do in our Makefile) usually resolves this. If not, you'd need to provide `memcpy` and `memset` functions with exactly those names.

## Wiring Into main.c

We add `mem_init()` to our startup sequence. In `src/main.c`:

```c
#include "boot.h"
#include "mem.h"

struct boot_state g_boot;
```

And at the top of `efi_main`, after populating `g_boot`:

```c
    g_boot.image_handle = image_handle;
    g_boot.st = st;
    g_boot.bs = st->BootServices;
    g_boot.rs = st->RuntimeServices;

    g_boot.bs->SetWatchdogTimer(0, 0, 0, NULL);

    mem_init();
```

Right now `mem_init` does nothing, but the call establishes the pattern. When we add `fb_init()` in the next chapter, the startup sequence will be clear:

```c
    mem_init();     // Memory first — everything else may allocate
    fb_init();      // Framebuffer second — needs memory for buffers
```

Memory goes first because everything else might need to allocate.

## Memory Map

When our application runs, memory is organized by UEFI roughly like this:

```
Address Space (simplified)
─────────────────────────────────
0x0000_0000_0000_0000   ← Bottom of RAM
   ...
   UEFI firmware code and data
   ...
0x0000_0000_4000_0000   ← Somewhere in the middle
   ...
   Our loaded EFI application (~64 KB)
   ...
   AllocatePool returns addresses here
   ...
0x0000_0000_8000_0000   ← Top of 2 GB RAM
─────────────────────────────────
0x0000_00XX_XXXX_XXXX   ← Framebuffer (memory-mapped I/O)
   Writing here changes pixels on screen
─────────────────────────────────
```

The exact addresses depend on the firmware. We never hardcode addresses — we always ask UEFI for pointers through `AllocatePool` or protocol queries.

The framebuffer address is special: it's not actually RAM. It's a region of the address space wired to the display controller. When you write a value there, the display hardware reads it and sends the corresponding pixel color to the monitor. We'll explore this in the next chapter.

## Why Not a More Sophisticated Allocator?

You might wonder why we don't build a proper heap with free lists, coalescing, and splitting. The answer: we don't need one yet.

Our application is simple right now. All major data structures are global or stack-allocated. We don't do dynamic allocation in tight loops. We don't have allocation patterns that would fragment memory.

If we later need something more sophisticated — an arena allocator for the text editor, a slab allocator for frequently-allocated structures — we change `mem_alloc` and `mem_free` in `mem.c` and nothing else. The rest of the codebase just calls `mem_alloc(size)` and doesn't care what's behind it.

## What We Have

Three files now:

```
src/boot.h   — Global state structure and UEFI includes
src/mem.h    — Memory allocation and utility functions
src/mem.c    — Implementation: 6 functions, 30 lines
src/main.c   — Entry point, console I/O loop, shutdown
```

We still can't do anything visible with this — the application behaves exactly the same as before. But we've laid the foundation for everything that follows. The framebuffer driver needs `mem_set` to clear the screen, `mem_copy` to scroll, and `mem_alloc` to create buffers. The file browser will need `mem_alloc` to load file contents. Every module from here on will `#include "mem.h"`.

Next, we put memory to work — painting pixels on the screen.

---

**Next:** [Chapter 6: Painting Pixels](chapter-06-painting-pixels)
