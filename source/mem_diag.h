#ifndef ASNX_MEM_DIAG_H
#define ASNX_MEM_DIAG_H
#include <stdint.h>

/* v0.1.75 compatibility stubs: retained for source compatibility only. */
uint64_t mem_diag_process_free_bytes(void);
void mem_diag_snapshot(const char *tag);
void mem_diag_allocator_probe(const char *tag);

#endif
