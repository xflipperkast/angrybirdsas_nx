#include "unity_patches.h"
#include "config.h"
#include "runtime.h"
#include "so_util.h"
#include "android_ndk.h"
#include "jni_fake.h"
#include "bionic.h"
#include <switch.h>
#include <stdint.h>
#include <time.h>

static void (*g_update_body)(void *,double);
static volatile uint64_t *g_vsync_counter;
static volatile uint64_t g_last_main_tick_ns;
static uint64_t g_clock_base_ns;
static void *g_time_manager;
static Mutex g_clock_lock;
static Thread g_clock_thread;
static volatile int g_stop;
static int g_started;
static int g_installed;

static uint64_t now_ns(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
}

static void clock_tick(void *tm){
    uint64_t now=now_ns();
    if(!g_clock_base_ns)g_clock_base_ns=now;
    double wall=(double)(now-g_clock_base_ns)/1000000000.0;
    if(g_update_body)g_update_body(tm,wall);
}

static void time_update_hook(void *tm){
    g_time_manager=tm;
    g_last_main_tick_ns=now_ns();
    *(volatile uint64_t*)((uint8_t*)tm+OFF_TM_FRAMECOUNT_U64)+=1;
    *(volatile uint32_t*)((uint8_t*)tm+OFF_TM_AUX_U32)+=1;
    if(*(volatile uint8_t*)((uint8_t*)tm+OFF_TM_PAUSE_U8))return;
    mutexLock(&g_clock_lock);
    clock_tick(tm);
    mutexUnlock(&g_clock_lock);
}

static int verify_time_prologue(void){
    static const uint32_t want[]={
        0xf940b008u,0xb9416809u,0x3946a00au,0x91000508u,
        0x11000529u,0xf900b008u,0xb9016809u,0x3400004au
    };
    for(unsigned i=0;i<sizeof(want)/sizeof(want[0]);i++){
        uint32_t got=so_read_word(&unity_mod,OFF_TIME_MANAGER_UPDATE_ENTRY+i*4u);
        if(got!=want[i]){
            return 0;
        }
    }
    return 1;
}

static int install_cpu_count_fix(void){
    u64 mask=0;
    Result rc=svcGetInfo(&mask,InfoType_CoreMask,CUR_PROCESS_HANDLE,0);
    if(R_FAILED(rc)||!mask){
        return 0;
    }
    unsigned count=(unsigned)__builtin_popcountll(mask);
    if(!count||count>0xffffu)return -1;

    uint32_t got=so_read_word(&unity_mod,OFF_UNITY_CPU_COUNT_RETURN);
    uint32_t want=0x52800000u|((uint32_t)count<<5);
    if(got!=UNITY_CPU_COUNT_RETURN_ORIG&&got!=want){
        return -2;
    }
    if(got==UNITY_CPU_COUNT_RETURN_ORIG&&
       so_patch_code(&unity_mod,OFF_UNITY_CPU_COUNT_RETURN,&want,sizeof(want))<0)
        return -3;
    return 0;
}

int unity_patches_install(void){
    mutexInit(&g_clock_lock);
    if(install_cpu_count_fix()<0)return -7;
    uintptr_t ub=(uintptr_t)unity_mod.load_virtbase;
    g_update_body=(void(*)(void*,double))(ub+OFF_TIME_MANAGER_UPDATE_BODY);
    g_vsync_counter=(volatile uint64_t*)(ub+OFF_VSYNC_COUNTER);

#if ENABLE_DYNAMIC_HEAP_REGION_PATCH
    typedef struct { uint32_t off, from, to; } HeapPatch;
    const HeapPatch hp[]={
        {OFF_DYNAMIC_HEAP_REGION_SIZE,       DYNAMIC_HEAP_REGION_256M_INSN, DYNAMIC_HEAP_REGION_64M_INSN},
        {OFF_DYNAMIC_HEAP_REGION_MINUS_ONE,  DYNAMIC_HEAP_MINUS1_256M_INSN, DYNAMIC_HEAP_MINUS1_64M_INSN},
        {OFF_DYNAMIC_HEAP_REGION_ALIGN_MASK, DYNAMIC_HEAP_MASK_256M_INSN,   DYNAMIC_HEAP_MASK_64M_INSN},
        {OFF_DYNAMIC_HEAP_VM_ALIGNMENT,      DYNAMIC_HEAP_ALIGN_256M_INSN,  DYNAMIC_HEAP_ALIGN_64M_INSN},
        {OFF_OWNER_RANGE_ADDR_MASK,           OWNER_RANGE_MASK_56_INSN,      OWNER_RANGE_MASK_54_INSN},
        {OFF_OWNER_RANGE_START_BUCKET,        OWNER_START_28_INSN,           OWNER_START_26_INSN},
        {OFF_OWNER_RANGE_END_BUCKET,          OWNER_END_28_INSN,             OWNER_END_26_INSN},
        {OFF_OWNER_LOOKUP_BUCKET,             OWNER_LOOKUP_28_INSN,          OWNER_LOOKUP_26_INSN},
        {OFF_OWNER_LOOKUP_BASE_MASK,          OWNER_BASE_MASK_28_INSN,       OWNER_BASE_MASK_26_INSN},
        {OFF_OWNER_LOOKUP_LEAF_INDEX,         OWNER_LEAF_28_INSN,            OWNER_LEAF_26_INSN},
        {OFF_OWNER_LOOKUP_REGION_INDEX,       OWNER_REGION_28_INSN,          OWNER_REGION_26_INSN},
        {OFF_OWNER_RUN_BACKSTEP_HI,           OWNER_BACKSTEP_HI_256M_INSN,   OWNER_BACKSTEP_HI_64M_INSN},
        {OFF_OWNER_RUN_BACKSTEP_LO,           OWNER_BACKSTEP_LO_256M_INSN,   OWNER_BACKSTEP_LO_64M_INSN},
        {OFF_OWNER_LOOKUP_BACKSTEP,           OWNER_BACKSTEP_SHIFT28_INSN,   OWNER_BACKSTEP_SHIFT26_INSN},
        {OFF_OWNER_DIRECT_TOP_INDEX,          OWNER_TOP_SHIFT40_INSN,        OWNER_TOP_SHIFT38_INSN},
        {OFF_OWNER_DIRECT_LEAF_INDEX,         OWNER_DIRECT_LEAF28_INSN,      OWNER_DIRECT_LEAF26_INSN},
        {OFF_OWNER_MANAGER_TOP_INDEX,         OWNER_MANAGER_TOP40_INSN,      OWNER_MANAGER_TOP38_INSN},
        {OFF_OWNER_MANAGER_LEAF_INDEX,        OWNER_MANAGER_LEAF28_INSN,     OWNER_MANAGER_LEAF26_INSN},
    };
    for(unsigned i=0;i<sizeof(hp)/sizeof(hp[0]);i++){
        uint32_t got=so_read_word(&unity_mod,hp[i].off);
        if(got!=hp[i].from && got!=hp[i].to){
            return -5;
        }
    }
    for(unsigned i=0;i<sizeof(hp)/sizeof(hp[0]);i++){
        uint32_t got=so_read_word(&unity_mod,hp[i].off);
        if(got==hp[i].from && so_patch_code(&unity_mod,hp[i].off,&hp[i].to,sizeof(hp[i].to))<0)
            return -6;
    }

#endif

#if ENABLE_CHOREOGRAPHER_WAIT_PATCH
    uint32_t branch=so_read_word(&unity_mod,OFF_CHOREOGRAPHER_WAIT_SITE);
    if(branch==CHOREOGRAPHER_WAIT_FROM){
        uint32_t replacement=CHOREOGRAPHER_WAIT_TO;
        if(so_patch_code(&unity_mod,OFF_CHOREOGRAPHER_WAIT_SITE,&replacement,sizeof(replacement))<0)return -1;
    }else{
        return -2;
    }
#endif

#if ENABLE_UNITY_TIME_FIX
    if(!verify_time_prologue())return -3;
    uint32_t stub[4]={
        0x58000050u,
        0xd61f0200u,
        (uint32_t)((uintptr_t)&time_update_hook&0xffffffffu),
        (uint32_t)((uintptr_t)&time_update_hook>>32)
    };
    if(so_patch_code(&unity_mod,OFF_TIME_MANAGER_UPDATE_ENTRY,stub,sizeof(stub))<0)return -4;

#endif
    g_installed=1;
    return 0;
}

static void clock_thread_main(void *arg){
    (void)arg;
    static uint8_t clock_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
    bionic_install_tls(clock_tls);
    uint64_t last_choreo=0;
    while(!g_stop&&!jni_quit_requested){
        svcSleepThread(8000000ull);
        if(g_vsync_counter)__atomic_add_fetch(g_vsync_counter,1,__ATOMIC_RELAXED);
        uint64_t now=now_ns();
        if(now-last_choreo>=16000000ull){last_choreo=now;choreographer_tick((int64_t)now);}
        void *tm=g_time_manager;
        if(tm&&g_update_body&&g_last_main_tick_ns&&now-g_last_main_tick_ns>100000000ull&&mutexTryLock(&g_clock_lock)){
            clock_tick(tm);
            mutexUnlock(&g_clock_lock);
        }
    }
}

int unity_patches_start(void){
    if(g_started)return 0;
    if(!g_installed)return -1;
    g_stop=0;
    Result r=threadCreate(&g_clock_thread,clock_thread_main,NULL,NULL,0x10000,0x2c,-2);
    if(R_FAILED(r))return -2;
    r=threadStart(&g_clock_thread);
    if(R_FAILED(r)){threadClose(&g_clock_thread);return -3;}
    g_started=1;
    return 0;
}

void unity_patches_stop(void){
    if(!g_started)return;
    g_stop=1;
    threadWaitForExit(&g_clock_thread);
    threadClose(&g_clock_thread);
    g_started=0;
}
