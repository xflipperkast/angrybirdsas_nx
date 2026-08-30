#include "config.h"
#include "installer.h"
#include "runtime.h"
#include "unity_entrypoints.h"
#include "jni_fake.h"
#include "android_ndk.h"
#include "input.h"
#include "fmod_audio.h"
#include "aaudio_bridge.h"
#include "unity_patches.h"
#include "file_bridge.h"
#include "guest_stack.h"
#include "fatal.h"
#include "trace_log.h"
#include "mem_diag.h"
#include "bionic.h"
#include "thread_util.h"
#include <switch.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

static void require_syscalls(void){
    if(!envIsSyscallHinted(0x77)||!envIsSyscallHinted(0x78)||!envIsSyscallHinted(0x73)||envGetOwnProcessHandle()==INVALID_HANDLE)
        fatal_error("Required code-memory syscalls are unavailable. Launch via full-memory hbmenu/title override.");
}


static void ensure_dirs(void){
    mkdir(GAME_HOME,0777);
    mkdir(DATA_ROOT,0777);
    mkdir(DATA_ROOT "/files",0777);
    mkdir(DATA_ROOT "/cache",0777);
}

#if ENABLE_TRACE_LOG && ENABLE_LOAD_DIAGNOSTICS
static Thread g_load_watchdog_thread;
static volatile int g_load_watchdog_stop;
static volatile int g_render_inflight;
static volatile uint64_t g_render_begin_us;
static volatile uint64_t g_render_frame;
static Handle g_unity_main_thread;

static void format_guest_pc(uint64_t addr,char *out,size_t cap){
    const struct { const so_module *m; const char *name; } mods[]={
        {&unity_mod,"libunity"},{&il2cpp_mod,"libil2cpp"},{&main_mod,"libmain"}
    };
    for(unsigned i=0;i<sizeof(mods)/sizeof(mods[0]);i++){
        uintptr_t base=(uintptr_t)mods[i].m->load_virtbase;
        if(base&&addr>=base&&addr<base+mods[i].m->load_size){
            snprintf(out,cap,"%s+0x%llx",mods[i].name,(unsigned long long)(addr-base));
            return;
        }
    }
    snprintf(out,cap,"host/other:0x%llx",(unsigned long long)addr);
}

static void sample_unity_main_context(uint64_t frame,uint64_t age_us){
    Handle h=g_unity_main_thread;
    if(!h)return;
    Result pr=svcSetThreadActivity(h,ThreadActivity_Paused);
    if(R_FAILED(pr)){
        trace_log_printf("PCSAMPLE","main frame=%llu age=%llums pause_rc=0x%x",
            (unsigned long long)frame,(unsigned long long)(age_us/1000ull),(unsigned)pr);
        return;
    }
    ThreadContext ctx;
    memset(&ctx,0,sizeof(ctx));
    Result cr=svcGetThreadContext3(&ctx,h);
    Result rr=svcSetThreadActivity(h,ThreadActivity_Runnable);
    if(R_FAILED(cr)){
        trace_log_printf("PCSAMPLE","main frame=%llu age=%llums context_rc=0x%x resume_rc=0x%x",
            (unsigned long long)frame,(unsigned long long)(age_us/1000ull),(unsigned)cr,(unsigned)rr);
        return;
    }
    char pc[64],lr[64];
    format_guest_pc(ctx.pc.x,pc,sizeof(pc));
    format_guest_pc(ctx.lr,lr,sizeof(lr));
    trace_log_printf("PCSAMPLE","main frame=%llu age=%llums pc=%s lr=%s sp=0x%llx resume_rc=0x%x",
        (unsigned long long)frame,(unsigned long long)(age_us/1000ull),pc,lr,
        (unsigned long long)ctx.sp,(unsigned)rr);
}

static void load_watchdog_entry(void *unused){
    (void)unused;
    uint64_t last_report=0,last_pc_sample=0,last_worker_sample=0;
    uint64_t sampled_frame=0;
    while(!__atomic_load_n(&g_load_watchdog_stop,__ATOMIC_ACQUIRE)){
        svcSleepThread(100000000ll);
        if(!__atomic_load_n(&g_render_inflight,__ATOMIC_ACQUIRE)){
            last_pc_sample=last_worker_sample=0; sampled_frame=0;
            continue;
        }
        uint64_t begin=__atomic_load_n(&g_render_begin_us,__ATOMIC_ACQUIRE);
        uint64_t now=trace_now_us();
        if(!begin||now<begin)continue;
        uint64_t age=now-begin;
        uint64_t frame=__atomic_load_n(&g_render_frame,__ATOMIC_RELAXED);
        if(frame!=sampled_frame){sampled_frame=frame;last_pc_sample=last_worker_sample=0;}
        if(age>=TRACE_PC_SAMPLE_START_US&&(!last_pc_sample||now-last_pc_sample>=TRACE_PC_SAMPLE_INTERVAL_US)){
            last_pc_sample=now;
            sample_unity_main_context(frame,age);
        }
        if(age>=TRACE_PC_SAMPLE_START_US&&(!last_worker_sample||now-last_worker_sample>=TRACE_WORKER_SAMPLE_INTERVAL_US)){
            last_worker_sample=now;
            bionic_thread_pc_snapshot("render-stall",2);
        }
        if(age<TRACE_HANG_THRESHOLD_US)continue;
        if(last_report&&now-last_report<TRACE_HANG_REPEAT_US)continue;
        last_report=now;
        trace_log_printf("HANG","Unity render still blocked frame=%llu age=%llums watchdog_core=%u",
            (unsigned long long)frame,(unsigned long long)(age/1000ull),
            svcGetCurrentProcessorNumber());
        mem_diag_snapshot("render-hang");
        trace_main_block_snapshot("render-hang");
        bionic_futex_diag_snapshot("render-hang");
        aaudio_bridge_diag_snapshot("render-hang");
        bionic_thread_diag_snapshot("render-hang");
        trace_log_flush();
    }
}

static int load_watchdog_start(void){
    __atomic_store_n(&g_load_watchdog_stop,0,__ATOMIC_RELEASE);
    Result r=threadCreate(&g_load_watchdog_thread,load_watchdog_entry,NULL,NULL,256u*1024u,0x3b,-2);
    if(R_FAILED(r))return -1;
    u64 mask=0;
    (void)asnx_thread_allow_all_cores(&g_load_watchdog_thread,&mask);
    r=threadStart(&g_load_watchdog_thread);
    if(R_FAILED(r)){threadClose(&g_load_watchdog_thread);return -1;}
    trace_log_printf("LOAD","watchdog started process_mask=0x%llx",(unsigned long long)mask);
    return 0;
}

static void load_watchdog_stop(void){
    if(!g_load_watchdog_thread.handle)return;
    __atomic_store_n(&g_load_watchdog_stop,1,__ATOMIC_RELEASE);
    (void)threadWaitForExit(&g_load_watchdog_thread);
    threadClose(&g_load_watchdog_thread);
    memset(&g_load_watchdog_thread,0,sizeof(g_load_watchdog_thread));
}

#endif

static intptr_t unity_session(void *unused){
    (void)unused;
    trace_log_printf("UNITY","session begin");
#if ENABLE_TRACE_LOG && ENABLE_LOAD_DIAGNOSTICS
    g_unity_main_thread=threadGetCurHandle();
#endif
    jni_init();
    uintptr_t ub=(uintptr_t)unity_mod.load_virtbase;

    const struct { so_module *mod; const char *label; } jni_modules[] = {
        { &main_mod, "libmain" }, { &unity_mod, "libunity" }, { &il2cpp_mod, "libil2cpp" }
    };
    for (unsigned i=0; i<sizeof(jni_modules)/sizeof(jni_modules[0]); ++i) {
        fn_jnionload onload=(fn_jnionload)so_try_find_addr_rx(jni_modules[i].mod,"JNI_OnLoad");
        if(!onload) fatal_error("%s has no exported JNI_OnLoad despite matching Build ID.",jni_modules[i].label);
        uint64_t t0=trace_now_us();
        int jv=onload(fake_vm,NULL);
        trace_log_printf("UNITY","%s JNI_OnLoad -> 0x%x time=%lluus",jni_modules[i].label,jv,
            (unsigned long long)(trace_now_us()-t0));
        if(jv!=JNI_VERSION_1_6) fatal_error("%s JNI_OnLoad returned 0x%x, expected JNI_VERSION_1_6.",jni_modules[i].label,jv);
    }

    fn_initJni initJni=(fn_initJni)(ub+OFF_UNITY_INIT_JNI);
    fn_gfxstate recreate=(fn_gfxstate)(ub+OFF_UNITY_RECREATE_GFX);
    fn_v surfaceChanged=(fn_v)(ub+OFF_UNITY_SURFACE_CHANGED);
    fn_z render=(fn_z)(ub+OFF_UNITY_RENDER);
    fn_inject inject=(fn_inject)(ub+OFF_UNITY_INJECT_EVENT);
    fn_v resume=(fn_v)(ub+OFF_UNITY_RESUME);
    fn_vz focus=(fn_vz)(ub+OFF_UNITY_FOCUS_CHANGED);
    fn_z done=(fn_z)(ub+OFF_UNITY_DONE);
    fn_v unload=(fn_v)(ub+OFF_UNITY_APP_UNLOAD);

#if ENABLE_FMOD_AUDIO
    fmod_audio_bind((void*)(ub+OFF_UNITY_FMOD_GET_INFO),(void*)(ub+OFF_UNITY_FMOD_PROCESS),(void*)(ub+OFF_UNITY_FMOD_PROCESS_MIC_DATA));
#endif

    uint64_t init_t0=trace_now_us();
    initJni(fake_env,fake_unityplayer_thiz,fake_context_obj,0,NULL);
    trace_log_printf("UNITY","initJni complete time=%lluus",(unsigned long long)(trace_now_us()-init_t0));
    recreate(fake_env,fake_unityplayer_thiz,0,fake_surface_obj);
    surfaceChanged(fake_env,fake_unityplayer_thiz);
    resume(fake_env,fake_unityplayer_thiz);
    focus(fake_env,fake_unityplayer_thiz,1);
    trace_log_printf("UNITY","graphics surface/resume/focus complete");
    mem_diag_snapshot("unity-ready");
    bionic_thread_diag_snapshot("unity-ready");

#if ENABLE_IL2CPP_GC_DISABLE
    typedef void(*GcVoid)(void); typedef void(*GcMode)(int);
    GcMode gc_mode=(GcMode)so_try_find_addr_rx(&il2cpp_mod,"il2cpp_gc_set_mode");
    GcVoid gc_disable=(GcVoid)so_try_find_addr_rx(&il2cpp_mod,"il2cpp_gc_disable");
    if(gc_mode)gc_mode(1);
    if(gc_disable)gc_disable();
#endif

    runtime_free_temp_images();
    if(unity_patches_start()<0)
        fatal_error("Could not start the Unity frame/clock helper thread.");
#if ENABLE_FMOD_AUDIO
    (void)fmod_audio_start();
#endif

    uint64_t frame=0;
#if ENABLE_TRACE_LOG && ENABLE_LOAD_DIAGNOSTICS
    uint64_t summary_start=trace_now_us();
    uint64_t summary_frames=0;
    uint64_t summary_render_us=0;
    uint64_t summary_max_us=0;
#endif
    while(appletMainLoop()&&!jni_quit_requested){
        android_update_mode();
#if ENABLE_TOUCH_INPUT
        input_feed(inject,fake_env,fake_unityplayer_thiz);
#endif
#if ENABLE_TRACE_LOG && ENABLE_LOAD_DIAGNOSTICS
        uint64_t t0=trace_now_us();
        __atomic_store_n(&g_render_frame,frame+1u,__ATOMIC_RELAXED);
        __atomic_store_n(&g_render_begin_us,t0,__ATOMIC_RELEASE);
        __atomic_store_n(&g_render_inflight,1,__ATOMIC_RELEASE);
        int keep_running=render(fake_env,fake_unityplayer_thiz);
        __atomic_store_n(&g_render_inflight,0,__ATOMIC_RELEASE);
        uint64_t render_us=trace_now_us()-t0;
        frame++;
        summary_frames++;
        summary_render_us+=render_us;
        if(render_us>summary_max_us)summary_max_us=render_us;
        if(render_us>=TRACE_SLOW_FRAME_US)
            trace_log_printf(render_us>=250000ull?"LOAD":"FRAME","slow frame=%llu render=%lluus",
                (unsigned long long)frame,(unsigned long long)render_us);

        uint64_t now=trace_now_us();
        if(now-summary_start>=2000000ull){
            uint64_t span=now-summary_start;
            uint64_t avg=summary_frames?summary_render_us/summary_frames:0;
            unsigned fps_x10=span?(unsigned)((summary_frames*10000000ull)/span):0;
            trace_log_printf("FRAME","summary frame=%llu fps=%u.%u avg_render=%lluus max_render=%lluus",
                (unsigned long long)frame,fps_x10/10u,fps_x10%10u,
                (unsigned long long)avg,(unsigned long long)summary_max_us);
            mem_diag_snapshot("frame-summary");
            summary_start=now;
            summary_frames=0;
            summary_render_us=0;
            summary_max_us=0;
        }
        trace_log_pump();
#else
        int keep_running=render(fake_env,fake_unityplayer_thiz);
        frame++;
#if ENABLE_TRACE_LOG
        trace_log_pump();
#endif
#endif
        if(!keep_running)break;
    }

#if ENABLE_FMOD_AUDIO
    fmod_audio_stop();
#endif
    unity_patches_stop();
    focus(fake_env,fake_unityplayer_thiz,0);
    unload(fake_env,fake_unityplayer_thiz);
    done(fake_env,fake_unityplayer_thiz);
    trace_log_printf("UNITY","session end frames=%llu",(unsigned long long)frame);
    trace_log_flush();
    return 0;
}

int main(int argc,char **argv){
    (void)argc;
    (void)argv;
    ensure_dirs();
#if ENABLE_TRACE_LOG
    (void)trace_log_init();
    trace_main_thread_set();
#else
    (void)unlink(TRACE_LOG_PATH);
#endif
    trace_log_printf("BOOT","Angry Bird Epic All Stars NX 1.0.18 direct audout release start");
    u64 process_core_mask=0;
    if(R_SUCCEEDED(svcGetInfo(&process_core_mask,InfoType_CoreMask,CUR_PROCESS_HANDLE,0))&&process_core_mask)
        (void)svcSetThreadCoreMask(CUR_THREAD_HANDLE,-1,(u32)process_core_mask);
    trace_log_printf("SCHED","process core_mask=0x%llx android_cpu_count=%ld main_core=%u",
        (unsigned long long)process_core_mask,bionic_sysconf(96),svcGetCurrentProcessorNumber());
    trace_log_printf("BOOT","heap_total=%lluMB override=%d newlib=%lluMB unity_mmap=%lluMB so_arena=%lluMB",
        (unsigned long long)(runtime_heap_total()>>20),runtime_heap_from_override(),
        (unsigned long long)(runtime_newlib_heap_size()>>20),
        (unsigned long long)(runtime_unity_mmap_arena_size()>>20),
        (unsigned long long)(runtime_so_arena_size()>>20));
    require_syscalls();
    trace_log_printf("BOOT","required syscalls available");

    if(runtime_heap_total() < (900u*1024u*1024u))
        fatal_error("Only %lu MB process heap available. Use title override/full-memory hbmenu, not Album applet mode.",
                    (unsigned long)(runtime_heap_total()>>20));
    uint64_t install_t0=trace_now_us();
    if(installer_prepare_game_files()<0)
        fatal_error("Installation check/extraction failed:\n%s",installer_last_error());
    trace_log_printf("BOOT","game files prepared time=%lluus",(unsigned long long)(trace_now_us()-install_t0));

    ensure_dirs();
    if(chdir(DATA_ROOT)!=0)
        fatal_error("Could not enter extracted runtime directory: %s",DATA_ROOT);

    Result net=socketInitializeDefault();
    trace_log_printf("BOOT","socketInitializeDefault -> 0x%x",(unsigned)net);
    android_update_mode();
    android_input_init();
    input_init();

    uint64_t modules_t0=trace_now_us();
    int module_rc=runtime_load_modules();
    trace_log_printf("BOOT","runtime_load_modules -> %d time=%lluus",module_rc,
        (unsigned long long)(trace_now_us()-modules_t0));
    if(module_rc<0)
        fatal_error("Could not map/validate the extracted Android native libraries.");
    mem_diag_snapshot("modules-loaded");
    (void)file_prepare_virtual_system_files();
    if(unity_patches_install()<0)
        fatal_error("Unity 6000 clock/frame patch guards did not match. Refusing unsafe patching.");
    trace_log_printf("BOOT","Unity patches installed");

    if(guest_stack_init(UNITY_SESSION_STACK_BYTES)<0)
        fatal_error("Could not allocate the %u MB Unity session stack.",
                    (unsigned)(UNITY_SESSION_STACK_BYTES>>20));
    trace_log_printf("BOOT","enter Unity session stack=%uMB",(unsigned)(UNITY_SESSION_STACK_BYTES>>20));
#if ENABLE_TRACE_LOG && ENABLE_LOAD_DIAGNOSTICS
    if(load_watchdog_start()<0)
        trace_log_printf("LOAD","watchdog could not start; continuing without hang snapshots");
#endif
    intptr_t session_rc=guest_stack_call(unity_session,NULL);
#if ENABLE_TRACE_LOG && ENABLE_LOAD_DIAGNOSTICS
    __atomic_store_n(&g_render_inflight,0,__ATOMIC_RELEASE);
    load_watchdog_stop();
#endif
    guest_stack_shutdown();

    if(R_SUCCEEDED(net))socketExit();
    trace_log_printf("BOOT","exit rc=%lld",(long long)session_rc);
    trace_log_close();
    return (int)session_rc;
}
