#ifndef ASNX_RUNTIME_H
#define ASNX_RUNTIME_H
#include "so_util.h"
extern so_module main_mod, unity_mod, il2cpp_mod;
int runtime_load_modules(void);
void runtime_free_temp_images(void);
size_t runtime_heap_total(void);
size_t runtime_newlib_heap_size(void);
#endif
