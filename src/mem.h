#ifndef MEM_H
#define MEM_H

#include "boot.h"

/* Initialize the memory allocator (call after boot_state is set up) */
void mem_init(void);

/* Simple allocator using UEFI AllocatePool */
void *mem_alloc(UINTN size);
void mem_free(void *ptr);

/* Utility */
void mem_set(void *dst, UINT8 val, UINTN size);
void mem_copy(void *dst, const void *src, UINTN size);
UINTN str_len(const CHAR8 *s);

#endif /* MEM_H */
