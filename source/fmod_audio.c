#include "fmod_audio.h"
#include "jni_fake.h"
#include "bionic.h"
#include "thread_util.h"
#include <switch.h>
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int (*FGetInfo)(void*,void*,int);
typedef int (*FProcess)(void*,void*,void*);
static FGetInfo g_info; static FProcess g_process; static void *g_mic;
static Thread g_thread; static volatile int g_stop; static SDL_AudioDeviceID g_dev; static int g_started;
static unsigned char g_pcm[128*1024] __attribute__((aligned(64)));
static int g_this_token;

enum { FMOD_INFO_OUTPUT_RATE=0, FMOD_INFO_DSP_BLOCK_LENGTH=1, FMOD_INFO_DSP_BUFFER_COUNT=2, FMOD_INFO_OUTPUT_TYPE=3, FMOD_INFO_CHANNELS=4 };
void fmod_audio_bind(void*a,void*b,void*c){g_info=(FGetInfo)a;g_process=(FProcess)b;g_mic=c;}
static void audio_thread(void*arg){(void)arg;
    static uint8_t audio_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
    bionic_install_tls(audio_tls);
    int rate=g_info?g_info(fake_env,&g_this_token,FMOD_INFO_OUTPUT_RATE):48000;
    int block=g_info?g_info(fake_env,&g_this_token,FMOD_INFO_DSP_BLOCK_LENGTH):1024;
    int ch=g_info?g_info(fake_env,&g_this_token,FMOD_INFO_CHANNELS):2;
    if (rate < 8000 || rate > 192000)
        rate = 48000;
    if (block < 64 || block > 8192)
        block = 1024;
    if (ch < 1 || ch > 8)
        ch = 2;
    size_t bytes=(size_t)block*(size_t)ch*sizeof(int16_t);if(bytes>sizeof(g_pcm))bytes=sizeof(g_pcm)&~3u;
    SDL_AudioSpec want={0},got={0};want.freq=rate;want.format=AUDIO_S16SYS;want.channels=(Uint8)ch;want.samples=(Uint16)(block>4096?4096:block);want.callback=NULL;
    g_dev=SDL_OpenAudioDevice(NULL,0,&want,&got,0);if(!g_dev){return;}
    SDL_PauseAudioDevice(g_dev,0);
    void*bb=jni_new_direct_buffer(g_pcm,bytes);
    while(!g_stop&&!jni_quit_requested){if(SDL_GetQueuedAudioSize(g_dev)>bytes*4){svcSleepThread(3000000ull);continue;}memset(g_pcm,0,bytes);int r=g_process?g_process(fake_env,&g_this_token,bb):-1;if(r<0){svcSleepThread(10000000ull);continue;}SDL_QueueAudio(g_dev,g_pcm,(Uint32)bytes);}
    SDL_ClearQueuedAudio(g_dev);SDL_CloseAudioDevice(g_dev);g_dev=0;
}
int fmod_audio_start(void){if(g_started||!g_process)return 0;if(SDL_InitSubSystem(SDL_INIT_AUDIO)!=0){return-1;}g_stop=0;Result r=threadCreate(&g_thread,audio_thread,NULL,NULL,0x10000,0x2c,-2);if(R_FAILED(r))return-1;(void)asnx_thread_allow_all_cores(&g_thread,NULL);r=threadStart(&g_thread);if(R_FAILED(r)){threadClose(&g_thread);return-1;}g_started=1;return 0;}
void fmod_audio_stop(void){if(!g_started)return;g_stop=1;threadWaitForExit(&g_thread);threadClose(&g_thread);SDL_QuitSubSystem(SDL_INIT_AUDIO);g_started=0;(void)g_mic;}
