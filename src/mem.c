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
