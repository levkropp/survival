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
