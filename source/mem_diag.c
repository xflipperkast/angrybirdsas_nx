#include "mem_diag.h"
#include <switch.h>

uint64_t mem_diag_process_free_bytes(void){
    uint64_t total=0,used=0;
    if(R_FAILED(svcGetInfo(&total,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0)))return 0;
    if(R_FAILED(svcGetInfo(&used,InfoType_UsedMemorySize,CUR_PROCESS_HANDLE,0)))return 0;
    return total>used?total-used:0;
}

void mem_diag_snapshot(const char *tag){(void)tag;}
void mem_diag_allocator_probe(const char *tag){(void)tag;}
