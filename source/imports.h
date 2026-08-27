#ifndef ASNX_IMPORTS_H
#define ASNX_IMPORTS_H
#include <stdint.h>
#include "so_util.h"
void imports_init(void);
uintptr_t imports_lookup(const char *name);
int imports_resolve_module(so_module *m);
uintptr_t imports_fallback(void);
#endif
