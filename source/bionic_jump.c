#include "bionic.h"
#include <stdlib.h>

#if defined(__aarch64__)

__asm__(
    ".text\n"
    ".align 2\n"
    ".global bionic_setjmp\n"
    ".type bionic_setjmp, %function\n"
    "bionic_setjmp:\n"
    "    stp x19, x20, [x0, #0]\n"
    "    stp x21, x22, [x0, #16]\n"
    "    stp x23, x24, [x0, #32]\n"
    "    stp x25, x26, [x0, #48]\n"
    "    stp x27, x28, [x0, #64]\n"
    "    stp x29, x30, [x0, #80]\n"
    "    mov x2, sp\n"
    "    str x2, [x0, #96]\n"
    "    stp d8, d9, [x0, #104]\n"
    "    stp d10, d11, [x0, #120]\n"
    "    stp d12, d13, [x0, #136]\n"
    "    stp d14, d15, [x0, #152]\n"
    "    str x18, [x0, #168]\n"
    "    mov w0, wzr\n"
    "    ret\n"
    ".size bionic_setjmp, .-bionic_setjmp\n"

    ".align 2\n"
    ".global bionic_longjmp\n"
    ".type bionic_longjmp, %function\n"
    "bionic_longjmp:\n"
    "    ldr x3, [x0, #96]\n"
    "    ldp d8, d9, [x0, #104]\n"
    "    ldp d10, d11, [x0, #120]\n"
    "    ldp d12, d13, [x0, #136]\n"
    "    ldp d14, d15, [x0, #152]\n"
    "    ldr x18, [x0, #168]\n"
    "    ldp x19, x20, [x0, #0]\n"
    "    ldp x21, x22, [x0, #16]\n"
    "    ldp x23, x24, [x0, #32]\n"
    "    ldp x25, x26, [x0, #48]\n"
    "    ldp x27, x28, [x0, #64]\n"
    "    ldp x29, x30, [x0, #80]\n"
    "    mov sp, x3\n"
    "    cmp w1, #0\n"
    "    mov w2, #1\n"
    "    csel w0, w1, w2, ne\n"
    "    ret\n"
    ".size bionic_longjmp, .-bionic_longjmp\n"
);

#else

int bionic_setjmp(void *env) {
    (void)env;
    return 0;
}

void bionic_longjmp(void *env, int value) {
    (void)env;
    (void)value;
    abort();
}
#endif
