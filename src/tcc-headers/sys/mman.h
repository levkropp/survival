/* stub — UEFI has no mman.h, shim.h provides the actual declarations */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED  ((void *)-1)
#endif
