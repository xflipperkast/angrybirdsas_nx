#ifndef ASNX_THREAD_UTIL_H
#define ASNX_THREAD_UTIL_H

#include <switch.h>

static inline Result asnx_thread_allow_all_cores(Thread *thread, u64 *out_mask){
    if(out_mask)*out_mask=0;
    if(!thread)return MAKERESULT(Module_Libnx,LibnxError_BadInput);
    u64 mask=0;
    Result rc=svcGetInfo(&mask,InfoType_CoreMask,CUR_PROCESS_HANDLE,0);
    if(R_FAILED(rc)||!mask)return R_FAILED(rc)?rc:MAKERESULT(Module_Libnx,LibnxError_BadInput);
    if(out_mask)*out_mask=mask;
    return svcSetThreadCoreMask(thread->handle,-1,(u32)mask);
}

#endif
