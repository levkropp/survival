---
layout: default
title: "Chapter 16: The Self-Hosting Rebuild"
parent: "Phase 3: The C Compiler"
grand_parent: "Part 1: The Bare-Metal Workstation"
nav_order: 4
---

# Chapter 16: The Self-Hosting Rebuild

## What Self-Hosting Means

A self-hosting system can rebuild itself from its own source code. The workstation carries its source on the FAT32 filesystem. It carries the TCC compiler in its binary. Press a key, and the workstation compiles its own source code into a new PE binary, writes it to disk, and reboots into the freshly built version.

This is the ultimate test of the system's completeness. If TCC can compile every `.c` file in the workstation — the framebuffer driver, the keyboard handler, the filesystem, the editor, the shim layer, TCC itself — then the system is truly self-contained. You could throw away the cross-compiler, the host machine, the entire development environment. The workstation can reproduce itself.

## The Rebuild Handler

The editor handles F5 for compile-and-run (Chapter 14). F6 triggers the rebuild. Add it to the status bar:

```c
msg = " F2:Save  F3:Select  F5:Run  F6:Rebuild  ESC:Exit";
```

And the key handler:

```c
case KEY_F6:
    handle_rebuild();
    break;
```

The rebuild handler is the most complex single function in the workstation. Here it is, piece by piece.

### Error Capture

First, we need to capture TCC's error messages. The same approach as Chapter 14's `tcc_error_handler`, but writing to both a buffer (for the summary) and the screen (for real-time feedback):

```c
static char s_rebuild_err[4096];
static int  s_rebuild_err_pos;

static void rebuild_error_handler(void *opaque, const char *msg) {
    (void)opaque;
    /* Print to screen immediately */
    fb_print("  ", COLOR_RED);
    fb_print(msg, COLOR_RED);
    fb_print("\n", COLOR_RED);
    /* Buffer for summary */
    int len = (int)strlen(msg);
    for (int i = 0; i < len && s_rebuild_err_pos < (int)sizeof(s_rebuild_err) - 2; i++)
        s_rebuild_err[s_rebuild_err_pos++] = msg[i];
    if (s_rebuild_err_pos < (int)sizeof(s_rebuild_err) - 1)
        s_rebuild_err[s_rebuild_err_pos++] = '\n';
    s_rebuild_err[s_rebuild_err_pos] = '\0';
}
```

Errors appear on screen as they happen AND accumulate in a buffer. This solves the problem where intermediate "Compiling..." messages scroll errors off the screen — we reprint the summary at the end.

### The Source List

```c
static void handle_rebuild(void) {
    /* Auto-save if modified */
    if (s_modified) {
        if (doc_save() != 0) {
            draw_info("Save failed - cannot rebuild");
            return;
        }
    }

    fb_clear(COLOR_BLACK);
    fb_print("\n", COLOR_CYAN);
    fb_print("  ========================================\n", COLOR_CYAN);
    fb_print("       REBUILD WORKSTATION\n", COLOR_CYAN);
    fb_print("  ========================================\n\n", COLOR_CYAN);

    s_rebuild_err_pos = 0;
    s_rebuild_err[0] = '\0';

    /* Source files to compile */
    static const char *sources[] = {
        "/src/main.c", "/src/fb.c", "/src/kbd.c", "/src/mem.c",
        "/src/font.c", "/src/fs.c", "/src/browse.c", "/src/edit.c",
        "/src/shim.c", "/src/tcc.c",
        "/src/disk.c", "/src/fat32.c",
        NULL
    };
```

Every `.c` file in the workstation. These paths are absolute on the FAT32 filesystem — `/src/main.c` maps to the `src/` directory on the SD card (or disk image). The `copy-sources` Makefile target (Chapter 15) puts them there at build time.

### TCC Configuration

```c
    /* Output path */
#ifdef __aarch64__
    const char *out_path = "/EFI/BOOT/BOOTAA64.EFI";
#else
    const char *out_path = "/EFI/BOOT/BOOTX64.EFI";
#endif

    /* Create TCC state for PE/COFF output */
    TCCState *tcc = tcc_new();
    if (!tcc) {
        fb_print("  Failed to create TCC context\n", COLOR_RED);
        goto wait;
    }

    tcc_set_error_func(tcc, NULL, rebuild_error_handler);
    tcc_set_options(tcc, "-nostdlib -nostdinc -Werror"
                        " -Wl,-subsystem=efiapp -Wl,-e=efi_main");
    tcc_set_output_type(tcc, TCC_OUTPUT_DLL);

    /* Include paths: our stub headers, then source dir */
    tcc_add_include_path(tcc, "/src/tcc-headers");
    tcc_add_include_path(tcc, "/src");
    tcc_add_include_path(tcc, "/tools/tinycc");
```

Three critical options:

**`-Werror`** treats warnings as errors. Without this, TCC might warn about something suspicious (incompatible pointer types, implicit declarations) and continue compiling. The resulting binary boots into a Synchronous Exception or UEFI shell. With `-Werror`, any warning aborts the build before writing a broken binary. Better to see a clear error message than to flash a bad binary.

**`-Wl,-subsystem=efiapp -Wl,-e=efi_main`** configures the PE output for UEFI (Chapter 15).

**`TCC_OUTPUT_DLL`** produces a PE with relocations intact (Chapter 15).

### Compilation Loop

```c
    /* Compile each workstation source file */
    for (int i = 0; sources[i]; i++) {
        fb_print("  Compiling ", COLOR_WHITE);
        fb_print(sources[i], COLOR_YELLOW);
        fb_print("...\n", COLOR_WHITE);

        if (tcc_add_file(tcc, sources[i]) < 0) {
            fb_print("\n  BUILD FAILED\n", COLOR_RED);
            tcc_delete(tcc);
            goto wait;
        }
    }
```

Each file is compiled by `tcc_add_file`. The user sees real-time progress — file names appear in yellow as they're compiled. If any file fails (syntax error, type error, or now with `-Werror`, any warning), the build aborts immediately.

### The TCC Library

The workstation includes TCC itself. TCC must compile TCC:

```c
    /* Compile TCC library (unity build) -- suppress warnings for TCC source */
    tcc_set_options(tcc, "-w");
    tcc_define_symbol(tcc, "__UEFI__", "1");

    fb_print("  Compiling ", COLOR_WHITE);
    fb_print("/tools/tinycc/libtcc.c", COLOR_YELLOW);
    fb_print("...\n", COLOR_WHITE);
    if (tcc_add_file(tcc, "/tools/tinycc/libtcc.c") < 0) {
        fb_print("\n  BUILD FAILED (TCC library)\n", COLOR_RED);
        tcc_delete(tcc);
        goto wait;
    }
```

We switch to `-w` (suppress all warnings) for TCC's own source code. TCC's codebase triggers hundreds of warnings under strict compilation — intentional casts, implicit fallthrough, unused parameters. These are TCC's business, not ours. The `__UEFI__` define activates our guards in `tccpe.c`.

The unity build means `libtcc.c` includes ALL of TCC — preprocessor, parser, code generator, assembler, PE linker. One `tcc_add_file` call compiles roughly 60,000 lines of C. This is the heaviest moment of the rebuild, taking several seconds even on fast hardware.

### setjmp/longjmp

```c
    /* Compile setjmp/longjmp */
#ifdef __aarch64__
    fb_print("  Compiling /src/setjmp_aarch64.c...\n", COLOR_WHITE);
    if (tcc_add_file(tcc, "/src/setjmp_aarch64.c") < 0) {
#else
    fb_print("  Compiling /src/setjmp_x86_64.S...\n", COLOR_WHITE);
    if (tcc_add_file(tcc, "/src/setjmp_x86_64.S") < 0) {
#endif
        fb_print("\n  BUILD FAILED (setjmp)\n", COLOR_RED);
        tcc_delete(tcc);
        goto wait;
    }
```

ARM64 uses the pre-assembled opcode C file (Chapter 15). x86_64 uses the actual assembly — TCC's x86 assembler handles it fine.

### Linking and Output

```c
    fb_print("\n  Linking...\n", COLOR_WHITE);

    /* Generate PE output */
    if (tcc_output_file(tcc, out_path) < 0) {
        fb_print("  Output failed\n", COLOR_RED);
        tcc_delete(tcc);
        goto wait;
    }

    tcc_delete(tcc);

    fb_print("\n", COLOR_GREEN);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("    BUILD COMPLETE!\n", COLOR_GREEN);
    fb_print("  ========================================\n", COLOR_GREEN);
    fb_print("\n  Written to: ", COLOR_WHITE);
    fb_print(out_path, COLOR_YELLOW);
    fb_print("\n\n  Press R to reboot now, any other key for editor.\n",
             COLOR_YELLOW);
```

`tcc_output_file` invokes the PE linker — resolving symbols, applying relocations (including our GOT relaxation from Chapter 15), and writing the PE/COFF binary to the FAT32 filesystem. The file overwrites `BOOTAA64.EFI` or `BOOTX64.EFI` — the same file the workstation booted from.

### Error Summary and Reboot

```c
    {
        struct key_event ev;
        kbd_wait(&ev);
        if (ev.code == 'R' || ev.code == 'r') {
            g_boot.rs->ResetSystem(EfiResetCold, 0, 0, NULL);
        }
    }
    draw_all();
    return;

wait:
    if (s_rebuild_err_pos > 0) {
        fb_print("\n  ---- Error Summary ----\n", COLOR_RED);
        fb_print(s_rebuild_err, COLOR_RED);
    }
    fb_print("\n  Press any key to return to editor...\n", COLOR_DGRAY);
    {
        struct key_event ev;
        kbd_wait(&ev);
    }
    draw_all();
}
```

On success, pressing R triggers `ResetSystem(EfiResetCold, ...)` — a hard reboot. The firmware re-reads the boot file from disk. If the rebuild wrote a valid PE, the new workstation boots.

On failure, the error summary reprints all accumulated errors at the bottom of the screen. During compilation, errors might scroll past under "Compiling..." messages. The summary ensures they're always visible.

## The Moment

Boot the workstation. Open the file browser. Navigate to `src/browse.c`. Open it in the editor. Find the title string — "SURVIVAL FILE BROWSER" — and change it. Press F6.

```
  ========================================
       REBUILD WORKSTATION
  ========================================

  Compiling /src/main.c...
  Compiling /src/fb.c...
  Compiling /src/kbd.c...
  Compiling /src/mem.c...
  Compiling /src/font.c...
  Compiling /src/fs.c...
  Compiling /src/browse.c...
  Compiling /src/edit.c...
  Compiling /src/shim.c...
  Compiling /src/tcc.c...
  Compiling /src/disk.c...
  Compiling /src/fat32.c...
  Compiling /tools/tinycc/libtcc.c...
  Compiling /src/setjmp_aarch64.c...

  Linking...

  ========================================
    BUILD COMPLETE!
  ========================================

  Written to: /EFI/BOOT/BOOTAA64.EFI

  Press R to reboot now, any other key for editor.
```

Press R. The screen goes black. The firmware reloads. The workstation boots.

The title has changed.

The change you made — with this keyboard, on this screen, in this editor — is now running. The binary that booted was compiled by the binary that came before it, which was compiled by GCC on a Linux host. But from this point forward, the chain is self-sustaining. The workstation can modify and rebuild itself indefinitely.

If a bad rebuild writes a broken binary, the worst case is reflashing the SD card from the host machine — the same way you flashed it initially. The `-Werror` flag catches most problems before they reach disk, and the error summary makes it clear what went wrong.

## What We Have

Phase 3 made the workstation self-hosting:

1. **TCC PE output** — fourteen patches to TCC enable it to produce UEFI PE/COFF binaries (Chapter 15). The GOT relaxation in `arm64-link.c` was the deepest change. The pre-assembled setjmp opcodes were the most creative workaround.

2. **The F6 rebuild** — press F6 in the editor to recompile the entire workstation from source using the embedded TCC compiler. The new binary writes to disk and boots on reboot.

The binary sizes:

```
aarch64: 689K
x86_64:  662K
```

Both well under the 4MB budget, even carrying all source code and TCC library files on the FAT32 filesystem.

The workstation is no longer dependent on its creator. It can evolve on its own.

---

**Next:** [Chapter 18: x86_64 Self-Hosting](chapter-18-x86-64-self-hosting)
