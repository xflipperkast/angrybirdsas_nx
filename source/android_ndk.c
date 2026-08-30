#include "android_ndk.h"
#include "fakefd.h"
#include "thread_util.h"
#include <switch.h>
#include <string.h>
#include <time.h>

struct ANativeWindow { int dummy; };
typedef int (*ALooperCallback)(int fd,int events,void *data);
typedef struct { int used,fd,ident,events; ALooperCallback cb; void *data; } ALooperReg;
struct ALooper { Mutex m; CondVar cv; int signaled; int refs; uintptr_t owner; int used; ALooperReg regs[16]; };
static uint32_t g_w=1280,g_h=720;
static struct ALooper g_loop[16]; static Mutex g_loop_lock; static int g_loop_init;
static Mutex g_choreo_lock; static int g_choreo_init;
typedef struct { int used,is64; union{void(*oldcb)(long,void*);void(*cb64)(int64_t,void*);}u; void*data; } ChCb;
static ChCb g_callbacks[32];

void android_update_mode(void){if(appletGetOperationMode()==AppletOperationMode_Console){g_w=1920;g_h=1080;}else{g_w=1280;g_h=720;}NWindow*w=nwindowGetDefault();nwindowSetDimensions(w,g_w,g_h);nwindowSetCrop(w,0,0,g_w,g_h);}
uint32_t android_width(void){return g_w;}uint32_t android_height(void){return g_h;}
ANativeWindow*android_native_window(void){android_update_mode();return(ANativeWindow*)nwindowGetDefault();}
void ANativeWindow_acquire(ANativeWindow*w){(void)w;}void ANativeWindow_release(ANativeWindow*w){(void)w;}
ANativeWindow*ANativeWindow_fromSurface(void*env,void*surface){(void)env;(void)surface;return android_native_window();}
void*ANativeWindow_toSurface(void*env,ANativeWindow*w){(void)env;return w;}
int32_t ANativeWindow_getWidth(ANativeWindow*w){(void)w;return(int32_t)g_w;}int32_t ANativeWindow_getHeight(ANativeWindow*w){(void)w;return(int32_t)g_h;}
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*w,int32_t x,int32_t y,int32_t fmt){(void)fmt;if(x>0&&y>0){g_w=x;g_h=y;nwindowSetDimensions((NWindow*)w,x,y);nwindowSetCrop((NWindow*)w,0,0,x,y);}return 0;}
void android_input_init(void){padConfigureInput(1,HidNpadStyleSet_NpadStandard);}

static struct ALooper*loop_get(int create){if(!g_loop_init){mutexInit(&g_loop_lock);g_loop_init=1;}uintptr_t tid=(uintptr_t)threadGetCurHandle();mutexLock(&g_loop_lock);for(int i=0;i<16;i++)if(g_loop[i].used&&g_loop[i].owner==tid){mutexUnlock(&g_loop_lock);return&g_loop[i];}if(create)for(int i=0;i<16;i++)if(!g_loop[i].used){struct ALooper*l=&g_loop[i];memset(l,0,sizeof(*l));l->used=1;l->owner=tid;l->refs=1;mutexInit(&l->m);condvarInit(&l->cv);mutexUnlock(&g_loop_lock);return l;}mutexUnlock(&g_loop_lock);return NULL;}
ALooper*ALooper_prepare(int o){(void)o;return loop_get(1);}ALooper*ALooper_forThread(void){return loop_get(1);}
void ALooper_acquire(ALooper*l){if(!l)return;mutexLock(&l->m);l->refs++;mutexUnlock(&l->m);}void ALooper_release(ALooper*l){if(!l)return;mutexLock(&l->m);if(--l->refs<=0)l->used=0;mutexUnlock(&l->m);}
void ALooper_wake(ALooper*l){if(!l)return;mutexLock(&l->m);l->signaled=1;condvarWakeAll(&l->cv);mutexUnlock(&l->m);}

#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define ALOOPER_EVENT_INPUT   0x01
#define ALOOPER_EVENT_OUTPUT  0x02
#define ALOOPER_EVENT_ERROR   0x04
#define ALOOPER_EVENT_HANGUP  0x08
#define ALOOPER_EVENT_INVALID 0x10

static int looper_ready_events(const ALooperReg*r){
    if(!r->used)return 0;
    if(!fakefd_is_range(r->fd))return 0;
    if(!fakefd_is_open(r->fd))return ALOOPER_EVENT_INVALID;
    int st=fakefd_ready(r->fd),ev=0;
    if((st&ASNX_FD_READY_READ)&&(r->events&ALOOPER_EVENT_INPUT))ev|=ALOOPER_EVENT_INPUT;
    if((st&ASNX_FD_READY_WRITE)&&(r->events&ALOOPER_EVENT_OUTPUT))ev|=ALOOPER_EVENT_OUTPUT;
    if(st&ASNX_FD_READY_ERR)ev|=ALOOPER_EVENT_ERROR;
    if(st&ASNX_FD_READY_HUP)ev|=ALOOPER_EVENT_HANGUP;
    return ev;
}
int ALooper_pollOnce(int ms,int*outFd,int*outEvents,void**outData){
    ALooper*l=loop_get(1);
    if(outFd)*outFd=0;
    if(outEvents)*outEvents=0;
    if(outData)*outData=NULL;
    int remaining=ms;
    for(;;){
        mutexLock(&l->m);
        if(l->signaled){l->signaled=0;mutexUnlock(&l->m);return ALOOPER_POLL_WAKE;}
        for(int i=0;i<16;i++){
            ALooperReg r=l->regs[i];int ev=looper_ready_events(&r);
            if(!ev)continue;
            if(r.cb){
                mutexUnlock(&l->m);
                int keep=r.cb(r.fd,ev,r.data);
                if(!keep)ALooper_removeFd(l,r.fd);
                return ALOOPER_POLL_CALLBACK;
            }
            if(outFd)*outFd=r.fd;
            if(outEvents)*outEvents=ev;
            if(outData)*outData=r.data;
            mutexUnlock(&l->m);
            return r.ident;
        }
        if(ms==0){mutexUnlock(&l->m);return ALOOPER_POLL_TIMEOUT;}
        int wait_ms=(ms<0||remaining>1)?1:remaining;
        if(wait_ms<=0){mutexUnlock(&l->m);return ALOOPER_POLL_TIMEOUT;}
        condvarWaitTimeout(&l->cv,&l->m,(uint64_t)wait_ms*1000000ULL);
        mutexUnlock(&l->m);
        if(ms>0){remaining-=wait_ms;if(remaining<=0)return ALOOPER_POLL_TIMEOUT;}
    }
}
int ALooper_addFd(ALooper*l,int fd,int ident,int ev,void*cb,void*d){
    if(!l||fd<0)return-1;
    mutexLock(&l->m);
    int free_slot=-1;
    for(int i=0;i<16;i++){
        if(l->regs[i].used&&l->regs[i].fd==fd){free_slot=i;break;}
        if(free_slot<0&&!l->regs[i].used)free_slot=i;
    }
    if(free_slot<0){mutexUnlock(&l->m);return-1;}
    l->regs[free_slot]=(ALooperReg){.used=1,.fd=fd,.ident=ident,.events=ev,.cb=(ALooperCallback)cb,.data=d};
    condvarWakeAll(&l->cv);
    mutexUnlock(&l->m);

    return 1;
}
int ALooper_removeFd(ALooper*l,int fd){
    if(!l)return 0;
    mutexLock(&l->m);
    for(int i=0;i<16;i++)if(l->regs[i].used&&l->regs[i].fd==fd){memset(&l->regs[i],0,sizeof(l->regs[i]));mutexUnlock(&l->m);return 1;}
    mutexUnlock(&l->m);return 0;
}

static void choreo_init(void){if(!g_choreo_init){mutexInit(&g_choreo_lock);g_choreo_init=1;}}
void*AChoreographer_getInstance(void){choreo_init();return(void*)&g_callbacks;}
void AChoreographer_postFrameCallback(void*c,void(*cb)(long,void*),void*d){(void)c;choreo_init();mutexLock(&g_choreo_lock);for(int i=0;i<32;i++)if(!g_callbacks[i].used){g_callbacks[i].used=1;g_callbacks[i].is64=0;g_callbacks[i].u.oldcb=cb;g_callbacks[i].data=d;break;}mutexUnlock(&g_choreo_lock);}
void AChoreographer_postFrameCallback64(void*c,void(*cb)(int64_t,void*),void*d){(void)c;choreo_init();mutexLock(&g_choreo_lock);for(int i=0;i<32;i++)if(!g_callbacks[i].used){g_callbacks[i].used=1;g_callbacks[i].is64=1;g_callbacks[i].u.cb64=cb;g_callbacks[i].data=d;break;}mutexUnlock(&g_choreo_lock);}
void choreographer_tick(int64_t now){choreo_init();ChCb local[32];memset(local,0,sizeof(local));mutexLock(&g_choreo_lock);for(int i=0;i<32;i++)if(g_callbacks[i].used){local[i]=g_callbacks[i];g_callbacks[i].used=0;}mutexUnlock(&g_choreo_lock);for(int i=0;i<32;i++)if(local[i].used){if(local[i].is64)local[i].u.cb64(now,local[i].data);else local[i].u.oldcb((long)now,local[i].data);}}

void*ASensorManager_getInstance(void){static int x;return&x;}int ASensorManager_getSensorList(void*m,void**l){(void)m;if(l)*l=NULL;return 0;}void*ASensorManager_getDefaultSensor(void*m,int t){(void)m;(void)t;return NULL;}void*ASensorManager_createEventQueue(void*m,void*l,int i,void*c,void*d){(void)m;(void)l;(void)i;(void)c;(void)d;static int q;return&q;}int ASensorManager_destroyEventQueue(void*m,void*q){(void)m;(void)q;return 0;}int ASensorEventQueue_enableSensor(void*q,const void*s){(void)q;(void)s;return-1;}int ASensorEventQueue_disableSensor(void*q,const void*s){(void)q;(void)s;return 0;}int ASensorEventQueue_setEventRate(void*q,const void*s,int32_t u){(void)q;(void)s;(void)u;return 0;}int ASensorEventQueue_getEvents(void*q,void*e,size_t n){(void)q;(void)e;(void)n;return 0;}int ASensorEventQueue_hasEvents(void*q){(void)q;return 0;}const char*ASensor_getName(const void*s){(void)s;return"";}const char*ASensor_getVendor(const void*s){(void)s;return"";}int ASensor_getType(const void*s){(void)s;return 0;}float ASensor_getResolution(const void*s){(void)s;return 0;}int ASensor_getMinDelay(const void*s){(void)s;return 0;}

#define NDKSYM(x) if(!strcmp(name,#x))return(uintptr_t)&x
uintptr_t android_ndk_lookup(const char *name){if(!name)return 0;
NDKSYM(ANativeWindow_acquire);NDKSYM(ANativeWindow_release);NDKSYM(ANativeWindow_fromSurface);NDKSYM(ANativeWindow_toSurface);NDKSYM(ANativeWindow_getWidth);NDKSYM(ANativeWindow_getHeight);NDKSYM(ANativeWindow_setBuffersGeometry);
NDKSYM(ALooper_prepare);NDKSYM(ALooper_forThread);NDKSYM(ALooper_acquire);NDKSYM(ALooper_release);NDKSYM(ALooper_wake);NDKSYM(ALooper_pollOnce);NDKSYM(ALooper_addFd);NDKSYM(ALooper_removeFd);
NDKSYM(AChoreographer_getInstance);NDKSYM(AChoreographer_postFrameCallback);NDKSYM(AChoreographer_postFrameCallback64);
NDKSYM(ASensorManager_getInstance);NDKSYM(ASensorManager_getSensorList);NDKSYM(ASensorManager_getDefaultSensor);NDKSYM(ASensorManager_createEventQueue);NDKSYM(ASensorManager_destroyEventQueue);NDKSYM(ASensorEventQueue_enableSensor);NDKSYM(ASensorEventQueue_disableSensor);NDKSYM(ASensorEventQueue_setEventRate);NDKSYM(ASensorEventQueue_getEvents);NDKSYM(ASensorEventQueue_hasEvents);NDKSYM(ASensor_getName);NDKSYM(ASensor_getVendor);NDKSYM(ASensor_getType);NDKSYM(ASensor_getResolution);NDKSYM(ASensor_getMinDelay);
return 0;}
#undef NDKSYM

static Thread g_choreo_thread;static volatile int g_choreo_stop;static int g_choreo_started;
static void choreo_thread_main(void *arg){(void)arg;while(!g_choreo_stop){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);int64_t ns=(int64_t)ts.tv_sec*1000000000ll+ts.tv_nsec;choreographer_tick(ns);svcSleepThread(16000000ull);}}
int choreographer_driver_start(void){if(g_choreo_started)return 0;g_choreo_stop=0;Result r=threadCreate(&g_choreo_thread,choreo_thread_main,NULL,NULL,0x10000,0x2c,-2);if(R_FAILED(r))return-1;(void)asnx_thread_allow_all_cores(&g_choreo_thread,NULL);r=threadStart(&g_choreo_thread);if(R_FAILED(r)){threadClose(&g_choreo_thread);return-1;}g_choreo_started=1;return 0;}
void choreographer_driver_stop(void){if(!g_choreo_started)return;g_choreo_stop=1;threadWaitForExit(&g_choreo_thread);threadClose(&g_choreo_thread);g_choreo_started=0;}
