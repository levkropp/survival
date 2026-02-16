---
layout: default
title: "Chapter 14: Write, Run, Repeat"
parent: "Phase 3: The C Compiler"
nav_order: 2
---

# Chapter 14: Write, Run, Repeat

## The Runtime Wrapper

Chapter 13 proved that TCC can compile C to machine code inside our UEFI application. But the test was hardcoded — a string literal baked into the binary. We need a clean interface that the editor can call with arbitrary user code.

Create `src/tcc.h`:

```c
struct tcc_result {
    int  success;
    char error_msg[2048];
    int  exit_code;
};

struct tcc_result tcc_run_source(const char *source, const char *filename);
```

The caller passes a source string and a filename (for error messages). The function returns success/failure, any compile errors, and the exit code from `main()`. Simple enough to call from one line of code.

Now `src/tcc.c`. The error handler captures TCC's diagnostic messages:

```c
static char *s_errbuf;
static int   s_errbuf_pos;
static int   s_errbuf_size;

static void tcc_error_handler(void *opaque, const char *msg) {
    (void)opaque;
    if (!s_errbuf) return;
    int len = (int)strlen(msg);
    for (int i = 0; i < len && s_errbuf_pos < s_errbuf_size - 2; i++)
        s_errbuf[s_errbuf_pos++] = msg[i];
    if (s_errbuf_pos < s_errbuf_size - 1)
        s_errbuf[s_errbuf_pos++] = '\n';
    s_errbuf[s_errbuf_pos] = '\0';
}
```

TCC calls this function whenever it encounters an error. The messages accumulate in the result buffer, separated by newlines. When the user sees "line 7: expected ';'", this is the path it traveled.

## Registering the Workstation API

User programs need to call our functions — draw pixels, read keys, allocate memory. TCC's `tcc_add_symbol()` makes this possible:

```c
static void register_api(TCCState *s) {
    /* Framebuffer */
    tcc_add_symbol(s, "fb_pixel",  fb_pixel);
    tcc_add_symbol(s, "fb_rect",   fb_rect);
    tcc_add_symbol(s, "fb_clear",  fb_clear);
    tcc_add_symbol(s, "fb_char",   fb_char);
    tcc_add_symbol(s, "fb_string", fb_string);
    tcc_add_symbol(s, "fb_scroll", fb_scroll);
    tcc_add_symbol(s, "fb_print",  fb_print);

    /* Keyboard */
    tcc_add_symbol(s, "kbd_poll", kbd_poll);
    tcc_add_symbol(s, "kbd_wait", kbd_wait);

    /* Memory */
    tcc_add_symbol(s, "mem_alloc", mem_alloc);
    tcc_add_symbol(s, "mem_free",  mem_free);
    tcc_add_symbol(s, "mem_set",   mem_set);
    tcc_add_symbol(s, "mem_copy",  mem_copy);

    /* Filesystem */
    tcc_add_symbol(s, "fs_readfile",  fs_readfile);
    tcc_add_symbol(s, "fs_writefile", fs_writefile);
    tcc_add_symbol(s, "fs_readdir",   fs_readdir);

    /* Global state */
    tcc_add_symbol(s, "g_boot", &g_boot);

    /* Libc basics */
    tcc_add_symbol(s, "printf",   printf);
    tcc_add_symbol(s, "snprintf", snprintf);
    tcc_add_symbol(s, "strlen",   strlen);
    tcc_add_symbol(s, "strcmp",   strcmp);
    tcc_add_symbol(s, "memcpy",   memcpy);
    tcc_add_symbol(s, "memset",   memset);
    tcc_add_symbol(s, "malloc",   malloc);
    tcc_add_symbol(s, "free",     free);
    tcc_add_symbol(s, "puts",     puts);
}
```

Each call tells TCC: "when compiled code references this name, use this address." The compiler resolves these during relocation. From the user's perspective, they just call `fb_rect(100, 200, 50, 50, COLOR_RED)` and a red rectangle appears.

## The Compile-and-Run Pipeline

The main function puts it all together:

```c
struct tcc_result tcc_run_source(const char *source, const char *filename) {
    struct tcc_result result;
    mem_set(&result, 0, sizeof(result));

    s_errbuf = result.error_msg;
    s_errbuf_pos = 0;
    s_errbuf_size = (int)sizeof(result.error_msg);

    TCCState *tcc = tcc_new();
    if (!tcc) {
        strcpy(result.error_msg, "Failed to create TCC context");
        return result;
    }

    tcc_set_error_func(tcc, NULL, tcc_error_handler);
    tcc_set_options(tcc, "-nostdlib -nostdinc");
    tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);
    tcc_add_include_path(tcc, "/include");
    register_api(tcc);
```

Three options are critical here. `-nostdlib` prevents TCC from searching for `libc` and `libtcc1.a` during relocation — they don't exist. `-nostdinc` prevents TCC from searching default system include paths. And `TCC_OUTPUT_MEMORY` tells TCC to generate code in memory rather than writing an object file.

The include path `/include` points to the FAT32 filesystem on the SD card (or disk image). User-facing headers like `survival.h` live there.

Next, we prepend a `#line` directive so error messages reference the original filename:

```c
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "#line 1 \"%s\"\n",
             filename ? filename : "input.c");

    int plen = (int)strlen(prefix);
    int slen = (int)strlen(source);
    char *full = (char *)malloc((size_t)(plen + slen + 1));
    memcpy(full, prefix, (size_t)plen);
    memcpy(full + plen, source, (size_t)slen);
    full[plen + slen] = '\0';
```

Then compile, relocate, and extract the entry point:

```c
    if (tcc_compile_string(tcc, full) < 0) {
        free(full);
        tcc_delete(tcc);
        return result;
    }
    free(full);

    if (tcc_relocate(tcc) < 0) {
        tcc_delete(tcc);
        return result;
    }

    int (*prog_main)(void) = (int (*)(void))tcc_get_symbol(tcc, "main");
    if (!prog_main) {
        strcpy(result.error_msg, "No main() function found");
        tcc_delete(tcc);
        return result;
    }
```

`tcc_compile_string` runs the preprocessor, parser, and code generator. `tcc_relocate` resolves all symbol references (including our registered API functions) and writes the final machine code into an allocated buffer. `tcc_get_symbol` returns the address of `main` as a void pointer, which we cast to a function pointer.

## Handling exit()

User programs might call `exit()`. In a normal OS, this terminates the process. We can't do that — there is no process. If `exit()` called UEFI's `ResetSystem`, it would reboot the machine. If it just returned, the call stack would be corrupt.

The solution is `setjmp`/`longjmp`:

```c
    shim_exit_active = 1;
    shim_exit_code = 0;
    int jmpval = setjmp(shim_exit_jmpbuf);

    if (jmpval == 0) {
        result.exit_code = prog_main();
        result.success = 1;
    } else {
        result.exit_code = shim_exit_code;
        result.success = 1;
    }

    shim_exit_active = 0;
    tcc_delete(tcc);
    return result;
```

Before calling the user program, we `setjmp` — saving the CPU state. If the program returns normally, we take the `jmpval == 0` path and record the return value. If it calls `exit()`, our shim's `exit()` implementation does `longjmp(shim_exit_jmpbuf, 1)`, which teleports back to the `setjmp` call site with `jmpval == 1`. Either way, we end up at `tcc_delete` with a valid result.

## The User Header

User programs need declarations for our API functions. Create `src/user-headers/survival.h`:

```c
#ifndef SURVIVAL_H
#define SURVIVAL_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long      size_t;

#define NULL ((void *)0)

/* Colors (ARGB32) */
#define COLOR_BLACK   0xFF000000
#define COLOR_WHITE   0xFFFFFFFF
#define COLOR_RED     0xFFFF0000
#define COLOR_GREEN   0xFF00FF00
#define COLOR_BLUE    0xFF0000FF
#define COLOR_YELLOW  0xFFFFFF00
#define COLOR_CYAN    0xFF00FFFF

/* Framebuffer */
void fb_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);
void fb_print(const char *s, uint32_t color);

/* Keyboard */
struct key_event { uint16_t code; uint16_t scancode; };
int  kbd_poll(struct key_event *ev);
void kbd_wait(struct key_event *ev);

/* Memory */
void *malloc(size_t size);
void  free(void *ptr);
int   printf(const char *fmt, ...);

#endif
```

This header gets copied to `/include/` on the FAT32 image by the Makefile's `copy-headers` target. When a user program writes `#include <survival.h>`, TCC finds it via the include path we set up.

## Wiring F5 to the Editor

The editor from Chapter 12 handles F2 for save and ESC for exit. We add F5 for compile-and-run.

First, check if the current file is C source:

```c
static int is_c_file(void) {
    int len = (int)str_len((CHAR8 *)s_filename);
    return len >= 2
        && s_filename[len - 2] == '.'
        && s_filename[len - 1] == 'c';
}
```

Then the handler:

```c
static void handle_compile_run(void) {
    if (!is_c_file()) {
        draw_info("Not a .c file");
        return;
    }

    /* Auto-save before compiling */
    if (s_modified) {
        if (doc_save() != 0) {
            draw_info("Save failed — cannot compile");
            return;
        }
    }

    /* Serialize document to source string */
    UINTN src_size = 0;
    char *source = doc_serialize(&src_size);
    if (!source) {
        draw_info("Out of memory");
        return;
    }

    /* Clear screen and show compile message */
    fb_clear(COLOR_BLACK);
    fb_print("  Compiling ", COLOR_CYAN);
    fb_print(s_filename, COLOR_CYAN);
    fb_print("...\n\n", COLOR_CYAN);

    /* Compile and run */
    struct tcc_result r = tcc_run_source(source, s_filename);
    mem_free(source);

    /* Display result */
    fb_print("\n", COLOR_WHITE);
    if (r.success) {
        fb_print("  --- Program exited with code ", COLOR_GRAY);
        char num[16];
        int_to_str(r.exit_code, num);
        fb_print(num, r.exit_code == 0 ? COLOR_GREEN : COLOR_YELLOW);
        fb_print(" ---\n", COLOR_GRAY);
    } else {
        fb_print("  --- Compile Error ---\n", COLOR_RED);
        if (r.error_msg[0])
            fb_print(r.error_msg, COLOR_RED);
    }

    fb_print("\n  Press any key to return to editor...\n", COLOR_DGRAY);

    struct key_event ev;
    kbd_wait(&ev);
    draw_all();
}
```

The flow: auto-save the file, serialize the document buffer to a single string, clear the screen, compile-and-run, show the output, wait for a keypress, then redraw the editor. The user's program gets the full screen while it runs — `fb_print` output appears as if it were a terminal. When the program finishes, pressing any key brings back the editor with the cursor exactly where they left it.

Add the case to the main loop:

```c
case KEY_F5:
    handle_compile_run();
    break;
```

And update the status bar:

```c
msg = " F2:Save  F5:Run  ESC:Exit";
```

## The Hello World Test

Place a test file in `src/user-headers/hello.c` (the Makefile copies it to the FAT32 image):

```c
#include <survival.h>

int main(void) {
    fb_print("Hello from TinyCC!\n", COLOR_GREEN);
    fb_print("This was compiled in memory.\n", COLOR_WHITE);

    fb_rect(100, 200, 200, 100, COLOR_BLUE);
    fb_rect(110, 210, 180, 80, COLOR_CYAN);

    fb_print("\nDrew a rectangle!\n", COLOR_YELLOW);
    return 0;
}
```

Boot the workstation. Open the file browser. Navigate to `hello.c`. Open it in the editor. Press F5.

The screen clears. "Compiling hello.c..." appears briefly. Then:

```
Hello from TinyCC!
This was compiled in memory.

Drew a rectangle!

  --- Program exited with code 0 ---

  Press any key to return to editor...
```

And a blue-and-cyan rectangle on the screen. The program was written on this machine, compiled on this machine, and executed on this machine. No operating system involved.

## What Just Happened

Let's trace the full path of a keystroke-to-execution:

1. You type C code using the keyboard driver from Chapter 7
2. Characters are stored in line buffers from Chapter 12's editor
3. F5 serializes the buffer and passes it to TCC
4. TCC preprocesses the source, parsing `#include <survival.h>` from the FAT32 filesystem (Chapter 9)
5. TCC's parser builds an AST, the code generator emits ARM64 instructions into an allocated buffer
6. That buffer is in `EfiLoaderCode` memory, so the CPU can execute it
7. We call the compiled `main()` through a function pointer
8. The user's code calls `fb_rect()`, which writes pixels to the framebuffer from Chapter 6
9. The program returns, we display the exit code, and the editor redraws

Eight chapters of infrastructure converged into a single keypress.

## The Binary

```
$ ls -lh build/survival.efi
-rwxrwxr-x 1 min min 632K ... build/survival.efi
```

632 kilobytes. That's a bootloader, framebuffer driver, keyboard input, filesystem, file browser, text editor, and a C compiler that generates and executes ARM64 machine code in memory. It fits on a floppy disk with room to spare.

The workstation is no longer inert. It can create.
