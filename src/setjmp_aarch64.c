/*
 * setjmp/longjmp for ARM64 — pre-assembled opcodes for TCC
 *
 * TCC's ARM64 assembler is a stub, so we embed raw machine code.
 * The arrays are named setjmp/longjmp directly so the linker resolves
 * calls to the opcode bytes themselves (not a pointer indirection).
 * Must be in .text so the memory is executable.
 *
 * jmp_buf layout: 22 x 8 = 176 bytes (see setjmp_aarch64.S)
 */

__attribute__((section(".text")))
unsigned int setjmp[] = {
    0xa9005013, /* stp x19, x20, [x0]       */
    0xa9015815, /* stp x21, x22, [x0, #16]  */
    0xa9026017, /* stp x23, x24, [x0, #32]  */
    0xa9036819, /* stp x25, x26, [x0, #48]  */
    0xa904701b, /* stp x27, x28, [x0, #64]  */
    0xa905781d, /* stp x29, x30, [x0, #80]  */
    0x910003e2, /* mov x2, sp               */
    0xf9003002, /* str x2, [x0, #96]        */
    0x6d06a408, /* stp d8,  d9,  [x0, #104] */
    0x6d07ac0a, /* stp d10, d11, [x0, #120] */
    0x6d08b40c, /* stp d12, d13, [x0, #136] */
    0x6d09bc0e, /* stp d14, d15, [x0, #152] */
    0xd2800000, /* mov x0, #0               */
    0xd65f03c0, /* ret                       */
};

__attribute__((section(".text")))
unsigned int longjmp[] = {
    0xa9405013, /* ldp x19, x20, [x0]       */
    0xa9415815, /* ldp x21, x22, [x0, #16]  */
    0xa9426017, /* ldp x23, x24, [x0, #32]  */
    0xa9436819, /* ldp x25, x26, [x0, #48]  */
    0xa944701b, /* ldp x27, x28, [x0, #64]  */
    0xa945781d, /* ldp x29, x30, [x0, #80]  */
    0xf9403002, /* ldr x2, [x0, #96]        */
    0x9100005f, /* mov sp, x2               */
    0x6d46a408, /* ldp d8,  d9,  [x0, #104] */
    0x6d47ac0a, /* ldp d10, d11, [x0, #120] */
    0x6d48b40c, /* ldp d12, d13, [x0, #136] */
    0x6d49bc0e, /* ldp d14, d15, [x0, #152] */
    0xf100003f, /* cmp x1, #0               */
    0x9a9f1420, /* csinc x0, x1, xzr, ne    */
    0xd65f03c0, /* ret                       */
};
