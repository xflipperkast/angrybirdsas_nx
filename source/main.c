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
    jni_init();
    uintptr_t ub=(uintptr_t)unity_mod.load_virtbase;

    const struct { so_module *mod; const char *label; } jni_modules[] = {
        { &main_mod, "libmain" }, { &unity_mod, "libunity" }, { &il2cpp_mod, "libil2cpp" }
    };
    for (unsigned i=0; i<sizeof(jni_modules)/sizeof(jni_modules[0]); ++i) {
        fn_jnionload onload=(fn_jnionload)so_try_find_addr_rx(jni_modules[i].mod,"JNI_OnLoad");
        if(!onload) fatal_error("%s has no exported JNI_OnLoad despite matching Build ID.",jni_modules[i].label);
        int jv=onload(fake_vm,NULL);
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

    initJni(fake_env,fake_unityplayer_thiz,fake_context_obj,0,NULL);
    recreate(fake_env,fake_unityplayer_thiz,0,fake_surface_obj);
    surfaceChanged(fake_env,fake_unityplayer_thiz);
    resume(fake_env,fake_unityplayer_thiz);
    focus(fake_env,fake_unityplayer_thiz,1);

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

    while(appletMainLoop()&&!jni_quit_requested){
        android_update_mode();
#if ENABLE_TOUCH_INPUT
        input_feed(inject,fake_env,fake_unityplayer_thiz);
#endif
        if(!render(fake_env,fake_unityplayer_thiz))break;
    }

#if ENABLE_FMOD_AUDIO
    fmod_audio_stop();
#endif
    unity_patches_stop();
    focus(fake_env,fake_unityplayer_thiz,0);
    unload(fake_env,fake_unityplayer_thiz);
    done(fake_env,fake_unityplayer_thiz);
    return 0;
}

int main(int argc,char **argv){
    (void)argc;
    (void)argv;
    ensure_dirs();
    require_syscalls();

    if(runtime_heap_total() < (900u*1024u*1024u))
        fatal_error("Only %lu MB process heap available. Use title override/full-memory hbmenu, not Album applet mode.",
                    (unsigned long)(runtime_heap_total()>>20));
    if(installer_prepare_game_files()<0)
        fatal_error("First-run APK extraction failed:\n%s",installer_last_error());

    ensure_dirs();
    if(chdir(DATA_ROOT)!=0)
        fatal_error("Could not enter extracted runtime directory: %s",DATA_ROOT);

    Result net=socketInitializeDefault();
    android_update_mode();
    android_input_init();
    input_init();

    if(runtime_load_modules()<0)
        fatal_error("Could not map/validate the extracted Android native libraries.");
    (void)file_prepare_virtual_system_files();
    if(unity_patches_install()<0)
        fatal_error("Unity 6000 clock/frame patch guards did not match. Refusing unsafe patching.");

    if(guest_stack_init(UNITY_SESSION_STACK_BYTES)<0)
        fatal_error("Could not allocate the %u MB Unity session stack.",
                    (unsigned)(UNITY_SESSION_STACK_BYTES>>20));
    intptr_t session_rc=guest_stack_call(unity_session,NULL);
    guest_stack_shutdown();

    if(R_SUCCEEDED(net))socketExit();
    return (int)session_rc;
}
