#include "runtime.h"
#include "config.h"
#include "imports.h"
#include "bionic.h"
#include <switch.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

so_module main_mod, unity_mod, il2cpp_mod;
static void *g_so_arena;
static size_t g_so_arena_size;
static void *g_unity_mmap_arena;
static size_t g_unity_mmap_arena_size;
static size_t g_heap_total;
static size_t g_newlib_heap_size;
static int g_heap_from_override;
static size_t g_loader_heap_prefix_size;
static size_t g_host_headroom_released;
static int g_headroom_release_status; /* 0=not attempted, 1=success, -1=validation, -2=svc failure */
static Result g_headroom_release_result;

static int runtime_find_loader_heap_base(uintptr_t override_lo, uintptr_t override_hi, uintptr_t *out_base) {
    if(!override_lo || override_hi<=override_lo || !out_base) return -1;
    MemoryInfo mi; u32 pi=0;
    if(R_FAILED(svcQueryMemory(&mi,&pi,override_lo)) || ((mi.type & 0xffu) != MemType_Heap)) return -1;
    uintptr_t base=(uintptr_t)mi.addr;
    for(unsigned guard=0; base>0 && guard<4096u; guard++) {
        MemoryInfo prev; pi=0;
        if(R_FAILED(svcQueryMemory(&prev,&pi,base-1u))) break;
        uintptr_t prev_end=(uintptr_t)prev.addr+(uintptr_t)prev.size;
        if(((prev.type & 0xffu) != MemType_Heap) || prev_end!=base) break;
        base=(uintptr_t)prev.addr;
    }
    pi=0;
    if(R_FAILED(svcQueryMemory(&mi,&pi,override_hi-1u)) || ((mi.type & 0xffu) != MemType_Heap)) return -1;
    if(base>=override_lo) return -1;
    *out_base=base;
    return 0;
}

static void runtime_release_override_headroom(void **addr_io, size_t *size_io) {
    if(!addr_io || !size_io || !*addr_io || !*size_io || !g_heap_from_override || !HOST_PROCESS_HEADROOM_BYTES) return;
    uintptr_t override_lo=(uintptr_t)*addr_io;
    uintptr_t override_hi=override_lo+(uintptr_t)*size_io;
    if(override_hi<=override_lo || *size_io <= (size_t)HOST_PROCESS_HEADROOM_BYTES) {
        g_headroom_release_status=-1;
        return;
    }

    uintptr_t heap_base=0;
    if(runtime_find_loader_heap_base(override_lo,override_hi,&heap_base)<0) {
        g_headroom_release_status=-1;
        return;
    }

    size_t prefix=(size_t)(override_lo-heap_base);
    size_t current_total=(size_t)(override_hi-heap_base);
    size_t release=(size_t)HOST_PROCESS_HEADROOM_BYTES;
    if((release & 0x1fffffu) || current_total<=release || *size_io<=release) {
        g_headroom_release_status=-1;
        return;
    }

    size_t new_total=current_total-release;
    void *new_base=NULL;
    Result rc=svcSetHeapSize(&new_base,new_total);
    g_headroom_release_result=rc;
    if(R_FAILED(rc) || (uintptr_t)new_base!=heap_base) {
        g_headroom_release_status=-2;
        return;
    }

    /* hbloader's OverrideHeap is exactly the tail after the NRO source image.
     * Shrinking the kernel heap from the top therefore shortens only this tail. */
    *size_io-=release;
    g_loader_heap_prefix_size=prefix;
    g_host_headroom_released=release;
    g_headroom_release_status=1;
}

void __libnx_initheap(void) {
    void *addr=NULL; size_t size=0;
    g_heap_from_override=envHasHeapOverride()?1:0;
    if(g_heap_from_override) {
        addr=envGetHeapOverrideAddr();
        size=envGetHeapOverrideSize();
        runtime_release_override_headroom(&addr,&size);
    } else {
        u64 total=0,used=0;
        svcGetInfo(&total,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0);
        svcGetInfo(&used,InfoType_UsedMemorySize,CUR_PROCESS_HANDLE,0);
        u64 reserve=0x400000ull+(u64)HOST_PROCESS_HEADROOM_BYTES;
        if(total>used+reserve) size=(size_t)((total-used-reserve)&~0x1fffffull);
        if(size<0x20000000u) size=0x20000000u;
        Result rc=svcSetHeapSize(&addr,size); if(R_FAILED(rc)) diagAbortWithResult(rc);
        if(R_SUCCEEDED(rc)) {
            g_host_headroom_released=(size_t)HOST_PROCESS_HEADROOM_BYTES;
            g_headroom_release_status=1;
            g_headroom_release_result=rc;
        }
    }

    uintptr_t heap_lo=(uintptr_t)addr;
    uintptr_t heap_hi=heap_lo+size;
    const uintptr_t slot=(uintptr_t)UNITY_MMAP_SLOT_BYTES;
    uintptr_t mmap_hi=heap_hi & ~(slot-1u);
    size_t mmap_bytes=(size_t)UNITY_MMAP_ARENA_BYTES;

    while(mmap_bytes>=slot) {
        if(mmap_hi >= heap_lo + mmap_bytes + (size_t)SO_ARENA_BYTES) {
            uintptr_t mmap_lo=mmap_hi-mmap_bytes;
            uintptr_t so_lo=mmap_lo-(uintptr_t)SO_ARENA_BYTES;
            if(so_lo>=heap_lo+(uintptr_t)MIN_NEWLIB_HEAP_BYTES) break;
        }
        mmap_bytes-=slot;
    }
    if(mmap_bytes<slot) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));

    uintptr_t mmap_lo=mmap_hi-mmap_bytes;
    uintptr_t so_lo=mmap_lo-(uintptr_t)SO_ARENA_BYTES;

    extern char *fake_heap_start,*fake_heap_end;
    fake_heap_start=(char*)heap_lo;
    fake_heap_end=(char*)so_lo;
    g_newlib_heap_size=(size_t)(so_lo-heap_lo);

    g_so_arena=(void*)so_lo;
    g_so_arena_size=(size_t)SO_ARENA_BYTES;
    g_unity_mmap_arena=(void*)mmap_lo;
    g_unity_mmap_arena_size=mmap_bytes;
    g_heap_total=size;
}
size_t runtime_heap_total(void){return g_heap_total;}
size_t runtime_newlib_heap_size(void){return g_newlib_heap_size;}
size_t runtime_so_arena_size(void){return g_so_arena_size;}
size_t runtime_unity_mmap_arena_size(void){return g_unity_mmap_arena_size;}
int runtime_heap_from_override(void){return g_heap_from_override;}
size_t runtime_loader_heap_prefix_size(void){return g_loader_heap_prefix_size;}
size_t runtime_host_headroom_released(void){return g_host_headroom_released;}
int runtime_headroom_release_status(void){return g_headroom_release_status;}
Result runtime_headroom_release_result(void){return g_headroom_release_result;}
static int load_one(so_module*m,const char*name,void**cursor,size_t*left){
    char p[768];snprintf(p,sizeof p,DATA_ROOT "/%s",name);
    if(so_load(m,p,*cursor,*left)<0)return-1;
    size_t used=ALIGN_MEM(m->load_size,0x1000);*cursor=(char*)*cursor+used;*left-=used;return 0;
}
int runtime_load_modules(void){
    imports_init();
    void*cur=g_so_arena;size_t left=g_so_arena_size;
    bionic_set_mmap_arena(g_unity_mmap_arena,g_unity_mmap_arena_size);

    if(load_one(&main_mod,LIB_MAIN,&cur,&left)<0)return-1;
    if(load_one(&unity_mod,LIB_UNITY,&cur,&left)<0)return-1;
    if(load_one(&il2cpp_mod,LIB_IL2CPP,&cur,&left)<0)return-1;

    if(!so_check_build_id(&main_mod,EXPECTED_MAIN_BUILD_ID) ||
       !so_check_build_id(&unity_mod,EXPECTED_UNITY_BUILD_ID) ||
       !so_check_build_id(&il2cpp_mod,EXPECTED_IL2CPP_BUILD_ID))return -11;
    so_module*mods[]={&main_mod,&unity_mod,&il2cpp_mod};
    for(unsigned i=0;i<3;i++){
        if(so_relocate(mods[i])<0)return-2;
    }
    int unresolved=0;
    for(unsigned i=0;i<3;i++)unresolved+=imports_resolve_module(mods[i]);
    if(unresolved){
        return -3;
    }
    for(unsigned i=0;i<3;i++){
        so_finalize(mods[i]);
        so_flush_caches(mods[i]);
    }

    bionic_install_main_tls();

    for(unsigned i=0;i<3;i++){
        so_execute_init_array(mods[i]);
    }
    return 0;
}
void runtime_free_temp_images(void){so_free_temp(&main_mod);so_free_temp(&unity_mod);so_free_temp(&il2cpp_mod);}
