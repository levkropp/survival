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
/* Use long long for 64-bit: safe on both LP64 and LLP64 (TCC PE x86_64) */
typedef unsigned char      uint8_t;
typedef signed   char      int8_t;
typedef unsigned short     uint16_t;
typedef signed   short     int16_t;
typedef unsigned int       uint32_t;
typedef signed   int       int32_t;
typedef unsigned long long uint64_t;
typedef signed   long long int64_t;
typedef unsigned long long uintptr_t;
typedef signed   long long intptr_t;

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

/* ---- setjmp / longjmp (assembly in setjmp_{arch}.S) ---- */
#ifdef __aarch64__
typedef long long jmp_buf[22];  /* 176 bytes: x19-x30, sp, d8-d15 */
#elif defined(__x86_64__)
typedef long long jmp_buf[10];  /* 80 bytes: rbx, rbp, rdi, rsi, r12-r15, rsp, ret addr */
#endif
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* ---- offsetof ---- */
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif /* _EFI_INCLUDE_ */

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
void *memchr(const void *s, int c, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strtok(char *s, const char *delim);

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
void  *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *));
int    rand(void);
void   srand(unsigned int seed);
#ifndef RAND_MAX
#define RAND_MAX 2147483647
#endif
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
