#include "mem_diag.h"
#include "runtime.h"
#include "bionic.h"
#include "trace_log.h"
#include <switch.h>

uint64_t mem_diag_process_free_bytes(void){
    uint64_t total=0,used=0;
    if(R_FAILED(svcGetInfo(&total,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0)))return 0;
    if(R_FAILED(svcGetInfo(&used,InfoType_UsedMemorySize,CUR_PROCESS_HANDLE,0)))return 0;
    return total>used?total-used:0;
}

void mem_diag_snapshot(const char *tag){
    uint64_t total=0,used=0;
    (void)svcGetInfo(&total,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0);
    (void)svcGetInfo(&used,InfoType_UsedMemorySize,CUR_PROCESS_HANDLE,0);
    BionicMmapStats mm;
    bionic_get_mmap_stats(&mm);
    trace_log_printf("MEM",
        "tag=%s process_used=%lluMB process_total=%lluMB free=%lluMB newlib=%lluMB unity_arena=%u/%u peak=%u fallbacks=%u mmap_records=%u backing=%lluMB live=%lluMB",
        tag?tag:"?",
        (unsigned long long)(used>>20),(unsigned long long)(total>>20),
        (unsigned long long)((total>used?total-used:0)>>20),
        (unsigned long long)(runtime_newlib_heap_size()>>20),
        mm.used_slots,mm.total_slots,mm.peak_slots,mm.fallback_count,mm.live_records,
        (unsigned long long)(mm.backing_bytes>>20),(unsigned long long)(mm.live_bytes>>20));
}

void mem_diag_allocator_probe(const char *tag){mem_diag_snapshot(tag);}
