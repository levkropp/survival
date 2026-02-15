/*
 * setjmp.S — ARM64 setjmp/longjmp for UEFI
 *
 * jmp_buf layout (22 × 8 = 176 bytes):
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
