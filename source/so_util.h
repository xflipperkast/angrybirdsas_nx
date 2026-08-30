#ifndef ASNX_SO_UTIL_H
#define ASNX_SO_UTIL_H
#include <stdint.h>
#include <stddef.h>
#include <elf.h>
#define ALIGN_MEM(x,a) (((x)+((a)-1)) & ~((a)-1))
#define SO_MAX_SEGMENTS 16

typedef struct { const char *symbol; uintptr_t func; } DynLibFunction;
typedef struct so_module {
    struct so_module *next;
    char name[96];
    void *load_base, *load_virtbase;
    size_t load_size;
    void *load_memrv;
    Elf64_Phdr phdr[SO_MAX_SEGMENTS]; int phnum;
    void *so_base; size_t so_size;
    Elf64_Ehdr *elf_hdr; Elf64_Phdr *prog_hdr; Elf64_Shdr *sec_hdr;
    Elf64_Sym *syms; int num_syms; char *shstrtab, *dynstrtab;
} so_module;

int so_load(so_module *m,const char *filename,void *base,size_t max_size);
int so_relocate(so_module *m);
int so_resolve(so_module *m,DynLibFunction *funcs,int n,uintptr_t fallback,uintptr_t (*extra_lookup)(const char*));
void so_finalize(so_module *m);
void so_execute_init_array(so_module *m);
void so_free_temp(so_module *m);
uintptr_t so_try_find_addr_rx(so_module *m,const char *symbol);
void *so_resolve_external(const char *name);
so_module *so_find_named(const char *needle);
int so_dl_iterate_phdr(int (*callback)(void*,size_t,void*),void *data);
void so_flush_caches(so_module *m);
int so_check_build_id(so_module *m,const char *expected);
uint32_t so_read_word(so_module *m,size_t offset);
int so_patch_code(so_module *m,size_t offset,const void *data,size_t size);
#endif
