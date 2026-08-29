#ifndef ASNX_GUEST_STACK_H
#define ASNX_GUEST_STACK_H

#include <stddef.h>
#include <stdint.h>

typedef intptr_t (*guest_stack_fn)(void *arg);

int guest_stack_init(size_t bytes);
void guest_stack_shutdown(void);
intptr_t guest_stack_call(guest_stack_fn fn, void *arg);
void *guest_stack_base(void);
size_t guest_stack_size(void);

#endif
