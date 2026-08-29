#include "config.h"
#include "installer.h"
#include "runtime.h"
#include "unity_entrypoints.h"
#include "jni_fake.h"
#include "android_ndk.h"
#include "input.h"
#include "fmod_audio.h"
#include "unity_patches.h"
#include "file_bridge.h"
#include "guest_stack.h"
#include "fatal.h"
#include "trace_log.h"
#include "mem_diag.h"
#include <switch.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

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

static intptr_t unity_session(void *unused){
    (void)unused;
    trace_log_printf("UNITY","session begin");
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
    uint64_t summary_start=trace_now_us();
    uint64_t summary_frames=0;
    uint64_t summary_render_us=0;
    uint64_t summary_max_us=0;
    while(appletMainLoop()&&!jni_quit_requested){
        android_update_mode();
#if ENABLE_TOUCH_INPUT
        input_feed(inject,fake_env,fake_unityplayer_thiz);
#endif
        uint64_t t0=trace_now_us();
        int keep_running=render(fake_env,fake_unityplayer_thiz);
        uint64_t render_us=trace_now_us()-t0;
        frame++;
        summary_frames++;
        summary_render_us+=render_us;
        if(render_us>summary_max_us)summary_max_us=render_us;
        if(render_us>=TRACE_SLOW_FRAME_US)
            trace_log_printf("FRAME","slow frame=%llu render=%lluus",
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
    (void)trace_log_init();
    trace_log_printf("BOOT","Angry Bird Epic All Stars NX 1.0.6 diagnostic start");
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
    intptr_t session_rc=guest_stack_call(unity_session,NULL);
    guest_stack_shutdown();

    if(R_SUCCEEDED(net))socketExit();
    trace_log_printf("BOOT","exit rc=%lld",(long long)session_rc);
    trace_log_close();
    return (int)session_rc;
}
