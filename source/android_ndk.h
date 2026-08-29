#ifndef ASNX_ANDROID_NDK_H
#define ASNX_ANDROID_NDK_H
#include <stdint.h>
#include <stddef.h>

typedef struct ANativeWindow ANativeWindow;
typedef struct ALooper ALooper;
ANativeWindow *android_native_window(void);
uint32_t android_width(void);
uint32_t android_height(void);
void android_update_mode(void);
void android_input_init(void);

void ANativeWindow_acquire(ANativeWindow*);
void ANativeWindow_release(ANativeWindow*);
ANativeWindow *ANativeWindow_fromSurface(void*,void*);
void *ANativeWindow_toSurface(void*,ANativeWindow*);
int32_t ANativeWindow_getWidth(ANativeWindow*);
int32_t ANativeWindow_getHeight(ANativeWindow*);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*,int32_t,int32_t,int32_t);

ALooper *ALooper_prepare(int);
ALooper *ALooper_forThread(void);
void ALooper_acquire(ALooper*);
void ALooper_release(ALooper*);
void ALooper_wake(ALooper*);
int ALooper_pollOnce(int,int*,int*,void**);
int ALooper_addFd(ALooper*,int,int,int,void*,void*);
int ALooper_removeFd(ALooper*,int);

void *AChoreographer_getInstance(void);
void AChoreographer_postFrameCallback(void*,void(*)(long,void*),void*);
void AChoreographer_postFrameCallback64(void*,void(*)(int64_t,void*),void*);
void choreographer_tick(int64_t now_ns);
int choreographer_driver_start(void);
void choreographer_driver_stop(void);
uintptr_t android_ndk_lookup(const char *name);

#endif
