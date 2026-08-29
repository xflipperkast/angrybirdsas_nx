#ifndef ASNX_INPUT_H
#define ASNX_INPUT_H
#include <stdint.h>
typedef uint8_t (*UnityInjectFn)(void*,void*,void*,int32_t);
void input_init(void);
void input_feed(UnityInjectFn fn,void *env,void *thiz);
int input_cursor_visible(void);
void input_draw_cursor_overlay(void);
#endif
