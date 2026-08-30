#ifndef ASNX_RUNTIME_H
#define ASNX_RUNTIME_H
#include <switch.h>
#include "so_util.h"
extern so_module main_mod, unity_mod, il2cpp_mod;
int runtime_load_modules(void);
void runtime_free_temp_images(void);
size_t runtime_heap_total(void);
size_t runtime_newlib_heap_size(void);
size_t runtime_so_arena_size(void);
size_t runtime_unity_mmap_arena_size(void);
int runtime_heap_from_override(void);
size_t runtime_loader_heap_prefix_size(void);
size_t runtime_host_headroom_released(void);
int runtime_headroom_release_status(void);
Result runtime_headroom_release_result(void);
#endif
