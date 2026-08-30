#ifndef ASNX_UNITY_ENTRYPOINTS_H
#define ASNX_UNITY_ENTRYPOINTS_H
#include <stdint.h>
#include "config.h"
#include "runtime.h"
typedef int      (*fn_jnionload)(void*,void*);
typedef void     (*fn_initJni)(void*,void*,void*,int32_t,void*);
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_player_running)(void*,void*,int32_t);
#define UNITY_AT(off) ((void*)((uintptr_t)unity_mod.load_virtbase+(uintptr_t)(off)))
#define IL2CPP_AT(off) ((void*)((uintptr_t)il2cpp_mod.load_virtbase+(uintptr_t)(off)))
#endif
