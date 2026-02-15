#include "mem.h"

void mem_init(void) {
    /* Nothing to do — we use UEFI AllocatePool directly */
}

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

void mem_free(void *ptr) {
    if (ptr)
        g_boot.bs->FreePool(ptr);
}

void *mem_alloc_code(UINTN size) {
    UINTN pages = (size + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS addr = 0x7FFFFFFF; /* below 2GB */
    EFI_STATUS status = g_boot.bs->AllocatePages(
        AllocateMaxAddress, EfiLoaderCode, pages, &addr);
    if (EFI_ERROR(status))
        return NULL;
    mem_set((void *)(UINTN)addr, 0, pages * 4096);
    return (void *)(UINTN)addr;
}

void mem_free_code(void *ptr, UINTN size) {
    if (!ptr) return;
    UINTN pages = (size + 4095) / 4096;
    g_boot.bs->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)ptr, pages);
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

int str_cmp(const CHAR8 *a, const CHAR8 *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(UINT8)*a - (int)(UINT8)*b;
}

void str_copy(char *dst, const char *src, UINTN max) {
    UINTN i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}
