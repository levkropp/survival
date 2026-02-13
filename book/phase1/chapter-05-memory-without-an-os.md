# Chapter 5: Memory Without an OS

## What Happens When You Call malloc()

On a normal desktop system, memory management is layered:

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

So how do we allocate memory?

## UEFI Memory Services

UEFI Boot Services provides two memory allocation functions:

### AllocatePool

```c
EFI_STATUS AllocatePool(
    EFI_MEMORY_TYPE PoolType,   // What kind of memory
    UINTN Size,                 // How many bytes
    VOID **Buffer               // Where to store the pointer
);
```

This is the UEFI equivalent of `malloc()`. You ask for a number of bytes, and it gives you a pointer to that much memory. The memory is guaranteed to be aligned to at least 8 bytes.

**`PoolType`** tells the firmware what you're using the memory for. This matters because UEFI tracks memory regions by type. The types we care about:

- `EfiLoaderData` — General-purpose data for a boot application. This is what we use.
- `EfiLoaderCode` — Executable code for a boot application.
- `EfiBootServicesData` — Data used by boot services themselves.

We always use `EfiLoaderData` because our allocations are for general data (buffers, strings, structures).

### FreePool

```c
EFI_STATUS FreePool(VOID *Buffer);
```

The counterpart to `AllocatePool`. Pass the pointer you got back and the memory is returned to the firmware's pool.

### AllocatePages (not used yet)

For large allocations, UEFI provides `AllocatePages` which allocates memory in 4 KB page granularity. We don't need this in Phase 1, but it becomes relevant when we need large contiguous buffers later.

## Our Memory Module

Our memory module is deliberately minimal. It's a thin wrapper around UEFI's allocator, plus a few utility functions that we'd normally get from the C standard library. Let's walk through every line.

### The Header: mem.h

```c
#ifndef MEM_H
#define MEM_H

#include "boot.h"
```

Include guards, and we pull in `boot.h` for the UEFI types and our `g_boot` global.

```c
void mem_init(void);
```

Initialization function. Currently empty, but having it in the API lets us add setup logic later (like pre-allocating an arena) without changing any calling code.

```c
void *mem_alloc(UINTN size);
void mem_free(void *ptr);
```

Our allocation interface. `mem_alloc` returns a pointer to `size` bytes of zeroed memory, or `NULL` on failure. `mem_free` releases memory allocated by `mem_alloc`.

Why wrap `AllocatePool` instead of calling it directly? Consistency and future-proofing. If we later want to switch to an arena allocator, a slab allocator, or add allocation tracking for debugging, we change `mem.c` and nothing else.

```c
void mem_set(void *dst, UINT8 val, UINTN size);
void mem_copy(void *dst, const void *src, UINTN size);
```

These replace `memset` and `memcpy` from the C standard library. Remember, we compiled with `-ffreestanding`, so the standard library doesn't exist. Any code that needs to fill or copy memory blocks must use these.

There's actually a subtle trap here. GCC sometimes **generates implicit calls** to `memcpy` or `memset`. For example, if you write:

```c
struct big_thing a = b;  // Struct copy — GCC may emit a memcpy call
char buffer[256] = {0};  // Array init — GCC may emit a memset call
```

With `-ffreestanding`, GCC is supposed to avoid this, but it doesn't always. If you get mysterious "undefined reference to memcpy" linker errors, that's why. Linking with `libgcc.a` (which we do) often resolves this. If not, you can provide your own `memcpy` and `memset` functions with exactly those names.

```c
UINTN str_len(const CHAR8 *s);
```

String length for 8-bit strings. `CHAR8` is UEFI's type for a single byte character. We'll need this when working with our ASCII text content (survival documentation, source code files, etc.).

```c
#endif
```

Close the include guard.

### The Implementation: mem.c

```c
#include "mem.h"

void mem_init(void) {
    /* Nothing to do — we use UEFI AllocatePool directly */
}
```

A placeholder. The comment explains why it's empty. This is intentional, not an oversight.

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

Let's trace through this carefully:

1. We declare `ptr` and initialize it to `NULL`. Always initialize pointers — an uninitialized pointer contains whatever garbage was in memory at that location.

2. We call `AllocatePool`. The firmware looks through its internal free list, finds a block of at least `size` bytes, marks it as used, and writes the address to `ptr` through the double-pointer `&ptr`.

3. We check the return status with `EFI_ERROR()`. This macro evaluates to true if the status indicates an error. Possible errors:
   - `EFI_OUT_OF_RESOURCES` — Not enough memory
   - `EFI_INVALID_PARAMETER` — Bad argument (shouldn't happen with correct code)

4. We **zero the memory** with `mem_set`. Unlike `calloc` in the standard library, `AllocatePool` does NOT guarantee that the returned memory is zeroed. It could contain leftover data from a previous allocation or from the firmware's own operations. Zeroing prevents bugs where code assumes freshly allocated memory is all zeros.

5. We return the pointer (or `NULL` if allocation failed).

```c
void mem_free(void *ptr) {
    if (ptr)
        g_boot.bs->FreePool(ptr);
}
```

We check for `NULL` before calling `FreePool`. Calling `FreePool(NULL)` is undefined behavior in UEFI — some implementations crash, some silently ignore it. The NULL check makes `mem_free` safe to call with any pointer, including NULL. This mirrors the behavior of standard `free()`.

```c
void mem_set(void *dst, UINT8 val, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    for (UINTN i = 0; i < size; i++)
        d[i] = val;
}
```

A byte-by-byte memory fill. We cast `dst` to `UINT8 *` (pointer to unsigned byte) so we can index individual bytes. The loop sets each byte to `val`.

This is deliberately simple. A production `memset` would use 64-bit writes for the bulk of the operation and only use byte writes for the first/last few bytes. But our version is correct, easy to understand, and fast enough for our needs. We're not filling gigabytes — our largest operations are clearing the framebuffer, which is a few megabytes at most.

```c
void mem_copy(void *dst, const void *src, UINTN size) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINTN i = 0; i < size; i++)
        d[i] = s[i];
}
```

Byte-by-byte memory copy. Note the `const` on `src` — we promise not to modify the source data.

**Important caveat:** this implementation doesn't handle overlapping regions correctly. If `dst` and `src` overlap and `dst` comes after `src`, we'll overwrite source data before we've read it. The standard library's `memmove` handles this by copying backward when needed. Our code only copies forward.

In practice, this matters in one place: scrolling the framebuffer. When we scroll, we copy framebuffer memory from a lower address (higher rows) to a higher address (lower rows). Since the destination is *before* the source in memory, our forward copy is correct. If we ever needed to copy in the other direction, we'd need to add a backward-copy path.

```c
UINTN str_len(const CHAR8 *s) {
    UINTN len = 0;
    while (s[len])
        len++;
    return len;
}
```

Count characters until we hit a zero byte (the null terminator). This is identical to the standard `strlen` function. The UEFI firmware uses null-terminated strings just like C.

## Memory Map: What's Where?

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

The exact addresses depend on the firmware. We never hardcode addresses — we always ask UEFI for pointers (through `AllocatePool` or protocol queries).

The framebuffer is at a memory-mapped I/O address, meaning it's not actually RAM — it's a region of the address space that's wired to the display hardware. When you write a value to a framebuffer address, the display controller reads it and sends the corresponding pixel color to the monitor.

## Why Not a More Sophisticated Allocator?

You might wonder why we use UEFI's allocator directly instead of building a proper heap with `malloc`/`free` semantics, free lists, coalescing, and so forth.

The answer is: we don't need one yet. Our Phase 1 application is simple:
- All major data structures (the boot state, the font) are global or stack-allocated
- We don't do dynamic allocation in a loop
- We don't have allocation patterns that would fragment memory

If we later need something more sophisticated — for example, for the text editor in Phase 4 — we can add an arena allocator or a simple free-list allocator on top of `mem_alloc`. The current interface is designed to be replaced without changing any calling code.

## Key Takeaways

- Without an OS, we use UEFI `AllocatePool`/`FreePool` for dynamic memory
- We must provide our own `mem_set` and `mem_copy` because the C standard library doesn't exist
- Always zero memory after allocation — `AllocatePool` doesn't guarantee zeroed memory
- Always check for NULL before freeing
- Our `mem_copy` only works correctly when regions don't overlap (or when `dst < src`)
- Start simple. A complex allocator can be added later when the need arises.

Next: the most visually exciting part — painting pixels on the screen.
