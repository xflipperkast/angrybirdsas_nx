#include "guest_stack.h"
#include "bionic.h"
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void *g_stack;
static size_t g_stack_size;

#if defined(__aarch64__)

extern intptr_t guest_stack_call_asm(void *stack_top, guest_stack_fn fn, void *arg);

static intptr_t call_with_stack(void *stack_top, guest_stack_fn fn, void *arg) {
    return guest_stack_call_asm(stack_top, fn, arg);
}
#else
static intptr_t call_with_stack(void *stack_top, guest_stack_fn fn, void *arg) {
    (void)stack_top;
    return fn(arg);
}
#endif

int guest_stack_init(size_t bytes) {
    if (g_stack) return 0;
    if (bytes < (2u * 1024u * 1024u)) bytes = 2u * 1024u * 1024u;
    bytes = (bytes + 0xfffu) & ~(size_t)0xfffu;
    g_stack = memalign(0x1000, bytes);
    if (!g_stack) {
        return -1;
    }
    memset(g_stack, 0, bytes);
    g_stack_size = bytes;
    bionic_set_main_stack_range(g_stack, g_stack_size);
    return 0;
}

void guest_stack_shutdown(void) {
    if (!g_stack) return;
    bionic_set_main_stack_range(NULL, 0);
    free(g_stack);
    g_stack = NULL;
    g_stack_size = 0;
}

intptr_t guest_stack_call(guest_stack_fn fn, void *arg) {
    if (!fn) return -1;
    if (!g_stack || g_stack_size < 0x1000) return fn(arg);
    uintptr_t top = (uintptr_t)g_stack + g_stack_size;
    top &= ~(uintptr_t)0xf;
    return call_with_stack((void *)top, fn, arg);
}

void *guest_stack_base(void) { return g_stack; }
size_t guest_stack_size(void) { return g_stack_size; }
