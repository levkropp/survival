/*
 * setjmp_x86_64.S — x86_64 setjmp/longjmp for UEFI (MS ABI)
 *
 * jmp_buf layout (10 × 8 = 80 bytes):
 *   [0] rbx   [1] rbp   [2] rdi   [3] rsi
 *   [4] r12   [5] r13   [6] r14   [7] r15
 *   [8] rsp   [9] return address
 *
 * MS x64 ABI callee-saved: rbx, rbp, rdi, rsi, r12-r15
 * First arg in rcx, second in edx.
 */

    .text
    .align 16

    .global setjmp
    .type   setjmp, @function
setjmp:
    /* rcx = jmp_buf pointer */
    movq %rbx,   (%rcx)
    movq %rbp,  8(%rcx)
    movq %rdi, 16(%rcx)
    movq %rsi, 24(%rcx)
    movq %r12, 32(%rcx)
    movq %r13, 40(%rcx)
    movq %r14, 48(%rcx)
    movq %r15, 56(%rcx)
    leaq 8(%rsp), %rax      /* rsp before call (skip return address) */
    movq %rax, 64(%rcx)
    movq (%rsp), %rax        /* return address */
    movq %rax, 72(%rcx)
    xorl %eax, %eax          /* return 0 */
    ret
    .size setjmp, . - setjmp

    .global longjmp
    .type   longjmp, @function
longjmp:
    /* rcx = jmp_buf, edx = return value */
    movq   (%rcx), %rbx
    movq  8(%rcx), %rbp
    movq 16(%rcx), %rdi
    movq 24(%rcx), %rsi
    movq 32(%rcx), %r12
    movq 40(%rcx), %r13
    movq 48(%rcx), %r14
    movq 56(%rcx), %r15
    movq 64(%rcx), %rsp
    movq 72(%rcx), %r8       /* return address */
    /* longjmp(buf, 0) must return 1 */
    movl %edx, %eax
    testl %eax, %eax
    jnz 1f
    movl $1, %eax
1:
    jmp *%r8
    .size longjmp, . - longjmp
