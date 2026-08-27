#ifndef ASNX_JNI_FAKE_H
#define ASNX_JNI_FAKE_H
#include <stdint.h>
#include <stddef.h>

#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_VERSION_1_6 0x00010006

typedef union JValue {
    uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i;
    int64_t j; float f; double d; void *l;
} JValue;

typedef struct FakeMotionEvent {
    int action;
    float x, y, pressure, size;
    int source, button_state, meta_state;
    int64_t down_time_ms, event_time_ms;
} FakeMotionEvent;

extern void *fake_env;
extern void *fake_vm;
extern void *fake_context_obj;
extern void *fake_unityplayer_thiz;
extern void *fake_surface_obj;
extern volatile int jni_quit_requested;

void jni_init(void);
void *jni_new_string(const char *utf8);
const char *jni_string_utf8(void *str);
void *jni_new_direct_buffer(void *ptr, size_t capacity);
void *jni_make_motion_event(const FakeMotionEvent *ev);
void jni_request_quit(void);

#endif
