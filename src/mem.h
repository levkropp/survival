#ifndef MEM_H
#define MEM_H

#include "boot.h"

/* Initialize the memory allocator (call after boot_state is set up) */
void mem_init(void);

/* Simple allocator using UEFI AllocatePool */
void *mem_alloc(UINTN size);
void mem_free(void *ptr);

/* Allocate/free executable memory in the lower address range.
   Uses AllocatePages(AllocateMaxAddress) below 2GB so TCC-generated
   code can reach workstation symbols via RIP-relative addressing. */
void *mem_alloc_code(UINTN size);
void mem_free_code(void *ptr, UINTN size);

/* Utility */
void mem_set(void *dst, UINT8 val, UINTN size);
void mem_copy(void *dst, const void *src, UINTN size);
UINTN str_len(const CHAR8 *s);
int str_cmp(const CHAR8 *a, const CHAR8 *b);
void str_copy(char *dst, const char *src, UINTN max);

#endif /* MEM_H */
