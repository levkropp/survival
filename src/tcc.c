/*
 * tcc.c — TCC runtime wrapper for Survival Workstation
 *
 * Compiles C source to machine code in memory and executes it.
 * Provides workstation API symbols (fb_*, kbd_*, fs_*, mem_*)
 * to user programs via tcc_add_symbol().
 */

#include "boot.h"
#include "fb.h"
#include "kbd.h"
#include "mem.h"
#include "fs.h"
#include "shim.h"
#include "tcc.h"

/* TCC's public API */
#include "libtcc.h"

/* ---- Error handler ---- */

static char *s_errbuf;
static int   s_errbuf_pos;
static int   s_errbuf_size;

static void tcc_error_handler(void *opaque, const char *msg) {
    (void)opaque;
    if (!s_errbuf) return;

    int len = (int)strlen(msg);
    for (int i = 0; i < len && s_errbuf_pos < s_errbuf_size - 2; i++) {
        s_errbuf[s_errbuf_pos++] = msg[i];
    }
    /* Add newline */
    if (s_errbuf_pos < s_errbuf_size - 1) {
        s_errbuf[s_errbuf_pos++] = '\n';
    }
    s_errbuf[s_errbuf_pos] = '\0';
}

/* ---- Register workstation API ---- */

/* Cast function pointers to const void* for tcc_add_symbol.
 * C forbids implicit function-ptr → void* conversion; GCC allows it
 * but TCC (when self-hosting) rejects it. */
#define SYM(name, fn) tcc_add_symbol(s, name, (const void *)(UINTN)(fn))

/* Shim functions that user programs can call */
static void register_api(TCCState *s) {
    /* Framebuffer */
    SYM("fb_pixel",  fb_pixel);
    SYM("fb_rect",   fb_rect);
    SYM("fb_clear",  fb_clear);
    SYM("fb_char",   fb_char);
    SYM("fb_string", fb_string);
    SYM("fb_scroll", fb_scroll);
    SYM("fb_print",  fb_print);

    /* Keyboard */
    SYM("kbd_poll", kbd_poll);
    SYM("kbd_wait", kbd_wait);

    /* Memory */
    SYM("mem_alloc", mem_alloc);
    SYM("mem_free",  mem_free);
    SYM("mem_set",   mem_set);
    SYM("mem_copy",  mem_copy);

    /* Filesystem */
    SYM("fs_readfile",  fs_readfile);
    SYM("fs_writefile", fs_writefile);
    SYM("fs_readdir",   fs_readdir);

    /* Global state */
    tcc_add_symbol(s, "g_boot", &g_boot);

    /* Shim libc functions */
    SYM("printf",   printf);
    SYM("snprintf", snprintf);
    SYM("sprintf",  sprintf);
    SYM("strlen",   strlen);
    SYM("strcmp",    strcmp);
    SYM("strcpy",   strcpy);
    SYM("memcpy",   memcpy);
    SYM("memset",   memset);
    SYM("malloc",   malloc);
    SYM("free",     free);
    SYM("puts",     puts);
    SYM("memchr",   memchr);
    SYM("strspn",   strspn);
    SYM("strcspn",  strcspn);
    SYM("strtok",   strtok);
    SYM("bsearch",  bsearch);
    SYM("rand",     rand);
    SYM("srand",    srand);
    SYM("strcat",   strcat);
    SYM("strrchr",  strrchr);
    SYM("strstr",   strstr);
    SYM("strdup",   strdup);
    SYM("strncpy",  strncpy);
    SYM("strncmp",  strncmp);
    SYM("strchr",   strchr);
    SYM("atoi",     atoi);
    SYM("realloc",  realloc);
    SYM("calloc",   calloc);
    SYM("memcmp",   memcmp);
    SYM("memmove",  memmove);
    SYM("qsort",    qsort);
    SYM("strtol",   strtol);
    SYM("strtoul",  strtoul);
}

#undef SYM

/* ---- Main compile+run entry point ---- */

struct tcc_result tcc_run_source(const char *source, const char *filename) {
    struct tcc_result result;
    mem_set(&result, 0, sizeof(result));

    /* Setup error capture */
    s_errbuf = result.error_msg;
    s_errbuf_pos = 0;
    s_errbuf_size = (int)sizeof(result.error_msg);
    s_errbuf[0] = '\0';

    /* Create TCC context */
    TCCState *tcc = tcc_new();
    if (!tcc) {
        strcpy(result.error_msg, "Failed to create TCC context");
        return result;
    }

    tcc_set_error_func(tcc, NULL, tcc_error_handler);
    tcc_set_options(tcc, "-nostdlib -nostdinc");
    tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);

    /* Add include path — user headers on the FAT32 image */
    tcc_add_include_path(tcc, "/include");

    /* Register workstation API symbols */
    register_api(tcc);

    /* Build a line directive so errors show the right filename */
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "#line 1 \"%s\"\n", filename ? filename : "input.c");

    /* Concatenate prefix + source */
    int plen = (int)strlen(prefix);
    int slen = (int)strlen(source);
    char *full = (char *)malloc((size_t)(plen + slen + 1));
    if (!full) {
        strcpy(result.error_msg, "Out of memory");
        tcc_delete(tcc);
        return result;
    }
    memcpy(full, prefix, (size_t)plen);
    memcpy(full + plen, source, (size_t)slen);
    full[plen + slen] = '\0';

    /* Compile */
    if (tcc_compile_string(tcc, full) < 0) {
        free(full);
        tcc_delete(tcc);
        return result; /* error_msg already filled by handler */
    }
    free(full);

    /* Relocate */
    if (tcc_relocate(tcc) < 0) {
        tcc_delete(tcc);
        return result;
    }

    /* Get main symbol */
    int (*prog_main)(void) = (int (*)(void))tcc_get_symbol(tcc, "main");
    if (!prog_main) {
        strcpy(result.error_msg, "No main() function found");
        tcc_delete(tcc);
        return result;
    }

    /* Setup exit() recovery via setjmp/longjmp */
    shim_exit_active = 1;
    shim_exit_code = 0;
    int jmpval = setjmp(shim_exit_jmpbuf);

    if (jmpval == 0) {
        /* First time — call the program */
        result.exit_code = prog_main();
        result.success = 1;
    } else {
        /* Returned from exit() via longjmp */
        result.exit_code = shim_exit_code;
        result.success = 1;
    }

    shim_exit_active = 0;
    tcc_delete(tcc);
    return result;
}
