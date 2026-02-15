# Chapter 13: The C Compiler

## Why a Compiler?

We have a text editor. We can create files, save them, browse the filesystem. But the workstation is inert — a fancy notepad running on bare metal. We can write C code, stare at it, and that's it.

A survival workstation needs to run programs. Not just pre-compiled binaries baked into the firmware, but programs written on the machine itself. You find yourself with this board, a keyboard, a monitor. You open the editor, write some C, press a key, and it runs. That's the vision.

The conventional approach would be to cross-compile programs on a host machine and copy them to an SD card. But that defeats the purpose. We want self-sufficiency. The compiler must live inside the workstation.

## TinyCC

Fabrice Bellard's [TinyCC](https://bellard.org/tcc/) is the only C compiler small enough to embed in a firmware application. The entire compiler — preprocessor, parser, code generator, assembler, and linker — compiles into a single ~300KB object file. It supports ARM64. And critically, it has a library API (`libtcc`) that compiles C source strings to machine code in memory:

```
tcc_new() → tcc_compile_string(source) → tcc_relocate() → tcc_get_symbol("main") → call it
```

No temporary files. No disk I/O. No ELF loading. Just source code in, function pointer out.

## Getting TCC

Clone the TCC source into `tools/`:

```bash
git clone https://repo.or.cz/tinycc.git tools/tinycc
```

For now, we don't modify TCC's source code. All adaptation happens in our shim layer — a set of files that make TCC's Linux-expecting code work in our UEFI environment. (In Phase 4, when we teach TCC to output UEFI PE binaries and make the workstation self-hosting, we'll need to patch TCC itself — but that's a story for later.)

## The Shim Layer

TCC's source code is written for Linux. It calls `malloc`, `printf`, `strlen`, `open`, `read` — all the libc functions you'd expect. We don't have libc. We have UEFI Boot Services.

The solution is a shim layer: roughly 30 libc functions implemented on top of UEFI. Memory allocation wraps `AllocatePool`. String output routes through `fb_print`. File I/O uses our `fs_readfile` from Chapter 9. Everything else is either a trivial implementation or a stub that returns a harmless default.

This shim is the bulk of the work in this chapter. TCC is a gift — someone already wrote the compiler. Our job is to build the world it expects to live in.

## The Dual-Header Problem

Here's a subtlety. When we compile our own code (`main.c`, `fb.c`, `shim.c`), we include gnu-efi's headers, which define types like `UINT64`, `UINTN`, and `jmp_buf`. But when we compile TCC's `libtcc.c`, we must NOT include gnu-efi headers — TCC expects standard C types like `size_t`, `int64_t`, and its own `jmp_buf`.

Two compilation modes, two different type systems. The shim header handles both with a compile-time check.

Create `src/shim.h`:

```c
/*
 * shim.h — Minimal libc replacement for TCC on UEFI
 *
 * Two usage modes:
 * 1. TCC compilation: included via tcc-headers/ stubs, provides ALL types
 * 2. Our own code: included after boot.h (gnu-efi), provides only declarations
 *
 * Detection: gnu-efi defines _EFI_INCLUDE_ in efi.h
 */
#ifndef SHIM_H
#define SHIM_H

/*
 * When building our own code (shim.c, tcc.c), gnu-efi provides base types.
 * When building libtcc.o, we provide everything.
 */
#ifdef _EFI_INCLUDE_
/* ---- Building with gnu-efi: types come from EFI headers ---- */
/* Just need a few types not in gnu-efi */
typedef long long          int64_shim_t;
typedef unsigned long long uint64_shim_t;
typedef long               ssize_t;
typedef long               off_t;
typedef long               time_t;
typedef int                pid_t;

/* Use gnu-efi's setjmp if available, else ours */
#include <efisetjmp.h>

#else /* !_EFI_INCLUDE_ — building TCC */

/* ---- Compiler builtins ---- */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- Fixed-width types ---- */
typedef unsigned char      uint8_t;
typedef signed   char      int8_t;
typedef unsigned short     uint16_t;
typedef signed   short     int16_t;
typedef unsigned int       uint32_t;
typedef signed   int       int32_t;
typedef unsigned long      uint64_t;
typedef signed   long      int64_t;
typedef unsigned long      uintptr_t;
typedef signed   long      intptr_t;

typedef uint64_t  size_t;
typedef int64_t   ssize_t;
typedef int64_t   ptrdiff_t;
typedef int64_t   off_t;
typedef int64_t   time_t;
typedef int32_t   pid_t;
typedef int64_t   intmax_t;
typedef uint64_t  uintmax_t;

/* ---- Varargs (GCC builtins) ---- */
typedef __builtin_va_list  va_list;
#define va_start(v,l)      __builtin_va_start(v,l)
#define va_end(v)          __builtin_va_end(v)
#define va_arg(v,t)        __builtin_va_arg(v,t)
#define va_copy(d,s)       __builtin_va_copy(d,s)

/* ---- setjmp / longjmp (assembly in setjmp.S) ---- */
/* ARM64: save x19-x30, sp, d8-d15 = 22 registers x 8 bytes = 176 bytes */
typedef long jmp_buf[22];
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* ---- offsetof ---- */
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif /* _EFI_INCLUDE_ */
```

gnu-efi's `efi.h` defines the macro `_EFI_INCLUDE_`. When compiling `shim.c` (which includes `boot.h` → `efi.h`), the guard activates and shim.h only provides a few extra types and function declarations. When compiling `libtcc.c` (which includes `stdio.h` → `shim.h` without gnu-efi), the guard is absent and shim.h provides all the standard C types, varargs, and setjmp.

Note that `jmp_buf` is defined as an array of 22 longs — we'll implement the assembly for this later. On ARM64, that's exactly enough to save all callee-preserved registers.

## Stub Headers

TCC's source files `#include <stdio.h>`, `#include <stdlib.h>`, `#include <string.h>`, and about 17 other standard headers. We create a `src/tcc-headers/` directory where each file contains a single line:

```c
#include "../shim.h"
```

Create these files:

```
src/tcc-headers/stdio.h
src/tcc-headers/stdlib.h
src/tcc-headers/string.h
src/tcc-headers/stdint.h
src/tcc-headers/stddef.h
src/tcc-headers/stdarg.h
src/tcc-headers/errno.h
src/tcc-headers/fcntl.h
src/tcc-headers/setjmp.h
src/tcc-headers/time.h
src/tcc-headers/unistd.h
src/tcc-headers/math.h
src/tcc-headers/assert.h
src/tcc-headers/signal.h
src/tcc-headers/dlfcn.h
src/tcc-headers/ctype.h
src/tcc-headers/float.h
src/tcc-headers/limits.h
src/tcc-headers/inttypes.h
src/tcc-headers/sys/time.h
```

Every one of them contains just `#include "../shim.h"` (or `#include "../../shim.h"` for `sys/time.h`). By compiling TCC with `-Isrc/tcc-headers`, GCC finds our stubs before searching system paths. Every standard header TCC tries to include redirects to our shim.

## The Rest of the Header

Now we need the declarations and constants that both compilation modes share. Everything below the `#endif /* _EFI_INCLUDE_ */` line in `shim.h` is included regardless of which mode we're building in.

### errno and Limits

TCC checks errno values and integer limits internally. We define the standard constants:

```c
/* ================================================================
 * Everything below is needed by both TCC and our own code
 * ================================================================ */

/* ---- errno ---- */
extern int errno;
#ifndef EPERM
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define EBADF    9
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENOTDIR 20
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ERANGE  34
#define ENOSYS  38
#endif

/* ---- limits ---- */
#ifndef CHAR_BIT
#define CHAR_BIT    8
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX
#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535
#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295U
#define LONG_MIN    (-9223372036854775807L - 1)
#define LONG_MAX    9223372036854775807L
#define ULONG_MAX   18446744073709551615UL
#define LLONG_MIN   LONG_MIN
#define LLONG_MAX   LONG_MAX
#define ULLONG_MAX  ULONG_MAX
#ifndef SIZE_MAX
#define SIZE_MAX    ULONG_MAX
#endif
#endif
#ifndef PATH_MAX
#define PATH_MAX    4096
#endif
#ifndef FILENAME_MAX
#define FILENAME_MAX 4096
#endif
```

The `#ifndef` guards prevent double-definition when both our header and a system header might be included.

### Float Constants

TCC's float literal parser and code generator reference float.h constants. We need the full set:

```c
/* ---- float.h ---- */
#ifndef DBL_MAX
#define DBL_MAX     1.7976931348623157e+308
#define DBL_MIN     2.2250738585072014e-308
#define FLT_MAX     3.40282347e+38F
#define FLT_MIN     1.17549435e-38F
#define LDBL_MAX    DBL_MAX
#define LDBL_MIN    DBL_MIN
#define DBL_MANT_DIG  53
#define FLT_MANT_DIG  24
#define LDBL_MANT_DIG DBL_MANT_DIG
#define DBL_DIG       15
#define FLT_DIG       6
#define LDBL_DIG      DBL_DIG
#define DBL_EPSILON   2.2204460492503131e-16
#define FLT_EPSILON   1.19209290e-7F
#define LDBL_EPSILON  DBL_EPSILON
#define DBL_MAX_EXP   1024
#define FLT_MAX_EXP   128
#define LDBL_MAX_EXP  DBL_MAX_EXP
#define DBL_MIN_EXP   (-1021)
#define FLT_MIN_EXP   (-125)
#define LDBL_MIN_EXP  DBL_MIN_EXP
#define DBL_MAX_10_EXP 308
#define FLT_MAX_10_EXP 38
#define LDBL_MAX_10_EXP DBL_MAX_10_EXP
#define DBL_MIN_10_EXP (-307)
#define FLT_MIN_10_EXP (-37)
#define LDBL_MIN_10_EXP DBL_MIN_10_EXP
#define FLT_RADIX       2
#endif
```

`LDBL_*` aliases `DBL_*` because our config sets `TCC_USING_DOUBLE_FOR_LDOUBLE` — on ARM64, long double is just double.

### FILE and I/O Constants

TCC uses FILE pointers for output and references stdin/stdout/stderr. We define a minimal FILE type and the POSIX constants for `open()` and `lseek()`:

```c
/* ---- FILE ---- */
#ifndef _SHIM_FILE_DEFINED
#define _SHIM_FILE_DEFINED
typedef struct _FILE { int _dummy; } FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#endif

#ifndef EOF
#define EOF (-1)
#endif
#ifndef BUFSIZ
#define BUFSIZ 1024
#endif

/* ---- fcntl / open flags ---- */
#ifndef O_RDONLY
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400
#endif
#ifndef O_BINARY
#define O_BINARY  0
#endif

/* ---- seek ---- */
#ifndef SEEK_SET
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#endif
```

Our FILE struct is empty — we don't actually do buffered I/O. The FILE pointer is just a token that our fprintf/fwrite implementations use to distinguish stdout from stderr.

### Signal, Time, Assert, and ctype

A grab-bag of types and macros that TCC references:

```c
/* ---- signal ---- */
#ifndef SIGABRT
#define SIGABRT   6
#define SIGFPE    8
#define SIGSEGV  11
#define SIGBUS    7
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)
#endif
typedef void (*sighandler_t)(int);

/* ---- time structs ---- */
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon;
    int tm_year, tm_wday, tm_yday, tm_isdst;
};

struct timeval {
    long tv_sec;
    long tv_usec;
};

/* ---- assert ---- */
#ifndef assert
#define assert(x) ((void)0)
#endif
```

Assertions are no-ops — we can't abort into a debugger. Signal types exist so TCC's code compiles but we never actually deliver signals.

The ctype functions are implemented as inline helpers to avoid external linkage issues:

```c
/* ---- ctype ---- */
static inline int shim_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int shim_isxdigit(int c) { return shim_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int shim_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int shim_isalnum(int c) { return shim_isalpha(c) || shim_isdigit(c); }
static inline int shim_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static inline int shim_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int shim_islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int shim_toupper(int c) { return shim_islower(c) ? c - 32 : c; }
static inline int shim_tolower(int c) { return shim_isupper(c) ? c + 32 : c; }
static inline int shim_isprint(int c) { return c >= 0x20 && c <= 0x7e; }

#define isdigit  shim_isdigit
#define isxdigit shim_isxdigit
#define isalpha  shim_isalpha
#define isalnum  shim_isalnum
#define isspace  shim_isspace
#define isupper  shim_isupper
#define islower  shim_islower
#define toupper  shim_toupper
#define tolower  shim_tolower
#define isprint  shim_isprint
```

The `shim_` prefix avoids name collisions, then macros map the standard names.

### Function Declarations

The rest of the header declares every function our shim provides. These are organized by category:

```c
/* ---- Memory ---- */
void *malloc(size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);

/* ---- String ---- */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);
char *strdup(const char *s);
int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, size_t n);

/* ---- Number parsing ---- */
long          strtol(const char *s, char **endp, int base);
unsigned long strtoul(const char *s, char **endp, int base);
long long     strtoll(const char *s, char **endp, int base);
unsigned long long strtoull(const char *s, char **endp, int base);
int           atoi(const char *s);
long          atol(const char *s);
double        strtod(const char *s, char **endp);
float         strtof(const char *s, char **endp);
long double   strtold(const char *s, char **endp);

/* ---- Formatted I/O ---- */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);
int puts(const char *s);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int putchar(int c);
int fflush(FILE *f);

/* ---- File I/O (fd-based) ---- */
int   open(const char *path, int flags, ...);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int   close(int fd);
int   unlink(const char *path);
int   ftruncate(int fd, off_t length);
FILE *fdopen(int fd, const char *mode);
char *realpath(const char *path, char *resolved);

/* ---- File I/O (FILE-based) ---- */
FILE *fopen(const char *path, const char *mode);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
int    fclose(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    ungetc(int c, FILE *f);
int    fgetc(FILE *f);
char  *fgets(char *buf, int size, FILE *f);
void   clearerr(FILE *f);
int    remove(const char *path);
int    rename(const char *oldpath, const char *newpath);
FILE  *tmpfile(void);

/* ---- Misc libc ---- */
void   exit(int status) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
void   _exit(int status) __attribute__((noreturn));
char  *getenv(const char *name);
time_t time(time_t *t);
struct tm *localtime(const time_t *t);
char  *getcwd(char *buf, size_t size);
int    gettimeofday(struct timeval *tv, void *tz);
void   qsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));
char  *strerror(int errnum);
int    mprotect(void *addr, size_t len, int prot);
#ifndef CONFIG_TCC_STATIC
void  *dlopen(const char *filename, int flags);
void  *dlsym(void *handle, const char *symbol);
char  *dlerror(void);
int    dlclose(void *handle);
#endif
sighandler_t signal(int signum, sighandler_t handler);
long   sysconf(int name);
int    isatty(int fd);
pid_t  getpid(void);
unsigned int sleep(unsigned int seconds);
```

That's a lot of functions. Most are trivial implementations or stubs. The ones that actually do work: `malloc`/`free`/`realloc`, `vsnprintf`, `open`/`read`/`close`, and `exit`. Everything else is either a standard string function or a stub that returns zero.

### Remaining Constants and Exit Recovery

Finally, the bottom of the header:

```c
/* mprotect prot flags */
#ifndef PROT_NONE
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#endif

/* dlopen flags */
#ifndef RTLD_LAZY
#define RTLD_LAZY    1
#define RTLD_NOW     2
#define RTLD_GLOBAL  0x100
#endif

/* sysconf names */
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#endif

/* ---- abs/labs ---- */
#ifndef _EFI_INCLUDE_
static inline int   abs(int x) { return x < 0 ? -x : x; }
static inline long  labs(long x) { return x < 0 ? -x : x; }
static inline long long llabs(long long x) { return x < 0 ? -x : x; }
#endif

/* ---- math stubs ---- */
double ldexp(double x, int exp);
double frexp(double x, int *exp);

/* ---- environ ---- */
extern char **environ;

/* ---- Shared state for exit recovery and error capture ---- */
#ifdef _EFI_INCLUDE_
#include <efisetjmp.h>
extern jmp_buf shim_exit_jmpbuf;
#else
extern jmp_buf shim_exit_jmpbuf;
#endif
extern int     shim_exit_active;
extern int     shim_exit_code;
extern char    shim_errbuf[4096];
extern int     shim_errbuf_pos;

#endif /* SHIM_H */
```

The exit recovery state at the bottom is how we handle user programs calling `exit()`. When the runtime wrapper in Chapter 14 prepares to call compiled code, it saves the CPU state with `setjmp`. If the compiled code calls `exit()`, our implementation does `longjmp` back to that saved state. The `shim_exit_active` flag tells `exit()` whether a recovery point exists.

That's the complete header — 403 lines. Now we implement it.

## The Shim Implementation

Create `src/shim.c`. We'll build it section by section.

### Globals

```c
/*
 * shim.c — Minimal libc implementation for TCC on UEFI
 *
 * Provides the ~30 C library functions that TCC's internals require.
 * Routes memory through UEFI AllocatePool, output through fb_print,
 * and file I/O through our fs_readfile/fs_writefile wrappers.
 */

#include "boot.h"
#include "fb.h"
#include "mem.h"
#include "fs.h"
#include "shim.h"

/* ================================================================
 * Globals
 * ================================================================ */

int errno = 0;

/* Fake FILE handles — we only need stdin/stdout/stderr */
static FILE _stdin_obj, _stdout_obj, _stderr_obj;
FILE *stdin  = &_stdin_obj;
FILE *stdout = &_stdout_obj;
FILE *stderr = &_stderr_obj;

/* environ — TCC references this */
static char *empty_environ[] = { NULL };
char **environ = empty_environ;

/* exit() recovery — set by tcc runtime wrapper */
jmp_buf shim_exit_jmpbuf;
int     shim_exit_active = 0;
int     shim_exit_code = 0;

/* Output buffer for captured stderr (TCC error messages) */
char shim_errbuf[4096];
int  shim_errbuf_pos = 0;
```

Three FILE objects exist as static variables. We never read or write through them — they're just addresses that `fprintf(stderr, ...)` can compare against to decide where output goes. The `environ` pointer satisfies TCC's reference to the global environment; it's always empty.

### Memory with Size Tracking

UEFI's `AllocatePool` allocates memory and `FreePool` frees it, but `FreePool` doesn't need a size argument — the firmware tracks that internally. The problem is `realloc`: it needs to know the old allocation size to copy the data. UEFI won't tell us.

The solution is a header prepended to every allocation:

```c
/* ================================================================
 * Memory with size tracking
 * ================================================================ */

#define ALLOC_MAGIC 0xA110CA7E

struct alloc_hdr {
    size_t size;
    size_t magic;
};

void *malloc(size_t size) {
    if (size == 0) size = 1;
    struct alloc_hdr *hdr = (struct alloc_hdr *)mem_alloc(
        sizeof(struct alloc_hdr) + size);
    if (!hdr) return NULL;
    hdr->size = size;
    hdr->magic = ALLOC_MAGIC;
    return (void *)(hdr + 1);
}

void free(void *ptr) {
    if (!ptr) return;
    struct alloc_hdr *hdr = ((struct alloc_hdr *)ptr) - 1;
    if (hdr->magic != ALLOC_MAGIC) return; /* bad pointer */
    hdr->magic = 0;
    mem_free(hdr);
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    struct alloc_hdr *hdr = ((struct alloc_hdr *)ptr) - 1;
    if (hdr->magic != ALLOC_MAGIC) return NULL;

    size_t old_size = hdr->size;
    if (size <= old_size) return ptr; /* shrink: just return */

    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    /* mem_alloc already zeroes, so no memset needed */
    return ptr;
}
```

Each allocation is 16 bytes larger than requested. The header stores the size and a magic number (`0xA110CA7E` — "allocate"). `free` checks the magic before freeing — a simple guard against double-frees and wild pointers. `calloc` skips zeroing because `mem_alloc` already does it (it calls `mem_set` internally). `realloc` returns the same pointer if the new size is smaller — no point copying.

### String Operations

These are the workhorses. TCC calls string functions constantly — parsing identifiers, copying symbol names, building error messages. We need them all:

```c
/* ================================================================
 * String / memory functions
 * ================================================================ */

__attribute__((weak))
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}

__attribute__((weak))
void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i;
    for (i = 0; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == 0) return (char *)s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    size_t nlen = strlen(needle);
    while (*hay) {
        if (strncmp(hay, needle, nlen) == 0) return (char *)hay;
        hay++;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = (char *)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((uint8_t)*a);
        int cb = tolower((uint8_t)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n && *a && *b; i++) {
        int ca = tolower((uint8_t)*a);
        int cb = tolower((uint8_t)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return 0;
}
```

Two functions have `__attribute__((weak))`: `memcpy` and `memset`. Without this, the linker would complain about duplicate symbols — `libefi.a` (from gnu-efi) also provides these functions. The `weak` attribute tells the linker: "use this definition only if no other (strong) definition exists." In practice, ours ends up being used because we link our objects before `-lefi`.

`memmove` handles overlapping regions by checking the copy direction. If the destination is before the source, we copy forward. If after, we copy backward. `memcpy` doesn't bother — the C standard says overlapping `memcpy` is undefined behavior, and TCC's code doesn't rely on it.

`strstr` uses a naive O(n*m) algorithm — scan forward, trying `strncmp` at each position. For the short strings TCC processes (filenames, identifiers), this is fine.

### Number Parsing

TCC parses numeric literals and command-line numbers. The core is a single function that handles all bases:

```c
/* ================================================================
 * Number parsing
 * ================================================================ */

static int char_to_digit(char c, int base) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    else return -1;
    return (d < base) ? d : -1;
}

unsigned long long shim_strtoull(const char *s, char **endp, int base,
                                  int *neg_out) {
    const char *start = s;
    while (isspace((uint8_t)*s)) s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    *neg_out = neg;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16; s += 2;
        } else if (s[0] == '0') {
            base = 8; s++;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    unsigned long long result = 0;
    int any = 0;
    while (*s) {
        int d = char_to_digit(*s, base);
        if (d < 0) break;
        result = result * (unsigned long long)base + (unsigned long long)d;
        any = 1;
        s++;
    }

    if (endp) *endp = (char *)(any ? s : start);
    return result;
}

long strtol(const char *s, char **endp, int base) {
    int neg;
    unsigned long long v = shim_strtoull(s, endp, base, &neg);
    return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *s, char **endp, int base) {
    int neg;
    unsigned long long v = shim_strtoull(s, endp, base, &neg);
    return neg ? -(unsigned long)v : (unsigned long)v;
}

long long strtoll(const char *s, char **endp, int base) {
    int neg;
    unsigned long long v = shim_strtoull(s, endp, base, &neg);
    return neg ? -(long long)v : (long long)v;
}

unsigned long long strtoull(const char *s, char **endp, int base) {
    int neg;
    unsigned long long v = shim_strtoull(s, endp, base, &neg);
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
```

The pattern: one core function (`shim_strtoull`) does all the work — skip whitespace, parse sign, detect base from prefix (`0x` for hex, `0` for octal), accumulate digits. The public functions are one-line wrappers that apply the sign and cast to the right type.

We also need a minimal floating-point parser. TCC encounters float literals in source code and needs `strtod`:

```c
/* Minimal strtod: integer + decimal + exponent */
double strtod(const char *s, char **endp) {
    const char *start = s;
    while (isspace((uint8_t)*s)) s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    double result = 0.0;
    int any = 0;

    /* Integer part */
    while (*s >= '0' && *s <= '9') {
        result = result * 10.0 + (*s - '0');
        s++;
        any = 1;
    }

    /* Decimal part */
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            result += (*s - '0') * frac;
            frac *= 0.1;
            s++;
            any = 1;
        }
    }

    /* Exponent part */
    if (any && (*s == 'e' || *s == 'E')) {
        s++;
        int exp_neg = 0;
        if (*s == '-') { exp_neg = 1; s++; }
        else if (*s == '+') s++;

        int exp_val = 0;
        while (*s >= '0' && *s <= '9') {
            exp_val = exp_val * 10 + (*s - '0');
            s++;
        }

        double factor = 1.0;
        for (int i = 0; i < exp_val; i++) factor *= 10.0;
        if (exp_neg) result /= factor;
        else result *= factor;
    }

    if (endp) *endp = (char *)(any ? s : start);
    return neg ? -result : result;
}

float strtof(const char *s, char **endp) {
    return (float)strtod(s, endp);
}

long double strtold(const char *s, char **endp) {
    return (long double)strtod(s, endp);
}
```

This isn't a perfect IEEE 754 parser — it loses precision on very large or small numbers. But TCC doesn't need perfect precision in its own internals; it just needs to be able to parse float constants well enough to generate the right code.

## Formatted Output

This is the largest single subsystem in the shim. TCC's error messages, debug output, and internal formatting all flow through `printf` and `snprintf`. If this doesn't work correctly, TCC's error messages are garbage and debugging becomes impossible.

We build `vsnprintf` from three helpers:

```c
/* ================================================================
 * Formatted output — vsnprintf
 * ================================================================ */

/* Helper: write a single char to buffer */
static void sn_putc(char *buf, size_t size, size_t *pos, char c) {
    if (*pos < size - 1) buf[*pos] = c;
    (*pos)++;
}
```

`sn_putc` writes a character to the output buffer if there's room, and increments the position counter regardless. This means `vsnprintf` always returns the total length the output *would* be, even if the buffer is too small — matching the C99 specification.

```c
/* Helper: write an unsigned integer */
static void sn_uint(char *buf, size_t size, size_t *pos,
                     unsigned long long val, int base, int upper,
                     int width, int zero_pad, int left_align) {
    char tmp[24];
    int len = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0) {
            tmp[len++] = digits[val % (unsigned long long)base];
            val /= (unsigned long long)base;
        }
    }

    int pad = (width > len) ? width - len : 0;
    char pad_char = zero_pad ? '0' : ' ';

    if (!left_align) {
        for (int i = 0; i < pad; i++) sn_putc(buf, size, pos, pad_char);
    }
    for (int i = len - 1; i >= 0; i--) sn_putc(buf, size, pos, tmp[i]);
    if (left_align) {
        for (int i = 0; i < pad; i++) sn_putc(buf, size, pos, ' ');
    }
}
```

`sn_uint` converts an unsigned integer to digits in reverse order (least significant first), then outputs them reversed. The `upper` flag controls hex case (`%x` vs `%X`). Width and padding flags handle `%08x` and `%-10d` style formatting.

```c
/* Helper: write a signed integer */
static void sn_int(char *buf, size_t size, size_t *pos,
                    long long val, int width, int zero_pad, int left_align) {
    int neg = 0;
    unsigned long long uval;

    if (val < 0) { neg = 1; uval = (unsigned long long)(-val); }
    else { uval = (unsigned long long)val; }

    if (neg) {
        if (zero_pad && !left_align) {
            sn_putc(buf, size, pos, '-');
            if (width > 0) width--;
            sn_uint(buf, size, pos, uval, 10, 0, width, 1, 0);
        } else {
            /* Count digits to do padding correctly */
            int dlen = 0;
            unsigned long long v = uval;
            if (v == 0) dlen = 1;
            else while (v > 0) { dlen++; v /= 10; }
            dlen++; /* for '-' */
            int pad = (width > dlen) ? width - dlen : 0;
            if (!left_align) {
                for (int i = 0; i < pad; i++) sn_putc(buf, size, pos, ' ');
            }
            sn_putc(buf, size, pos, '-');
            sn_uint(buf, size, pos, uval, 10, 0, 0, 0, 0);
            if (left_align) {
                for (int i = 0; i < pad; i++) sn_putc(buf, size, pos, ' ');
            }
        }
    } else {
        sn_uint(buf, size, pos, uval, 10, 0, width, zero_pad, left_align);
    }
}
```

Signed integers have a subtlety with zero-padding: `%-08d` with value -42 should produce `-0000042`, with the minus sign before the zeros. The code handles both cases.

Now the main event — the `vsnprintf` loop that parses format strings:

```c
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;

    if (size == 0) {
        /* Just count characters */
        size = 1; /* prevent writing, but allow counting */
        buf = NULL;
    }

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sn_putc(buf ? buf : (char*)"", buf ? size : 1, &pos, *fmt);
            continue;
        }
        fmt++; /* skip '%' */

        /* Flags */
        int zero_pad = 0, left_align = 0;
        while (*fmt == '0' || *fmt == '-' || *fmt == ' ' || *fmt == '+') {
            if (*fmt == '0') zero_pad = 1;
            if (*fmt == '-') left_align = 1;
            fmt++;
        }
        if (left_align) zero_pad = 0;

        /* Width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left_align = 1; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* Length modifier */
        int is_long = 0; /* 1=long, 2=long long */
        int is_size_t = 0;
        if (*fmt == 'l') {
            is_long = 1; fmt++;
            if (*fmt == 'l') { is_long = 2; fmt++; }
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++; /* hh */
        } else if (*fmt == 'z') {
            is_size_t = 1; fmt++;
        } else if (*fmt == 'j' || *fmt == 't') {
            is_long = 2; fmt++;
        }

        char *out = buf ? buf : (char *)"";
        size_t out_size = buf ? size : 1;

        /* Conversion */
        switch (*fmt) {
        case 'd': case 'i': {
            long long val;
            if (is_long == 2) val = va_arg(ap, long long);
            else if (is_long == 1 || is_size_t) val = va_arg(ap, long);
            else val = va_arg(ap, int);
            sn_int(out, out_size, &pos, val, width, zero_pad, left_align);
            break;
        }
        case 'u': {
            unsigned long long val;
            if (is_long == 2) val = va_arg(ap, unsigned long long);
            else if (is_long == 1 || is_size_t) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            sn_uint(out, out_size, &pos, val, 10, 0, width, zero_pad, left_align);
            break;
        }
        case 'x': case 'X': {
            unsigned long long val;
            if (is_long == 2) val = va_arg(ap, unsigned long long);
            else if (is_long == 1 || is_size_t) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            sn_uint(out, out_size, &pos, val, 16, (*fmt == 'X'), width, zero_pad, left_align);
            break;
        }
        case 'o': {
            unsigned long long val;
            if (is_long == 2) val = va_arg(ap, unsigned long long);
            else if (is_long == 1 || is_size_t) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            sn_uint(out, out_size, &pos, val, 8, 0, width, zero_pad, left_align);
            break;
        }
        case 'p': {
            unsigned long long val = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            sn_putc(out, out_size, &pos, '0');
            sn_putc(out, out_size, &pos, 'x');
            sn_uint(out, out_size, &pos, val, 16, 0, width > 2 ? width - 2 : 0, 1, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = (int)strlen(s);
            if (prec >= 0 && prec < slen) slen = prec;
            int pad = (width > slen) ? width - slen : 0;
            if (!left_align) {
                for (int i = 0; i < pad; i++) sn_putc(out, out_size, &pos, ' ');
            }
            for (int i = 0; i < slen; i++) sn_putc(out, out_size, &pos, s[i]);
            if (left_align) {
                for (int i = 0; i < pad; i++) sn_putc(out, out_size, &pos, ' ');
            }
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = (width > 1) ? width - 1 : 0;
            if (!left_align) {
                for (int i = 0; i < pad; i++) sn_putc(out, out_size, &pos, ' ');
            }
            sn_putc(out, out_size, &pos, c);
            if (left_align) {
                for (int i = 0; i < pad; i++) sn_putc(out, out_size, &pos, ' ');
            }
            break;
        }
        case '%':
            sn_putc(out, out_size, &pos, '%');
            break;
        case 'n':
            /* Store characters written so far */
            if (is_long == 2) *va_arg(ap, long long *) = (long long)pos;
            else if (is_long == 1) *va_arg(ap, long *) = (long)pos;
            else *va_arg(ap, int *) = (int)pos;
            break;
        case '\0':
            goto done;
        default:
            sn_putc(out, out_size, &pos, '%');
            sn_putc(out, out_size, &pos, *fmt);
            break;
        }
    }
done:
    if (buf) {
        if (pos < size) buf[pos] = '\0';
        else buf[size - 1] = '\0';
    }
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap);
    return ret;
}
```

The format string parser handles: `%d`, `%i`, `%u`, `%x`, `%X`, `%o`, `%p`, `%s`, `%c`, `%n`, `%%`, with width, precision, zero-padding, left-alignment, and length modifiers `l`, `ll`, `h`, `hh`, `z`, `j`, `t`. This covers everything TCC uses.

We don't handle `%f`/`%e`/`%g` (floating-point output). TCC doesn't use these internally. If a user program needs them, that's a future enhancement.

## Output Routing

Now we need `printf` and `fprintf` to actually produce visible output. The key function is `shim_output`, which routes text to either the framebuffer or the error capture buffer:

```c
static void shim_output(const char *s, int to_stderr) {
    if (to_stderr) {
        /* Capture to error buffer */
        int len = (int)strlen(s);
        for (int i = 0; i < len && shim_errbuf_pos < (int)sizeof(shim_errbuf) - 1; i++) {
            shim_errbuf[shim_errbuf_pos++] = s[i];
        }
        shim_errbuf[shim_errbuf_pos] = '\0';
    }
    /* Also print to screen if we have a framebuffer */
    if (g_boot.framebuffer) {
        fb_print(s, to_stderr ? COLOR_RED : COLOR_WHITE);
    }
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    shim_output(buf, 0);
    return ret;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[1024];
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    shim_output(buf, (f == stderr));
    return ret;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(f, fmt, ap);
    va_end(ap);
    return ret;
}

int puts(const char *s) {
    shim_output(s, 0);
    shim_output("\n", 0);
    return 0;
}

int fputs(const char *s, FILE *f) {
    shim_output(s, (f == stderr));
    return 0;
}

int fputc(int c, FILE *f) {
    char buf[2] = { (char)c, '\0' };
    shim_output(buf, (f == stderr));
    return c;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fflush(FILE *f) {
    (void)f;
    return 0;
}
```

When TCC calls `fprintf(stderr, ...)` with an error message, `shim_output` captures it in `shim_errbuf` AND displays it on screen in red. This dual routing means the runtime wrapper can collect error messages into the result structure while the user can also see them live.

`fflush` is a no-op — we have no output buffering to flush.

We also need a minimal `sscanf` for TCC's internal use:

```c
/* ================================================================
 * Minimal sscanf
 * ================================================================ */

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;

    while (*fmt && *str) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'd') {
                int *p = va_arg(ap, int *);
                char *end;
                long v = strtol(str, &end, 10);
                if (end == str) break;
                *p = (int)v;
                str = end;
                count++;
                fmt++;
            } else if (*fmt == 'x') {
                unsigned int *p = va_arg(ap, unsigned int *);
                char *end;
                unsigned long v = strtoul(str, &end, 16);
                if (end == str) break;
                *p = (unsigned int)v;
                str = end;
                count++;
                fmt++;
            } else if (*fmt == 's') {
                char *p = va_arg(ap, char *);
                while (*str && !isspace((uint8_t)*str)) *p++ = *str++;
                *p = '\0';
                count++;
                fmt++;
            } else {
                break;
            }
        } else if (isspace((uint8_t)*fmt)) {
            while (isspace((uint8_t)*str)) str++;
            fmt++;
        } else {
            if (*fmt != *str) break;
            fmt++; str++;
        }
    }

    va_end(ap);
    return count;
}
```

This handles `%d`, `%x`, and `%s` — the three conversions TCC uses.

## File I/O

TCC opens files when processing `#include` directives. It calls `open()`, `read()`, `lseek()`, and `close()` — standard POSIX file I/O. We implement these on top of our filesystem from Chapter 9.

The approach: when TCC opens a file, we read the entire file into memory (using `fs_readfile`) and store it in a slot table. Subsequent `read()` calls copy from this in-memory buffer. This is simple and works because TCC reads small header files sequentially.

```c
/* ================================================================
 * File I/O shim — fd-based
 * ================================================================ */

#define FD_MAX 16
#define FD_OFFSET 3  /* skip stdin=0, stdout=1, stderr=2 */

struct fd_slot {
    char   *data;
    size_t  size;
    size_t  pos;
    int     used;
};

static struct fd_slot fd_table[FD_MAX];

/* Convert ASCII path to CHAR16 for UEFI */
static void path_to_uefi(const char *path, CHAR16 *out, size_t max) {
    size_t i = 0;
    /* Skip leading dot-slash */
    if (path[0] == '.' && path[1] == '/') path += 2;

    /* Prepend backslash */
    if (path[0] != '/' && path[0] != '\\') {
        out[i++] = L'\\';
    }
    while (*path && i < max - 1) {
        out[i++] = (*path == '/') ? L'\\' : (CHAR16)*path;
        path++;
    }
    out[i] = 0;
}

int open(const char *path, int flags, ...) {
    (void)flags;

    /* Gracefully fail if filesystem not initialized */
    if (!path) { errno = EINVAL; return -1; }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < FD_MAX; i++) {
        if (!fd_table[i].used) { slot = i; break; }
    }
    if (slot < 0) { errno = EMFILE; return -1; }

    CHAR16 upath[512];
    path_to_uefi(path, upath, 512);

    UINTN fsize = 0;
    char *data = (char *)fs_readfile(upath, &fsize);
    if (!data) { errno = ENOENT; return -1; }

    fd_table[slot].data = data;
    fd_table[slot].size = fsize;
    fd_table[slot].pos = 0;
    fd_table[slot].used = 1;

    return slot + FD_OFFSET;
}
```

`FD_OFFSET` is 3 because POSIX reserves file descriptors 0, 1, and 2 for stdin, stdout, and stderr. Our returned fds start at 3. `path_to_uefi` converts Unix-style forward slashes to UEFI backslashes, and handles the leading `./` that TCC sometimes prepends to relative paths.

```c
ssize_t read(int fd, void *buf, size_t count) {
    int slot = fd - FD_OFFSET;
    if (slot < 0 || slot >= FD_MAX || !fd_table[slot].used) {
        errno = EBADF;
        return -1;
    }
    struct fd_slot *f = &fd_table[slot];
    size_t avail = f->size - f->pos;
    if (count > avail) count = avail;
    if (count > 0) {
        memcpy(buf, f->data + f->pos, count);
        f->pos += count;
    }
    return (ssize_t)count;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == 1) {
        /* stdout */
        char tmp[1025];
        size_t n = count > 1024 ? 1024 : count;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        shim_output(tmp, 0);
        return (ssize_t)count;
    }
    if (fd == 2) {
        /* stderr */
        char tmp[1025];
        size_t n = count > 1024 ? 1024 : count;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        shim_output(tmp, 1);
        return (ssize_t)count;
    }
    errno = EBADF;
    return -1;
}

off_t lseek(int fd, off_t offset, int whence) {
    int slot = fd - FD_OFFSET;
    if (slot < 0 || slot >= FD_MAX || !fd_table[slot].used) {
        errno = EBADF;
        return -1;
    }
    struct fd_slot *f = &fd_table[slot];
    off_t new_pos;
    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = (off_t)f->pos + offset; break;
        case SEEK_END: new_pos = (off_t)f->size + offset; break;
        default: errno = EINVAL; return -1;
    }
    if (new_pos < 0) new_pos = 0;
    if (new_pos > (off_t)f->size) new_pos = (off_t)f->size;
    f->pos = (size_t)new_pos;
    return new_pos;
}

int close(int fd) {
    int slot = fd - FD_OFFSET;
    if (slot < 0 || slot >= FD_MAX || !fd_table[slot].used) {
        errno = EBADF;
        return -1;
    }
    if (fd_table[slot].data) {
        mem_free(fd_table[slot].data);
    }
    memset(&fd_table[slot], 0, sizeof(struct fd_slot));
    return 0;
}
```

`read` is just a `memcpy` from the file buffer. `lseek` adjusts the position pointer. `close` frees the file data and clears the slot. `write` routes stdout/stderr to screen output — we don't support writing to files via fd (that's done through our `fs_writefile` API).

### FILE-Based Wrappers

TCC also uses `fopen`/`fread`/`fclose` in some code paths. These wrap our fd functions with a trick: we encode the fd number into the FILE pointer itself.

```c
/* ================================================================
 * FILE-based I/O wrappers
 * ================================================================ */

/* We encode fd into the FILE pointer: FILE * = (FILE *)(uintptr_t)(fd + 100) */
#define FILE_TO_FD(f)  ((int)((uintptr_t)(f) - 100))
#define FD_TO_FILE(fd) ((FILE *)(uintptr_t)((fd) + 100))

FILE *fopen(const char *path, const char *mode) {
    (void)mode; /* always read-only for now */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    return FD_TO_FILE(fd);
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (f == stdin) return 0;
    int fd = FILE_TO_FD(f);
    ssize_t total = size * nmemb;
    ssize_t got = read(fd, ptr, (size_t)total);
    if (got <= 0) return 0;
    return (size_t)got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    size_t total = size * nmemb;
    if (f == stdout || f == stderr) {
        char buf[1025];
        size_t done = 0;
        while (done < total) {
            size_t chunk = total - done;
            if (chunk > 1024) chunk = 1024;
            memcpy(buf, (const char *)ptr + done, chunk);
            buf[chunk] = '\0';
            shim_output(buf, (f == stderr));
            done += chunk;
        }
        return nmemb;
    }
    return 0;
}

int fseek(FILE *f, long offset, int whence) {
    int fd = FILE_TO_FD(f);
    return (lseek(fd, (off_t)offset, whence) < 0) ? -1 : 0;
}

long ftell(FILE *f) {
    int fd = FILE_TO_FD(f);
    return (long)lseek(fd, 0, SEEK_CUR);
}

int fclose(FILE *f) {
    if (f == stdin || f == stdout || f == stderr) return 0;
    int fd = FILE_TO_FD(f);
    return close(fd);
}

int feof(FILE *f) {
    if (f == stdin) return 1;
    int fd = FILE_TO_FD(f);
    int slot = fd - FD_OFFSET;
    if (slot < 0 || slot >= FD_MAX || !fd_table[slot].used) return 1;
    return fd_table[slot].pos >= fd_table[slot].size;
}

int ferror(FILE *f) { (void)f; return 0; }
void clearerr(FILE *f) { (void)f; }

FILE *freopen(const char *path, const char *mode, FILE *f) {
    (void)path; (void)mode; (void)f;
    return NULL;
}

int ungetc(int c, FILE *f) {
    if (f == stdin) return EOF;
    int fd = FILE_TO_FD(f);
    int slot = fd - FD_OFFSET;
    if (slot < 0 || slot >= FD_MAX || !fd_table[slot].used) return EOF;
    if (fd_table[slot].pos > 0) fd_table[slot].pos--;
    return c;
}

int fgetc(FILE *f) {
    unsigned char c;
    if (fread(&c, 1, 1, f) == 1) return c;
    return EOF;
}

char *fgets(char *buf, int size, FILE *f) {
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

int remove(const char *path) { (void)path; errno = ENOSYS; return -1; }
int rename(const char *oldp, const char *newp) { (void)oldp; (void)newp; errno = ENOSYS; return -1; }
FILE *tmpfile(void) { return NULL; }
int unlink(const char *path) { (void)path; return 0; }
int ftruncate(int fd, off_t length) { (void)fd; (void)length; return 0; }

FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    if (fd < 0) return NULL;
    return FD_TO_FILE(fd);
}

char *realpath(const char *path, char *resolved) {
    if (!path) return NULL;
    if (resolved) {
        strcpy(resolved, path);
        return resolved;
    }
    return strdup(path);
}
```

The `FD_TO_FILE` / `FILE_TO_FD` trick avoids allocating real FILE structures. A FILE pointer of value 103 means fd 3. Since our real FILE objects (stdin, stdout, stderr) have addresses in the high memory region and our encoded values are small integers, there's no collision. It's ugly but effective.

## The Stubs

The final category: functions TCC references but that don't need real implementations. These are one-liners that return harmless defaults.

```c
/* ================================================================
 * Stubs
 * ================================================================ */

void exit(int status) {
    if (shim_exit_active) {
        shim_exit_code = status;
        longjmp(shim_exit_jmpbuf, status ? status : (int)0xE0E00E0E);
    }
    /* If no jmpbuf set, just hang */
    for (;;) { }
}

void abort(void) { exit(1); }

void _exit(int status) { exit(status); }
```

`exit` is the most important stub. When TCC encounters a fatal error, it calls `exit(1)`. In a normal process, this terminates the program. We can't do that — we ARE the program. Instead, `exit` uses `longjmp` to teleport back to the point where `setjmp` was called in our runtime wrapper (Chapter 14). The magic value `0xE0E00E0E` handles the edge case where someone calls `exit(0)` — `longjmp` can't return 0 (that's indistinguishable from `setjmp`'s initial return), so we pass a distinguishable value.

If `shim_exit_active` is false (no recovery point), we hang rather than corrupt the stack.

```c
char *getenv(const char *name) { (void)name; return NULL; }

time_t time(time_t *t) {
    if (t) *t = 0;
    return 0;
}

static struct tm zero_tm;
struct tm *localtime(const time_t *t) { (void)t; return &zero_tm; }

char *getcwd(char *buf, size_t size) {
    if (buf && size > 1) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return buf;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
    return 0;
}
```

No environment variables, no clock, no working directory. The filesystem root is `/`.

```c
/* Simple insertion sort for qsort */
void qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *)) {
    char *b = (char *)base;
    char *tmp = (char *)malloc(size);
    if (!tmp) return;
    for (size_t i = 1; i < nmemb; i++) {
        size_t j = i;
        while (j > 0 && compar(b + j * size, b + (j-1) * size) < 0) {
            memcpy(tmp, b + j * size, size);
            memcpy(b + j * size, b + (j-1) * size, size);
            memcpy(b + (j-1) * size, tmp, size);
            j--;
        }
    }
    free(tmp);
}
```

TCC calls `qsort` to sort symbol tables. Insertion sort is O(n^2), but TCC's symbol tables are small enough that it doesn't matter.

```c
char *strerror(int errnum) {
    (void)errnum;
    return (char *)"error";
}

int mprotect(void *addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    /* UEFI boot memory is RWX — nothing to do */
    return 0;
}

void *dlopen(const char *filename, int flags) {
    (void)filename; (void)flags;
    return NULL;
}

void *dlsym(void *handle, const char *symbol) {
    (void)handle; (void)symbol;
    return NULL;
}

char *dlerror(void) {
    return (char *)"dlopen not supported";
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}

sighandler_t signal(int signum, sighandler_t handler) {
    (void)signum; (void)handler;
    return SIG_DFL;
}

long sysconf(int name) {
    if (name == _SC_PAGESIZE) return 4096;
    return -1;
}

int isatty(int fd) { (void)fd; return 0; }

pid_t getpid(void) { return 1; }

unsigned int sleep(unsigned int seconds) {
    if (g_boot.bs) {
        g_boot.bs->Stall(seconds * 1000000ULL);
    }
    return 0;
}

/* Math stubs */
double ldexp(double x, int exp) {
    double factor = 1.0;
    int neg = 0;
    if (exp < 0) { neg = 1; exp = -exp; }
    for (int i = 0; i < exp; i++) factor *= 2.0;
    return neg ? x / factor : x * factor;
}

double frexp(double x, int *exp) {
    if (x == 0.0) { *exp = 0; return 0.0; }
    *exp = 0;
    int neg = 0;
    if (x < 0) { neg = 1; x = -x; }
    while (x >= 1.0) { x *= 0.5; (*exp)++; }
    while (x < 0.5) { x *= 2.0; (*exp)--; }
    return neg ? -x : x;
}
```

`mprotect` is a no-op because UEFI boot memory is already read-write-execute. `dlopen` always fails because we have no dynamic libraries. `sleep` wraps UEFI's `Stall` function (which takes microseconds). `ldexp` and `frexp` are minimal math functions — multiply/divide by powers of 2.

That's the complete shim implementation — 1129 lines. Most of it is straightforward C library code, but the important parts are the UEFI integrations: memory allocation through `AllocatePool`, file I/O through `fs_readfile`, output through `fb_print`, and `exit()` through `longjmp`.

## setjmp and longjmp

TCC uses `setjmp`/`longjmp` internally for error recovery. Our `exit()` implementation also uses them. We need an ARM64 implementation.

On ARM64, the calling convention specifies which registers a function must preserve across calls: x19 through x30 (general purpose), sp (stack pointer), and d8 through d15 (floating point). That's 13 general-purpose values and 8 floating-point values — 21 registers. We allocate 22 slots for alignment.

Create `src/setjmp.S`:

```asm
/*
 * setjmp.S — ARM64 setjmp/longjmp for UEFI
 *
 * jmp_buf layout (22 x 8 = 176 bytes):
 *   [0]  x19   [1]  x20   [2]  x21   [3]  x22
 *   [4]  x23   [5]  x24   [6]  x25   [7]  x26
 *   [8]  x27   [9]  x28   [10] x29(fp) [11] x30(lr)
 *   [12] sp
 *   [13] d8    [14] d9    [15] d10   [16] d11
 *   [17] d12   [18] d13   [19] d14   [20] d15
 *   [21] reserved
 */

    .text
    .align 4

    .global setjmp
    .type   setjmp, %function
setjmp:
    /* x0 = jmp_buf pointer */
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x2, sp
    str x2, [x0, #96]
    stp d8,  d9,  [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov x0, #0
    ret
    .size setjmp, . - setjmp

    .global longjmp
    .type   longjmp, %function
longjmp:
    /* x0 = jmp_buf, x1 = return value */
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2
    ldp d8,  d9,  [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    /* longjmp(buf, 0) must return 1 */
    cmp x1, #0
    csinc x0, x1, xzr, ne
    ret
    .size longjmp, . - longjmp
```

`setjmp` saves every register into the jmp_buf using `stp` (store pair) instructions, then returns 0. `longjmp` restores them all with `ldp` (load pair) and returns the value passed as its second argument.

The `csinc` instruction at the end of `longjmp` handles a C standard requirement: `longjmp(buf, 0)` must behave as if `setjmp` returned 1, not 0 (since 0 is how `setjmp` signals its initial return). `csinc x0, x1, xzr, ne` means: if x1 is nonzero, x0 = x1; if x1 is zero, x0 = xzr + 1 = 1.

## TCC Configuration

TCC needs a `config.h` that tells it about our target. Create `tools/tinycc/config.h`:

```c
/* TCC config for Survival Workstation (UEFI, multi-arch) */
#define TCC_VERSION "0.9.28"
#define ONE_SOURCE 1
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_PREDEFS 1
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define TCC_USING_DOUBLE_FOR_LDOUBLE 1
#define CONFIG_TCCDIR "/include"
```

Key settings:

- `ONE_SOURCE=1` makes TCC compile as a single translation unit — `libtcc.c` includes all other `.c` files. One source file in, one object file out.
- `CONFIG_TCC_STATIC=1` disables dynamic library loading (no `dlopen`).
- `CONFIG_TCC_PREDEFS=1` bakes built-in type definitions into the binary instead of loading them from disk at runtime.
- `TCC_USING_DOUBLE_FOR_LDOUBLE=1` treats `long double` as `double` — ARM64's long double is 128-bit quad precision, which we don't support.
- `CONFIG_TCCDIR="/include"` tells TCC where to find headers on the target (the FAT32 image).

Notice we don't set `TCC_TARGET_ARM64` here — the Makefile passes the target via `-D` flags (`-DTCC_TARGET_ARM64=1` or `-DTCC_TARGET_X86_64=1`). This keeps config.h architecture-neutral, which matters when we add x86_64 support in Phase 4.

That last setting — `CONFIG_TCC_PREDEFS` — is important. Without it, TCC tries to `#include <tccdefs.h>` every time it compiles — from the filesystem. We need to pre-generate the baked-in version:

```bash
cd tools/tinycc
gcc -DC2STR conftest.c -o c2str
./c2str include/tccdefs.h tccdefs_.h
```

This converts `tccdefs.h` into C string literals stored in `tccdefs_.h`, which get compiled directly into TCC's binary. The `c2str` tool is TCC's own — it reads a header file and outputs a C source file containing the header as a string constant.

## Building TCC

The Makefile needs new variables and rules for TCC. Add these after the existing `CFLAGS` and `LDFLAGS` definitions:

```makefile
# TinyCC as library
TCC_DIR  := tools/tinycc
TCC_CFLAGS := -ffreestanding -fno-stack-protector -fno-stack-check \
              -fshort-wchar -mstrict-align -fPIC -fPIE \
              -fno-strict-aliasing -fno-merge-all-constants \
              -Isrc/tcc-headers -I$(TCC_DIR) \
              -DONE_SOURCE=1 -DTCC_TARGET_ARM64=1 -w -Os
```

Notice the differences from our normal `CFLAGS`:

- `-Isrc/tcc-headers` instead of `-I$(EFI_INC)` — TCC gets our stubs, not gnu-efi headers.
- `-w` suppresses all warnings — TCC's code triggers hundreds of GCC warnings that we can't fix without forking.
- `-Os` optimizes for size — TCC is the largest single object and every kilobyte counts.
- No `-Wall -Wextra` — again, TCC's code is not warning-clean under modern GCC.

Add `shim.c` and `tcc.c` to the source list:

```makefile
SOURCES  := $(SRCDIR)/main.c $(SRCDIR)/fb.c $(SRCDIR)/kbd.c $(SRCDIR)/mem.c $(SRCDIR)/font.c \
            $(SRCDIR)/fs.c $(SRCDIR)/browse.c $(SRCDIR)/edit.c \
            $(SRCDIR)/shim.c $(SRCDIR)/tcc.c
```

And add the non-C objects:

```makefile
OBJECTS  += $(BUILDDIR)/setjmp.o $(BUILDDIR)/libtcc.o
```

Add the specific build rules. The `tcc.o` rule needs access to `libtcc.h`:

```makefile
# tcc.c needs access to libtcc.h
$(BUILDDIR)/tcc.o: $(SRCDIR)/tcc.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(TCC_DIR) -c -o $@ $<

# setjmp.S — our own setjmp/longjmp for TCC
$(BUILDDIR)/setjmp.o: $(SRCDIR)/setjmp.S
	@mkdir -p $(BUILDDIR)
	$(CC) -c -o $@ $<

# TCC unity build — compiled with shim headers, not gnu-efi
$(BUILDDIR)/libtcc.o: $(TCC_DIR)/libtcc.c $(TCC_DIR)/tcc.h $(TCC_DIR)/config.h
	@mkdir -p $(BUILDDIR)
	$(CC) $(TCC_CFLAGS) -c -o $@ $<
```

The first build takes a moment — `libtcc.c` is about 60,000 lines of code being compiled as a single unit. But it produces just one object file, `libtcc.o`, about 300KB.

## Executable Memory

There's a trap waiting at runtime. When TCC compiles code, it allocates memory for the machine code via `malloc()`. Our `malloc()` calls `mem_alloc()`, which calls UEFI's `AllocatePool()`. By default, `mem_alloc` requests `EfiLoaderData` memory. On newer UEFI firmware (including QEMU's OVMF), `EfiLoaderData` pages are marked **NX** — No-eXecute.

The compiled code is valid. The instructions are correct. But the CPU refuses to execute them because the memory permissions forbid it. On ARM64, this manifests as a Synchronous Exception at the exact address of the compiled function — the first instruction never runs.

The fix is one word in `src/mem.c`. Change `EfiLoaderData` to `EfiLoaderCode`:

```c
void *mem_alloc(UINTN size) {
    void *ptr = NULL;
    /* Use EfiLoaderCode so TCC's JIT-compiled code is executable.
       EfiLoaderData pages may be NX on newer UEFI firmware. */
    EFI_STATUS status = g_boot.bs->AllocatePool(EfiLoaderCode, size, &ptr);
    if (EFI_ERROR(status))
        return NULL;
    mem_set(ptr, 0, size);
    return ptr;
}
```

`EfiLoaderCode` memory is marked executable. Both types are reclaimed at `ExitBootServices`, so there's no downside to using code memory for everything. This single change makes the difference between a working JIT compiler and a cryptic crash.

## Filesystem Initialization

TCC's file I/O shim calls `fs_readfile` from Chapter 9. But `fs_readfile` requires the filesystem to be initialized first — the root directory handle `s_root` must be set. In `src/main.c`, add `fs_init()` to the initialization sequence if it isn't there already:

```c
/* Initialize subsystems */
mem_init();
fs_init();

status = fb_init();
```

Without this, TCC's first `open()` call crashes — `fs_readfile` dereferences a NULL root handle.

## The First Test

Before wiring TCC into the editor (that's Chapter 14), we test it standalone. Add a self-test function to `main.c`:

```c
#include "tcc.h"

/* Compiler self-test: compile "return 42" and verify */
static void tcc_selftest(void) {
    con_print(L"TCC self-test: ");

    struct tcc_result r = tcc_run_source(
        "int main(void) { return 42; }\n", "selftest.c");

    if (r.success && r.exit_code == 42)
        con_print(L"PASS (return 42)\r\n");
    else
        con_print(L"FAIL\r\n");
}
```

Call it from both `console_loop` and `fb_loop` after the banner prints. The `tcc_run_source` function will be implemented in Chapter 14, but for now, here's a minimal version to test compilation. Create `src/tcc.h`:

```c
#ifndef TCC_WRAPPER_H
#define TCC_WRAPPER_H

#include "boot.h"

struct tcc_result {
    int  success;       /* 1 = ok, 0 = error */
    char error_msg[2048];
    int  exit_code;
};

/* Compile and run C source in memory. Returns result. */
struct tcc_result tcc_run_source(const char *source, const char *filename);

#endif /* TCC_WRAPPER_H */
```

And a bare-bones `src/tcc.c` — just enough to prove the compiler works:

```c
#include "boot.h"
#include "mem.h"
#include "shim.h"
#include "tcc.h"
#include "libtcc.h"

struct tcc_result tcc_run_source(const char *source, const char *filename) {
    struct tcc_result result;
    mem_set(&result, 0, sizeof(result));

    TCCState *tcc = tcc_new();
    if (!tcc) {
        strcpy(result.error_msg, "Failed to create TCC context");
        return result;
    }

    tcc_set_options(tcc, "-nostdlib -nostdinc");
    tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);

    if (tcc_compile_string(tcc, source) < 0) {
        tcc_delete(tcc);
        return result;
    }

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

    result.exit_code = prog_main();
    result.success = 1;

    tcc_delete(tcc);
    return result;
}
```

Build and run:

```bash
make clean && make
./scripts/run-qemu.sh console
```

Watch the serial output:

```
SURVIVAL WORKSTATION: Booting...
No framebuffer, falling back to console.

  ========================================
       SURVIVAL WORKSTATION v0.1
  ========================================

  Type anything. Press ESC to shutdown.
  ----------------------------------------

> TCC self-test: PASS (return 42)
```

TinyCC compiled `int main(void) { return 42; }` to ARM64 machine code in memory, and our firmware called the resulting function pointer. The return value was 42. The C compiler works.

## What Just Happened

Let's trace the full path of that test:

1. `tcc_run_source` creates a new TCC compiler instance
2. `-nostdlib -nostdinc` prevents TCC from searching for libraries and system headers that don't exist
3. `tcc_compile_string` runs the preprocessor, parser, and ARM64 code generator on our source string
4. `tcc_relocate` resolves all symbol references and writes the final machine code into memory allocated via our `malloc` → UEFI's `AllocatePool(EfiLoaderCode, ...)`
5. `tcc_get_symbol("main")` returns the address of the generated `main` function
6. We cast it to a function pointer and call it — the CPU jumps to freshly generated ARM64 instructions
7. The compiled function executes `mov w0, #42; ret`, returns 42
8. We verify the return value and print PASS

Eight chapters of infrastructure converged into that call: UEFI boot (Ch. 4), memory allocation (Ch. 5), console output (Ch. 4), filesystem (Ch. 9), and now the shim layer and compiler.

## The Binary

```
$ ls -lh build/survival.efi
-rwxrwxr-x 1 min min 632K ... build/survival.efi
```

632 kilobytes. That's a bootloader, framebuffer driver, keyboard input, filesystem, file browser, text editor, and a C compiler that generates and executes ARM64 machine code in memory. It fits on a floppy disk with room to spare.

In the next chapter, we'll flesh out `tcc.c` into a proper runtime wrapper — with error capture, API registration, exit handling — and wire F5 in the editor to compile-and-run. The workstation will go from "can compile return 42" to "user writes code and runs it."
