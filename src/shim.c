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

/* no-op on UEFI — only needed for JIT, not PE file output */
void __clear_cache(void *beg, void *end) { (void)beg; (void)end; }

__attribute__((weak))
void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (uint8_t)c) return (void *)(p + i);
    }
    return NULL;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

/* libgcc builtins needed by TCC-compiled code.
 * Use pure bit manipulation to avoid recursive calls to ourselves. */
unsigned long long __fixunsdfdi(double a) {
    union { double d; unsigned long long u; } v;
    v.d = a;
    if (v.u >> 63) return 0;           /* negative */
    int exp = ((v.u >> 52) & 0x7FF) - 1023;
    if (exp < 0) return 0;
    if (exp > 63) return ~0ULL;
    unsigned long long m = (v.u & 0x000FFFFFFFFFFFFFULL) | 0x0010000000000000ULL;
    if (exp >= 52) return m << (exp - 52);
    return m >> (52 - exp);
}

double __floatundidf(unsigned long long a) {
    union { unsigned long long u; double d; } v;
    if (a == 0) { v.u = 0; return v.d; }
    /* Normalize: shift left so bit 63 is set */
    int e = 63;
    unsigned long long t = a;
    while (!(t & (1ULL << 63))) { t <<= 1; e--; }
    /* Mantissa: bits 62..11 (52 bits, implicit leading 1 removed) */
    unsigned long long mantissa = (t >> 11) & 0x000FFFFFFFFFFFFFULL;
    v.u = ((unsigned long long)(e + 1023) << 52) | mantissa;
    return v.d;
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

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    for (; *s; s++) {
        const char *a = accept;
        int found = 0;
        while (*a) { if (*s == *a++) { found = 1; break; } }
        if (!found) break;
        count++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    for (; *s; s++) {
        const char *r = reject;
        while (*r) { if (*s == *r++) return count; }
        count++;
    }
    return count;
}

char *strtok(char *s, const char *delim) {
    static char *saved;
    if (s) saved = s;
    if (!saved) return NULL;
    /* Skip leading delimiters */
    saved += strspn(saved, delim);
    if (*saved == '\0') { saved = NULL; return NULL; }
    char *token = saved;
    saved += strcspn(saved, delim);
    if (*saved) *saved++ = '\0';
    else saved = NULL;
    return token;
}

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

/* ================================================================
 * Formatted output — vsnprintf
 * ================================================================ */

/* Helper: write a single char to buffer */
static void sn_putc(char *buf, size_t size, size_t *pos, char c) {
    if (*pos < size - 1) buf[*pos] = c;
    (*pos)++;
}

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

/* Helper: format a double value as %f, %e, or %g */
static void sn_float(char *buf, size_t size, size_t *pos,
                     double val, int prec, int width, int zero_pad,
                     int left_align, char fmt_ch) {
    char tmp[80];
    int len = 0;

    if (prec < 0) prec = 6;

    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }

    if (fmt_ch == 'e' || fmt_ch == 'E') {
        /* Scientific notation */
        int exponent = 0;
        if (val != 0.0) {
            while (val >= 10.0) { val /= 10.0; exponent++; }
            while (val < 1.0 && val > 0.0) { val *= 10.0; exponent--; }
        }
        /* Format mantissa as fixed point with 1 digit before decimal */
        if (neg) tmp[len++] = '-';
        /* Integer part (single digit) */
        int ipart = (int)val;
        tmp[len++] = '0' + (ipart % 10);
        if (prec > 0) {
            tmp[len++] = '.';
            double frac = val - (double)ipart;
            for (int i = 0; i < prec; i++) {
                frac *= 10.0;
                int d = (int)frac;
                if (d > 9) d = 9;
                tmp[len++] = '0' + d;
                frac -= (double)d;
            }
        }
        /* Exponent */
        tmp[len++] = fmt_ch;
        tmp[len++] = (exponent < 0) ? '-' : '+';
        if (exponent < 0) exponent = -exponent;
        if (exponent < 10) tmp[len++] = '0';
        if (exponent >= 100) { tmp[len++] = '0' + (exponent / 100); exponent %= 100; }
        if (exponent >= 10) { tmp[len++] = '0' + (exponent / 10); exponent %= 10; }
        tmp[len++] = '0' + exponent;
    } else {
        /* %f — fixed point */
        if (neg) tmp[len++] = '-';
        /* Integer part */
        unsigned long long ipart = (unsigned long long)val;
        char itmp[24]; int ilen = 0;
        if (ipart == 0) itmp[ilen++] = '0';
        else while (ipart > 0) { itmp[ilen++] = '0' + (int)(ipart % 10); ipart /= 10; }
        for (int i = ilen - 1; i >= 0; i--) tmp[len++] = itmp[i];
        if (prec > 0) {
            tmp[len++] = '.';
            double frac = val - (double)(unsigned long long)val;
            for (int i = 0; i < prec && len < 78; i++) {
                frac *= 10.0;
                int d = (int)frac;
                if (d > 9) d = 9;
                tmp[len++] = '0' + d;
                frac -= (double)d;
            }
        }
    }
    tmp[len] = '\0';

    /* %g: strip trailing zeros */
    if (fmt_ch == 'g' || fmt_ch == 'G') {
        char *dot = NULL;
        for (int i = 0; i < len; i++) if (tmp[i] == '.') { dot = &tmp[i]; break; }
        if (dot) {
            char *end = tmp + len - 1;
            while (end > dot && *end == '0') end--;
            if (end == dot) end--; /* remove dot too */
            len = (int)(end - tmp + 1);
            tmp[len] = '\0';
        }
    }

    int pad = (width > len) ? width - len : 0;
    if (!left_align) {
        char pc = zero_pad ? '0' : ' ';
        for (int i = 0; i < pad; i++) sn_putc(buf, size, pos, pc);
    }
    for (int i = 0; i < len; i++) sn_putc(buf, size, pos, tmp[i]);
    if (left_align) {
        for (int i = 0; i < pad; i++) sn_putc(buf, size, pos, ' ');
    }
}

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
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double val = va_arg(ap, double);
            char fc = *fmt;
            if (fc == 'g' || fc == 'G') fc = 'f'; /* %g uses %f format, then strips zeros */
            sn_float(out, out_size, &pos, val, prec, width, zero_pad, left_align,
                     (*fmt == 'g' || *fmt == 'G') ? *fmt : fc);
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

/* ================================================================
 * File I/O shim — fd-based
 * ================================================================ */

#define FD_MAX 64
#define FD_OFFSET 3  /* skip stdin=0, stdout=1, stderr=2 */

struct fd_slot {
    char   *data;
    size_t  size;
    size_t  pos;
    size_t  cap;       /* buffer capacity (for writable fds) */
    int     used;
    int     writable;
    CHAR16  wpath[512]; /* path for write-back on close */
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
    if (!path) { errno = EINVAL; return -1; }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < FD_MAX; i++) {
        if (!fd_table[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        errno = EMFILE; return -1;
    }

    CHAR16 upath[512];
    path_to_uefi(path, upath, 512);

    if (flags & (O_WRONLY | O_CREAT)) {
        /* Write mode: start with empty growable buffer */
        fd_table[slot].data = (char *)mem_alloc(4096);
        fd_table[slot].size = 0;
        fd_table[slot].cap = 4096;
        fd_table[slot].pos = 0;
        fd_table[slot].used = 1;
        fd_table[slot].writable = 1;
        memcpy(fd_table[slot].wpath, upath, 512 * sizeof(CHAR16));
        return slot + FD_OFFSET;
    }

    /* Read mode */
    UINTN fsize = 0;
    char *data = (char *)fs_readfile(upath, &fsize);
    if (!data) {
        errno = ENOENT; return -1;
    }

    fd_table[slot].data = data;
    fd_table[slot].size = fsize;
    fd_table[slot].pos = 0;
    fd_table[slot].used = 1;
    fd_table[slot].writable = 0;

    return slot + FD_OFFSET;
}

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
    /* Regular file write */
    int slot = fd - FD_OFFSET;
    if (slot >= 0 && slot < FD_MAX && fd_table[slot].used && fd_table[slot].writable) {
        struct fd_slot *f = &fd_table[slot];
        size_t need = f->pos + count;
        if (need > f->cap) {
            size_t new_cap = f->cap * 2;
            if (new_cap < need) new_cap = need;
            char *new_data = (char *)mem_alloc(new_cap);
            if (f->size > 0) memcpy(new_data, f->data, f->size);
            mem_free(f->data);
            f->data = new_data;
            f->cap = new_cap;
        }
        memcpy(f->data + f->pos, buf, count);
        f->pos += count;
        if (f->pos > f->size) f->size = f->pos;
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
    /* Flush writable files to filesystem */
    if (fd_table[slot].writable && fd_table[slot].data && fd_table[slot].size > 0) {
        fs_writefile(fd_table[slot].wpath, fd_table[slot].data, fd_table[slot].size);
    }
    if (fd_table[slot].data) {
        mem_free(fd_table[slot].data);
    }
    memset(&fd_table[slot], 0, sizeof(struct fd_slot));
    return 0;
}

/* ================================================================
 * FILE-based I/O wrappers
 * ================================================================ */

/* We encode fd into the FILE pointer: FILE * = (FILE *)(uintptr_t)(fd + 100) */
#define FILE_TO_FD(f)  ((int)((uintptr_t)(f) - 100))
#define FD_TO_FILE(fd) ((FILE *)(uintptr_t)((fd) + 100))

FILE *fopen(const char *path, const char *mode) {
    int flags = O_RDONLY;
    if (mode && (mode[0] == 'w' || mode[0] == 'a'))
        flags = O_WRONLY | O_CREAT;
    int fd = open(path, flags);
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
    int fd = FILE_TO_FD(f);
    ssize_t wrote = write(fd, ptr, total);
    if (wrote <= 0) return 0;
    return (size_t)wrote / size;
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

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const char *b = (const char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compar(key, b + mid * size);
        if (cmp < 0) hi = mid;
        else if (cmp > 0) lo = mid + 1;
        else return (void *)(b + mid * size);
    }
    return NULL;
}

static unsigned int rand_seed = 1;

void srand(unsigned int seed) { rand_seed = seed; }

int rand(void) {
    /* Park-Miller LCG */
    rand_seed = rand_seed * 1103515245U + 12345U;
    return (int)((rand_seed >> 16) & 0x7fffffff);
}

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
