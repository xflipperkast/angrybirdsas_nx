#include "bionic.h"
#include "config.h"
#include "so_util.h"
#include "crash_log.h"
#include <switch.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>

#define SYNC_SLOTS 2048
#define TLS_KEYS 128

enum {
    BIONIC_MUTEX_NORMAL = 0,
    BIONIC_MUTEX_RECURSIVE = 1,
    BIONIC_MUTEX_ERRORCHECK = 2,
};

enum {
    BIONIC_EDEADLK   = 35,
    BIONIC_ENOSYS    = 38,
    BIONIC_ETIMEDOUT = 110,
};

typedef struct {
    void *key;
    int used;
    int kind;
    Mutex m;
    CondVar cv;
    int count;
    int mutex_type;
    int mutex_type_known;
    int owner_tid;
    uint32_t recursion_depth;
    int cond_clock;
} SyncSlot;
static SyncSlot g_slots[SYNC_SLOTS];
static Mutex g_slots_lock;
static int g_slots_ready;
static __thread void *g_tls[TLS_KEYS];
static void (*g_tls_dtors[TLS_KEYS])(void*);
static unsigned g_next_key=1;
static Mutex g_key_lock;
static Mutex g_thread_lock;
static Mutex g_futex_registry_lock;

static uint8_t g_main_bionic_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
static uintptr_t g_main_stack_lo;
static uintptr_t g_main_stack_hi;

void bionic_install_tls(void *block){
    if(!block)return;
    memset(block,0,BIONIC_TLS_SIZE);
#if defined(__aarch64__)
    void *tp=(uint8_t*)block+BIONIC_TLS_TP_OFFSET;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tp) : "memory");
#else
    (void)block;
#endif
}
void bionic_install_main_tls(void){bionic_install_tls(g_main_bionic_tls);}
void bionic_set_main_stack_range(void *base,size_t size){
    g_main_stack_lo=(uintptr_t)base;
    g_main_stack_hi=base ? (uintptr_t)base+size : 0;
}

static void ensure_bionic_state_initialized(void){if(!g_slots_ready){mutexInit(&g_slots_lock);mutexInit(&g_key_lock);mutexInit(&g_thread_lock);mutexInit(&g_futex_registry_lock);g_slots_ready=1;}}
static SyncSlot *slot_get(void *key,int kind,int create){
    ensure_bionic_state_initialized();
    mutexLock(&g_slots_lock);
    SyncSlot *freep=NULL;
    for(int i=0;i<SYNC_SLOTS;i++){
        if(g_slots[i].used&&g_slots[i].key==key&&g_slots[i].kind==kind){
            mutexUnlock(&g_slots_lock);
            return &g_slots[i];
        }
        if(!g_slots[i].used&&!freep)freep=&g_slots[i];
    }
    if(create&&freep){
        memset(freep,0,sizeof(*freep));
        freep->used=1;
        freep->key=key;
        freep->kind=kind;
        mutexInit(&freep->m);
        condvarInit(&freep->cv);
        mutexUnlock(&g_slots_lock);
        return freep;
    }
    mutexUnlock(&g_slots_lock);
    return NULL;
}

static void slot_release(void *key,int kind){
    if(!key)return;
    ensure_bionic_state_initialized();
    mutexLock(&g_slots_lock);
    for(int i=0;i<SYNC_SLOTS;i++)if(g_slots[i].used&&g_slots[i].key==key&&g_slots[i].kind==kind){
        g_slots[i].used=0;
        g_slots[i].key=NULL;
        break;
    }
    mutexUnlock(&g_slots_lock);
}
int *bionic_errno(void){return &errno;}

#define MMAP_SLOTS 256
#define UNITY_DYNAMIC_RESERVE_LOGICAL 0x07fff000ull
#define UNITY_DYNAMIC_REGION_BYTES    0x04000000ull
#define UNITY_DYNAMIC_DIRECT_LOGICAL  UNITY_DYNAMIC_REGION_BYTES

#define UNITY_ALIGNED256_RESERVE_LOGICAL 0x13fff000ull
#define UNITY_ALIGNED256_REGION_BYTES    0x10000000ull
#define UNITY_ALIGNED256_LIVE_SLOTS      4u
#define UNITY_ARENA_MAX_SLOTS 33
_Static_assert(UNITY_DYNAMIC_DIRECT_LOGICAL == UNITY_DYNAMIC_REGION_BYTES,
               "64M Unity direct geometry");
_Static_assert(UNITY_DYNAMIC_RESERVE_LOGICAL == 2ull*UNITY_DYNAMIC_REGION_BYTES-0x1000ull,
               "64M Unity reserve geometry");
_Static_assert(UNITY_ALIGNED256_REGION_BYTES == UNITY_ALIGNED256_LIVE_SLOTS*UNITY_DYNAMIC_REGION_BYTES,
               "256M Unity live geometry");
_Static_assert(UNITY_ALIGNED256_RESERVE_LOGICAL ==
               (UNITY_ALIGNED256_LIVE_SLOTS+1ull)*UNITY_DYNAMIC_REGION_BYTES-0x1000ull,
               "256M+64M Unity reserve geometry");

typedef struct {
    void *base;
    size_t logical_size;
    size_t backing_size;
    uintptr_t live_lo;
    uintptr_t live_hi;
    int used;
    int unity_aligned_reserve;
    int arena_index;
    unsigned arena_count;
    unsigned arena_keep_count;
} MmapSlot;

static MmapSlot g_mmaps[MMAP_SLOTS];
static Mutex g_mmap_lock;
static Mutex g_mmap_io_lock;
static int g_mmap_ready;
static size_t g_mmap_backing_bytes;
static uint8_t *g_unity_arena_base;
static unsigned g_unity_arena_slots;
static uint64_t g_unity_arena_used;
static unsigned g_unity_arena_peak_slots;
static unsigned g_unity_fallback_count;

static void initialize_mmap_state(void){
    if(!g_mmap_ready){
        mutexInit(&g_mmap_lock);
        mutexInit(&g_mmap_io_lock);
        g_mmap_ready=1;
    }
}

void bionic_set_mmap_arena(void *base,size_t size){
    initialize_mmap_state();
    unsigned slots=(unsigned)(size/(size_t)UNITY_DYNAMIC_REGION_BYTES);
    int ok=base && slots>0 && slots<=UNITY_ARENA_MAX_SLOTS &&
           (((uintptr_t)base&(UNITY_DYNAMIC_REGION_BYTES-1u))==0) &&
           ((size%(size_t)UNITY_DYNAMIC_REGION_BYTES)==0);
    mutexLock(&g_mmap_lock);
    g_unity_arena_base=ok?(uint8_t*)base:NULL;
    g_unity_arena_slots=ok?slots:0;
    g_unity_arena_used=0;
    g_unity_arena_peak_slots=0;
    g_unity_fallback_count=0;
    mutexUnlock(&g_mmap_lock);
}

static size_t page_up_size(size_t n){
    if(n > SIZE_MAX - 0xfffu) return 0;
    return (n + 0xfffu) & ~(size_t)0xfffu;
}

static uint64_t unity_arena_mask(unsigned first,unsigned count){
    if(!count||count>64u||first>=64u||first+count>64u)return 0;
    uint64_t bits=count==64u?UINT64_MAX:((1ull<<count)-1ull);
    return bits<<first;
}

static int unity_arena_take_run(unsigned count,int from_high,void **out){
    *out=NULL;
    if(!count||count>UNITY_ARENA_MAX_SLOTS||!g_unity_arena_base||
       g_unity_arena_slots<count)return -1;

    if(from_high){
        for(unsigned n=g_unity_arena_slots-count+1u;n>0;n--){
            unsigned i=n-1u;
            uint64_t mask=unity_arena_mask(i,count);
            if(mask&&!(g_unity_arena_used&mask)){
                g_unity_arena_used|=mask;
                unsigned used=(unsigned)__builtin_popcountll(g_unity_arena_used);
                if(used>g_unity_arena_peak_slots)g_unity_arena_peak_slots=used;
                *out=g_unity_arena_base+(size_t)i*(size_t)UNITY_DYNAMIC_REGION_BYTES;
                return (int)i;
            }
        }
    }else{
        for(unsigned i=0;i+count<=g_unity_arena_slots;i++){
            uint64_t mask=unity_arena_mask(i,count);
            if(mask&&!(g_unity_arena_used&mask)){
                g_unity_arena_used|=mask;
                unsigned used=(unsigned)__builtin_popcountll(g_unity_arena_used);
                if(used>g_unity_arena_peak_slots)g_unity_arena_peak_slots=used;
                *out=g_unity_arena_base+(size_t)i*(size_t)UNITY_DYNAMIC_REGION_BYTES;
                return (int)i;
            }
        }
    }
    return -1;
}

static void unity_arena_release_run(unsigned first,unsigned count){
    if(!count||first>=UNITY_ARENA_MAX_SLOTS||first+count>UNITY_ARENA_MAX_SLOTS)return;
    uint64_t mask=unity_arena_mask(first,count);
    g_unity_arena_used&=~mask;
}

static ssize_t read_mapped_file_range(int fd, void *dst, size_t len, long off){
    if(fd<0||off<0){ errno=EINVAL; return -1; }
    mutexLock(&g_mmap_io_lock);
    off_t old=lseek(fd,0,SEEK_CUR);
    if(old==(off_t)-1){ int e=errno; mutexUnlock(&g_mmap_io_lock); errno=e; return -1; }
    if(lseek(fd,(off_t)off,SEEK_SET)==(off_t)-1){ int e=errno; mutexUnlock(&g_mmap_io_lock); errno=e; return -1; }

    size_t done=0;
    while(done<len){
        ssize_t r=read(fd,(uint8_t*)dst+done,len-done);
        if(r>0){ done+=(size_t)r; continue; }
        if(r==0) break;
        if(errno==EINTR) continue;
        int e=errno;
        (void)lseek(fd,old,SEEK_SET);
        mutexUnlock(&g_mmap_io_lock);
        errno=e;
        return -1;
    }
    int e=errno;
    if(lseek(fd,old,SEEK_SET)==(off_t)-1){ e=errno; mutexUnlock(&g_mmap_io_lock); errno=e; return -1; }
    mutexUnlock(&g_mmap_io_lock);
    errno=e;
    return (ssize_t)done;
}

void *bionic_mmap(void*a,size_t n,int prot,int flags,int fd,long off){
    (void)a;(void)prot;
    if(!n){errno=EINVAL;return(void*)-1;}
    size_t logical_size=page_up_size(n);
    if(!logical_size){errno=ENOMEM;return(void*)-1;}

    const int guest_map_anonymous=0x20;
    int file_backed=(fd>=0)&&((flags&guest_map_anonymous)==0);
    if(file_backed&&off<0){errno=EINVAL;return(void*)-1;}

    int unity_dynamic_window=!file_backed&&
        (logical_size==(size_t)UNITY_DYNAMIC_RESERVE_LOGICAL);
    int unity_dynamic_direct=!file_backed&&
        (logical_size==(size_t)UNITY_DYNAMIC_DIRECT_LOGICAL);
    int unity_aligned256=!file_backed&&
        (logical_size==(size_t)UNITY_ALIGNED256_RESERVE_LOGICAL);
    int unity_reserve=unity_dynamic_window||unity_dynamic_direct||unity_aligned256;
    unsigned arena_keep_count=(unity_dynamic_window||unity_dynamic_direct) ? 1u :
                              unity_aligned256 ? UNITY_ALIGNED256_LIVE_SLOTS : 0u;

    /* Unity's generic aligned VM helper asks for 0x07fff000 to obtain one
     * 64 MiB-aligned 64 MiB Dynamic Heap region. Arena slots already satisfy
     * that alignment, so expose the logical window while backing it with one
     * real slot. This preserves Unity's alignment contract without requiring
     * two adjacent arena slots. */
    unsigned arena_count=(unity_dynamic_window||unity_dynamic_direct) ? 1u :
                         arena_keep_count ? arena_keep_count+1u : 0u;
    size_t backing_size=logical_size;
    void *p=NULL;
    int arena_index=-1;

    initialize_mmap_state();
    if(unity_reserve){
        mutexLock(&g_mmap_lock);

        arena_index=unity_arena_take_run(arena_count,0,&p);
        mutexUnlock(&g_mmap_lock);
        if(arena_index>=0)
            backing_size=(size_t)arena_count*(size_t)UNITY_DYNAMIC_REGION_BYTES;
    }

    if(!p){
        size_t alignment=unity_reserve ? (size_t)UNITY_DYNAMIC_REGION_BYTES : 0x1000u;
        backing_size=(unity_dynamic_window||unity_dynamic_direct) ?
                     (size_t)UNITY_DYNAMIC_REGION_BYTES : logical_size;
        p=memalign(alignment,backing_size);
        if(!p){
            errno=ENOMEM;
            return(void*)-1;
        }
        if(unity_reserve){
            mutexLock(&g_mmap_lock);
            ++g_unity_fallback_count;
            mutexUnlock(&g_mmap_lock);
        }
    }

    if(unity_reserve && (((uintptr_t)p & (UNITY_DYNAMIC_REGION_BYTES-1u))!=0)){
        if(arena_index>=0){
            mutexLock(&g_mmap_lock);
            unity_arena_release_run((unsigned)arena_index,arena_count);
            mutexUnlock(&g_mmap_lock);
        }else free(p);
        errno=ENOMEM;
        return(void*)-1;
    }
    memset(p,0,backing_size);
    if(file_backed){
        ssize_t got=read_mapped_file_range(fd,p,n,off);
        if(got<0){
            int e=errno;
            free(p);
            errno=e;
            return(void*)-1;
        }
    }

    mutexLock(&g_mmap_lock);
    MmapSlot *slot=NULL;
    for(int i=0;i<MMAP_SLOTS;i++) if(!g_mmaps[i].used){slot=&g_mmaps[i];break;}
    if(slot){
        slot->base=p;
        slot->logical_size=logical_size;
        slot->backing_size=backing_size;
        slot->live_lo=(uintptr_t)p;
        slot->live_hi=(uintptr_t)p+logical_size;
        slot->used=1;
        slot->unity_aligned_reserve=unity_dynamic_window||unity_aligned256;
        slot->arena_index=arena_index;
        slot->arena_count=arena_count;
        slot->arena_keep_count=arena_keep_count;
        g_mmap_backing_bytes+=backing_size;
    }
    mutexUnlock(&g_mmap_lock);

    if(!slot){
        if(arena_index>=0){
            mutexLock(&g_mmap_lock);
            unity_arena_release_run((unsigned)arena_index,arena_count);
            mutexUnlock(&g_mmap_lock);
        }else free(p);
        errno=ENOMEM;
        return(void*)-1;
    }

    return p;
}

int bionic_munmap(void*p,size_t n){
    if(!p||!n){errno=EINVAL;return-1;}
    size_t len=page_up_size(n);
    if(!len){errno=EINVAL;return-1;}
    uintptr_t start=(uintptr_t)p;
    uintptr_t end=start+len;
    if(end<start){errno=EINVAL;return-1;}

    initialize_mmap_state();
    void *to_free=NULL;
    size_t free_size=0;
    int released_arena=-1;
    unsigned released_arena_count=0;
    int found=0;

    mutexLock(&g_mmap_lock);
    for(int i=0;i<MMAP_SLOTS;i++){
        MmapSlot *s=&g_mmaps[i];
        if(!s->used)continue;
        uintptr_t base=(uintptr_t)s->base;
        uintptr_t limit=base+s->logical_size;
        /* After Unity trims an aligned mmap window, the released address range
         * may immediately be reused by another mapping. Match munmap against
         * the current live interval, not the stale original logical window, or
         * an older record can steal the newer mapping's unmap and leak slots. */
        if(start<s->live_lo||start>=s->live_hi)continue;
        found=1;
        if(end>limit)end=limit;

        if(start<=s->live_lo && end>=s->live_hi){
            free_size=s->backing_size;
            if(g_mmap_backing_bytes>=free_size)g_mmap_backing_bytes-=free_size;
            else g_mmap_backing_bytes=0;
            if(s->arena_index>=0){
                released_arena=s->arena_index;
                released_arena_count=s->arena_count?s->arena_count:1u;
                unity_arena_release_run((unsigned)s->arena_index,released_arena_count);
            }else to_free=s->base;
            memset(s,0,sizeof(*s));
            s->arena_index=-1;
        }else if(start<=s->live_lo && end>s->live_lo){
            s->live_lo=end<s->live_hi?end:s->live_hi;
        }else if(end>=s->live_hi && start<s->live_hi){
            s->live_hi=start>s->live_lo?start:s->live_lo;

            if(s->arena_index>=0 && s->arena_keep_count>0u &&
               s->arena_count>s->arena_keep_count &&
               start==base+(size_t)s->arena_keep_count*(size_t)UNITY_DYNAMIC_REGION_BYTES &&
               end>=limit){
                unsigned trim_count=s->arena_count-s->arena_keep_count;
                unity_arena_release_run((unsigned)s->arena_index+s->arena_keep_count,
                                        trim_count);
                size_t old_backing=s->backing_size;
                s->arena_count=s->arena_keep_count;
                s->backing_size=(size_t)s->arena_keep_count*(size_t)UNITY_DYNAMIC_REGION_BYTES;
                if(g_mmap_backing_bytes>=old_backing-s->backing_size)
                    g_mmap_backing_bytes-=old_backing-s->backing_size;
                else g_mmap_backing_bytes=0;
                }
        }else if(start>=s->live_lo && end<=s->live_hi){
        }
        break;
    }
    mutexUnlock(&g_mmap_lock);

    if(released_arena>=0) return 0;
    if(to_free){ free(to_free); return 0; }
    if(found) return 0;

    return 0;
}
int bionic_mprotect(void*p,size_t n,int prot){(void)p;(void)n;(void)prot;return 0;}
int bionic_madvise(void*p,size_t n,int advice){(void)p;(void)n;(void)advice;return 0;}
#define ARM64_SYS_FUTEX 98
#define ARM64_SYS_SCHED_SETAFFINITY 122
#define ARM64_SYS_GETTID 178
#define ARM64_SYS_PROCESS_VM_READV 270
#define ARM64_SYS_PROCESS_VM_WRITEV 271
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CMD_MASK 0x7f

#define FUTEX_SLOTS 4096
_Static_assert((FUTEX_SLOTS & (FUTEX_SLOTS - 1u)) == 0, "FUTEX_SLOTS must be power-of-two");

typedef struct {
    volatile int32_t *key;
    int used;
    Mutex m;
    CondVar cv;
    unsigned waiters;
} FutexSlot;

static FutexSlot g_futex_slots[FUTEX_SLOTS];

static unsigned futex_slot_hash(volatile int32_t *key) {
    uintptr_t x = (uintptr_t)key >> 2;
    x ^= x >> 17;
    x ^= x >> 31;
    return (unsigned)x & (FUTEX_SLOTS - 1u);
}

static FutexSlot *futex_slot_get(volatile int32_t *key, int create) {
    FutexSlot *found = NULL;
    unsigned start = futex_slot_hash(key);

    ensure_bionic_state_initialized();
    mutexLock(&g_futex_registry_lock);
    for (unsigned probe = 0; probe < FUTEX_SLOTS; probe++) {
        FutexSlot *s = &g_futex_slots[(start + probe) & (FUTEX_SLOTS - 1u)];
        if (s->used) {
            if (s->key == key) {
                found = s;
                break;
            }
            continue;
        }
        if (!create) break;
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->key = key;
        mutexInit(&s->m);
        condvarInit(&s->cv);
        found = s;
        break;
    }
    mutexUnlock(&g_futex_registry_lock);
    return found;
}

static long futex_operation(volatile int32_t *u, int op, int val, const struct timespec *to) {
    int cmd = op & FUTEX_CMD_MASK;

    if (!u) {
        errno = EFAULT;
        return -1;
    }

    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        FutexSlot *s = futex_slot_get(u, 1);
        if (!s) {
            errno = ENOMEM;
            return -1;
        }

        mutexLock(&s->m);
        if (*u != val) {
            mutexUnlock(&s->m);
            errno = EAGAIN;
            return -1;
        }

        s->waiters++;
        Result wr = 0;
        if (to) {
            if (to->tv_sec < 0 || to->tv_nsec < 0 || to->tv_nsec >= 1000000000L) {
                s->waiters--;
                mutexUnlock(&s->m);
                errno = EINVAL;
                return -1;
            }
            uint64_t sec = (uint64_t)to->tv_sec;
            uint64_t ns;
            if (sec > (UINT64_MAX - (uint64_t)to->tv_nsec) / 1000000000ull)
                ns = UINT64_MAX;
            else
                ns = sec * 1000000000ull + (uint64_t)to->tv_nsec;
            wr = condvarWaitTimeout(&s->cv, &s->m, ns);
        } else {

            wr = condvarWaitTimeout(&s->cv, &s->m, 16000000ull);
        }
        s->waiters--;
        mutexUnlock(&s->m);

        if (to && R_FAILED(wr)) {
            errno = BIONIC_ETIMEDOUT;
            return -1;
        }
        return 0;
    }

    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        if (val <= 0) return 0;

        FutexSlot *s = futex_slot_get(u, 0);
        if (!s) return 0;

        mutexLock(&s->m);
        unsigned waiters = s->waiters;
        unsigned requested = (unsigned)val;
        unsigned wake_count = requested < waiters ? requested : waiters;

        if (wake_count == 1) {
            condvarWakeOne(&s->cv);
        } else if (wake_count > 1) {
            if (wake_count >= waiters) {
                condvarWakeAll(&s->cv);
            } else {
                for (unsigned i = 0; i < wake_count; i++)
                    condvarWakeOne(&s->cv);
            }
        }
        mutexUnlock(&s->m);

        return (long)wake_count;
    }

    errno = BIONIC_ENOSYS;
    return -1;
}
struct nx_iovec { void *iov_base; size_t iov_len; };
static int guest_memory_is_readable(uintptr_t a,size_t n){uintptr_t e=a+n;while(a<e){MemoryInfo mi;u32 pi;if(R_FAILED(svcQueryMemory(&mi,&pi,a))||mi.type==MemType_Unmapped||(mi.perm&Perm_R)==0)return 0;uintptr_t ne=mi.addr+mi.size;if(ne<=a)return 0;a=ne;}return 1;}
long bionic_syscall(long n,...){va_list a;va_start(a,n);long r=-1;
    if(n==ARM64_SYS_GETTID)r=bionic_gettid();
    else if(n==ARM64_SYS_SCHED_SETAFFINITY)r=0;
    else if(n==ARM64_SYS_FUTEX){volatile int32_t*u=va_arg(a,volatile int32_t*);int op=va_arg(a,int);int val=va_arg(a,int);const struct timespec*to=va_arg(a,const struct timespec*);r=futex_operation(u,op,val,to);}
    else if(n==ARM64_SYS_PROCESS_VM_READV||n==ARM64_SYS_PROCESS_VM_WRITEV){(void)va_arg(a,long);const struct nx_iovec*l=va_arg(a,const struct nx_iovec*);unsigned long lc=va_arg(a,unsigned long);const struct nx_iovec*rr=va_arg(a,const struct nx_iovec*);unsigned long rc=va_arg(a,unsigned long);(void)va_arg(a,unsigned long);unsigned long li=0,ri=0;size_t lo=0,ro=0;ssize_t total=0;int wr=n==ARM64_SYS_PROCESS_VM_WRITEV;while(li<lc&&ri<rc){size_t ln=l[li].iov_len-lo,rn=rr[ri].iov_len-ro,z=ln<rn?ln:rn;char*lp=(char*)l[li].iov_base+lo;char*rp=(char*)rr[ri].iov_base+ro;if(!guest_memory_is_readable((uintptr_t)(wr?lp:rp),z)){if(!total){errno=EFAULT;total=-1;}break;}if(wr)memcpy(rp,lp,z);else memcpy(lp,rp,z);total+=z;lo+=z;ro+=z;if(lo==l[li].iov_len){li++;lo=0;}if(ro==rr[ri].iov_len){ri++;ro=0;}}r=total;}
    else {errno=BIONIC_ENOSYS;r=-1;}va_end(a);return r;}
unsigned long bionic_getauxval(unsigned long t){if(t==6)return 0x1000;return 0;}
int bionic_gettid(void){u64 id=1;if(R_SUCCEEDED(svcGetThreadId(&id,CUR_THREAD_HANDLE)))return(int)(id&0x7fffffff);return 1;}
long bionic_sysconf(int name){switch(name){case 39:case 40:return 0x1000;case 96:case 97:return 3;case 98:return (512ll*1024*1024)/0x1000;default:return -1;}}
int bionic_clock_gettime(int id,struct timespec*tp){if(!tp){errno=EINVAL;return-1;}clockid_t h;switch(id){case 0:case 5:case 8:case 11:h=CLOCK_REALTIME;break;case 1:case 2:case 3:case 4:case 6:case 7:case 9:h=CLOCK_MONOTONIC;break;default:errno=EINVAL;tp->tv_sec=0;tp->tv_nsec=0;return-1;}return clock_gettime(h,tp);}
int bionic_clock_getres(int id,struct timespec*tp){if(!tp){errno=EINVAL;return-1;}struct timespec t; if(bionic_clock_gettime(id,&t)<0)return-1;tp->tv_sec=0;tp->tv_nsec=1000000;return 0;}

static int bionic_mutex_type_from_attr(const void *a, int *type) {
    uint64_t attr = 0;
    if (a) memcpy(&attr, a, sizeof(attr));
    int t = (int)(attr & 0x0fu);
    if (t < BIONIC_MUTEX_NORMAL || t > BIONIC_MUTEX_ERRORCHECK) return EINVAL;
    *type = t;
    return 0;
}

static void bionic_mutex_detect_static_type(SyncSlot *s, const void *k) {

    if (!s || !k || s->mutex_type_known) return;
    uint32_t v = 0;
    memcpy(&v, k, sizeof(v));
    int t = (int)((v >> 14) & 3u);
    if (t == BIONIC_MUTEX_RECURSIVE || t == BIONIC_MUTEX_ERRORCHECK)
        s->mutex_type = t;
    else
        s->mutex_type = BIONIC_MUTEX_NORMAL;
    s->mutex_type_known = 1;
}

int bionic_pthread_mutex_init(void*k,const void*a){
    if(!k)return EINVAL;
    int type=BIONIC_MUTEX_NORMAL;
    int rc=bionic_mutex_type_from_attr(a,&type);
    if(rc)return rc;
    SyncSlot*s=slot_get(k,1,1);
    if(!s)return ENOMEM;
    s->mutex_type=type;
    s->mutex_type_known=1;
    __atomic_store_n(&s->owner_tid,0,__ATOMIC_RELAXED);
    s->recursion_depth=0;
    return 0;
}
int bionic_pthread_mutex_destroy(void*k){
    slot_release(k,1);
    return 0;
}
int bionic_pthread_mutex_lock(void*k){
    SyncSlot*s=slot_get(k,1,1);if(!s)return ENOMEM;
    bionic_mutex_detect_static_type(s,k);
    if(s->mutex_type==BIONIC_MUTEX_NORMAL){mutexLock(&s->m);return 0;}
    int tid=bionic_gettid();
    if(__atomic_load_n(&s->owner_tid,__ATOMIC_ACQUIRE)==tid){
        if(s->mutex_type==BIONIC_MUTEX_ERRORCHECK)return BIONIC_EDEADLK;
        if(s->recursion_depth==UINT32_MAX)return EAGAIN;
        s->recursion_depth++;
        return 0;
    }
    mutexLock(&s->m);
    s->recursion_depth=1;
    __atomic_store_n(&s->owner_tid,tid,__ATOMIC_RELEASE);
    return 0;
}
int bionic_pthread_mutex_trylock(void*k){
    SyncSlot*s=slot_get(k,1,1);if(!s)return ENOMEM;
    bionic_mutex_detect_static_type(s,k);
    if(s->mutex_type==BIONIC_MUTEX_NORMAL)return mutexTryLock(&s->m)?0:EBUSY;
    int tid=bionic_gettid();
    if(__atomic_load_n(&s->owner_tid,__ATOMIC_ACQUIRE)==tid){
        if(s->mutex_type==BIONIC_MUTEX_ERRORCHECK)return BIONIC_EDEADLK;
        if(s->recursion_depth==UINT32_MAX)return EAGAIN;
        s->recursion_depth++;
        return 0;
    }
    if(!mutexTryLock(&s->m))return EBUSY;
    s->recursion_depth=1;
    __atomic_store_n(&s->owner_tid,tid,__ATOMIC_RELEASE);
    return 0;
}
int bionic_pthread_mutex_unlock(void*k){
    SyncSlot*s=slot_get(k,1,0);if(!s)return EINVAL;
    if(s->mutex_type==BIONIC_MUTEX_NORMAL){mutexUnlock(&s->m);return 0;}
    int tid=bionic_gettid();
    if(__atomic_load_n(&s->owner_tid,__ATOMIC_ACQUIRE)!=tid || s->recursion_depth==0)return EPERM;
    if(s->recursion_depth>1){s->recursion_depth--;return 0;}
    s->recursion_depth=0;
    __atomic_store_n(&s->owner_tid,0,__ATOMIC_RELEASE);
    mutexUnlock(&s->m);
    return 0;
}

static int bionic_cond_clock_from_attr(const void *a,int *clock_id){
    if(!clock_id)return EINVAL;
    if(!a){*clock_id=0;return 0;}
    uint64_t attr=0;
    memcpy(&attr,a,sizeof(attr));
    *clock_id=(int)((attr>>1)&1u);
    return 0;
}

int bionic_pthread_cond_init(void*k,const void*a){
    if(!k)return EINVAL;
    int clock_id=0;
    int rc=bionic_cond_clock_from_attr(a,&clock_id);
    if(rc)return rc;
    SyncSlot*s=slot_get(k,2,1);
    if(!s)return ENOMEM;
    s->cond_clock=clock_id;
    return 0;
}
int bionic_pthread_cond_destroy(void*k){
    slot_release(k,2);
    return 0;
}

static Result bionic_cond_wait_bridge(SyncSlot*cs,SyncSlot*ms,uint64_t timeout){
    if(ms->mutex_type==BIONIC_MUTEX_NORMAL)
        return condvarWaitTimeout(&cs->cv,&ms->m,timeout);

    int tid=bionic_gettid();
    if(__atomic_load_n(&ms->owner_tid,__ATOMIC_ACQUIRE)!=tid || ms->recursion_depth==0)
        return MAKERESULT(Module_Libnx,LibnxError_BadInput);

    if(ms->recursion_depth==1){

        ms->recursion_depth=0;
        __atomic_store_n(&ms->owner_tid,0,__ATOMIC_RELEASE);
        Result r=condvarWaitTimeout(&cs->cv,&ms->m,timeout);
        ms->recursion_depth=1;
        __atomic_store_n(&ms->owner_tid,tid,__ATOMIC_RELEASE);
        return r;
    }

    ms->recursion_depth--;
    mutexLock(&cs->m);
    Result r=condvarWaitTimeout(&cs->cv,&cs->m,timeout);
    mutexUnlock(&cs->m);
    ms->recursion_depth++;
    return r;
}

static int bionic_cond_abs_timeout_ns(int clock_id,const struct timespec*abs,uint64_t*out_ns){
    if(!abs||!out_ns||abs->tv_nsec<0||abs->tv_nsec>=1000000000L)return EINVAL;

    struct timespec now;
    if(bionic_clock_gettime(clock_id,&now)<0)return EINVAL;

    __int128 sec=(__int128)abs->tv_sec-(__int128)now.tv_sec;
    __int128 nsec=(__int128)abs->tv_nsec-(__int128)now.tv_nsec;
    if(nsec<0){sec--;nsec+=1000000000;}
    if(sec<0||(sec==0&&nsec<=0)){
        *out_ns=0;
        return BIONIC_ETIMEDOUT;
    }

    const __int128 max_wait=(__int128)UINT64_MAX-1;
    __int128 total=sec*1000000000+nsec;
    if(total>max_wait)total=max_wait;
    if(total<1)total=1;
    *out_ns=(uint64_t)total;
    return 0;
}

static int bionic_cond_result(Result r,int timed){
    if(R_SUCCEEDED(r))return 0;

    if(timed&&(uint32_t)r==0xEA01u)return BIONIC_ETIMEDOUT;
    return EINVAL;
}

#define BIONIC_COND_COOPERATIVE_NS 16000000ull
int bionic_pthread_cond_wait(void*c,void*m){
    SyncSlot*cs=slot_get(c,2,1);
    SyncSlot*ms=slot_get(m,1,1);
    if(!cs||!ms)return EINVAL;
    bionic_mutex_detect_static_type(ms,m);

    Result r=bionic_cond_wait_bridge(cs,ms,BIONIC_COND_COOPERATIVE_NS);
    if(R_SUCCEEDED(r)||(uint32_t)r==0xEA01u)return 0;
    return EINVAL;
}

int bionic_pthread_cond_timedwait(void*c,void*m,const struct timespec*t){
    SyncSlot*cs=slot_get(c,2,1);
    SyncSlot*ms=slot_get(m,1,1);
    if(!cs||!ms)return EINVAL;
    bionic_mutex_detect_static_type(ms,m);

    uint64_t timeout_ns=0;
    int rc=bionic_cond_abs_timeout_ns(cs->cond_clock,t,&timeout_ns);
    if(rc)return rc;
    return bionic_cond_result(bionic_cond_wait_bridge(cs,ms,timeout_ns),1);
}

int bionic_pthread_cond_signal(void*c){
    SyncSlot*s=slot_get(c,2,1);
    if(!s)return ENOMEM;
    return R_SUCCEEDED(condvarWakeOne(&s->cv))?0:EINVAL;
}
int bionic_pthread_cond_broadcast(void*c){
    SyncSlot*s=slot_get(c,2,1);
    if(!s)return ENOMEM;
    return R_SUCCEEDED(condvarWakeAll(&s->cv))?0:EINVAL;
}
int bionic_pthread_rwlock_init(void*k,const void*a){(void)a;return bionic_pthread_mutex_init(k,NULL);}
int bionic_pthread_rwlock_rdlock(void*k){return bionic_pthread_mutex_lock(k);}
int bionic_pthread_rwlock_wrlock(void*k){return bionic_pthread_mutex_lock(k);}
int bionic_pthread_rwlock_unlock(void*k){return bionic_pthread_mutex_unlock(k);}
int bionic_pthread_once(volatile int*o,void(*fn)(void)){if(__atomic_load_n(o,__ATOMIC_ACQUIRE)==2)return 0;int expected=0;if(__atomic_compare_exchange_n(o,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){fn();__atomic_store_n(o,2,__ATOMIC_RELEASE);}else while(__atomic_load_n(o,__ATOMIC_ACQUIRE)!=2)svcSleepThread(100000);return 0;}

typedef struct {
    uint32_t flags;
    uint32_t _pad;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
    char reserved[16];
} AndroidPthreadAttr;
_Static_assert(sizeof(AndroidPthreadAttr) == 56, "Android pthread_attr_t size");

typedef struct GuestThread {
    Thread th;
    void *(*fn)(void*);
    void *arg;
    void *ret;
    uint64_t pthread_id;
    int detached;
    int finished;
    int joining;
    struct GuestThread *next;
    uint8_t bionic_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
} GuestThread;

static GuestThread *g_guest_threads;

static GuestThread *guest_thread_find_locked(uint64_t id){
    for(GuestThread *g=g_guest_threads;g;g=g->next)
        if(g->pthread_id==id)return g;
    return NULL;
}

static void guest_thread_add(GuestThread *g){
    ensure_bionic_state_initialized();
    mutexLock(&g_thread_lock);
    g->next=g_guest_threads;
    g_guest_threads=g;
    mutexUnlock(&g_thread_lock);
}

static int guest_thread_remove_locked(GuestThread *g){
    GuestThread **pp=&g_guest_threads;
    while(*pp){
        if(*pp==g){*pp=g->next;g->next=NULL;return 1;}
        pp=&(*pp)->next;
    }
    return 0;
}

static void guest_thread_reap_detached(void){
    ensure_bionic_state_initialized();
    for(;;){
        GuestThread *victim=NULL;
        uint64_t self=(uint64_t)(uintptr_t)threadGetCurHandle();
        mutexLock(&g_thread_lock);
        for(GuestThread *g=g_guest_threads;g;g=g->next){

            if(g->detached&&!g->joining&&g->pthread_id!=self&&
               R_SUCCEEDED(waitSingleHandle(g->th.handle,0))){
                guest_thread_remove_locked(g);
                victim=g;
                break;
            }
        }
        mutexUnlock(&g_thread_lock);
        if(!victim)break;
        (void)threadClose(&victim->th);
        free(victim);
    }
}

static void guest_thread_run_dtors(void){
    for(unsigned i=1;i<TLS_KEYS;i++)
        if(g_tls[i]&&g_tls_dtors[i])g_tls_dtors[i](g_tls[i]);
}

static void thread_entry(void *p){
    GuestThread*g=p;
    bionic_install_tls(g->bionic_tls);
    void *ret=g->fn(g->arg);
    guest_thread_run_dtors();
    mutexLock(&g_thread_lock);
    g->ret=ret;
    g->finished=1;
    mutexUnlock(&g_thread_lock);
}

static int attr_fill_current_stack(AndroidPthreadAttr *a) {
    uintptr_t sp;
#if defined(__aarch64__)
    __asm__ volatile("mov %0, sp" : "=r"(sp));
#else
    sp=(uintptr_t)&sp;
#endif

    if(g_main_stack_lo && sp>=g_main_stack_lo && sp<g_main_stack_hi){
        a->stack_base=(void*)g_main_stack_lo;
        a->stack_size=(size_t)(g_main_stack_hi-g_main_stack_lo);
        return 0;
    }
    MemoryInfo mi; u32 pi=0;
    if (R_SUCCEEDED(svcQueryMemory(&mi,&pi,sp)) && mi.addr && mi.size) {
        a->stack_base=(void*)(uintptr_t)mi.addr;
        a->stack_size=(size_t)mi.size;
        return 0;
    }
    return ESRCH;
}

static int attr_fill_thread_stack(uint64_t t, AndroidPthreadAttr *a) {
    uintptr_t base=0;
    size_t size=0;
    uint64_t self=bionic_pthread_self();

    ensure_bionic_state_initialized();
    mutexLock(&g_thread_lock);
    GuestThread *g=guest_thread_find_locked(t);
    if(g){
        base=(uintptr_t)g->th.stack_mirror;
        size=g->th.stack_sz;
    }
    mutexUnlock(&g_thread_lock);

    if(base && size){
        a->stack_base=(void*)base;
        a->stack_size=size;
        return 0;
    }

    if(t==self)return attr_fill_current_stack(a);
    return ESRCH;
}

int bionic_pthread_create(uint64_t*out,const void*attr,void*(*fn)(void*),void*arg){
    if(!out||!fn)return EINVAL;
    guest_thread_reap_detached();
    GuestThread*g=calloc(1,sizeof(*g));if(!g)return ENOMEM;g->fn=fn;g->arg=arg;
    size_t stack=2u*1024u*1024u;
    if(attr){
        const AndroidPthreadAttr*a=(const AndroidPthreadAttr*)attr;
        if(a->stack_size>stack)stack=a->stack_size;
        g->detached=(a->flags&1u)?1:0;
    }
    if(stack>16u*1024u*1024u)stack=16u*1024u*1024u;
    Result r=threadCreate(&g->th,thread_entry,g,NULL,stack,0x2c,-2);
    if(R_FAILED(r)){free(g);return EAGAIN;}

    g->pthread_id=(uint64_t)(uintptr_t)g->th.handle;
    if(!g->pthread_id){(void)threadClose(&g->th);free(g);return EAGAIN;}
    guest_thread_add(g);
    *out=g->pthread_id;
    r=threadStart(&g->th);
    if(R_FAILED(r)){
        mutexLock(&g_thread_lock);
        guest_thread_remove_locked(g);
        mutexUnlock(&g_thread_lock);
        *out=0;
        (void)threadClose(&g->th);
        free(g);
        return EAGAIN;
    }
    return 0;
}

int bionic_pthread_join(uint64_t t,void**ret){
    uint64_t self=bionic_pthread_self();
    if(!t)return ESRCH;
    if(t==self)return BIONIC_EDEADLK;
    guest_thread_reap_detached();
    mutexLock(&g_thread_lock);
    GuestThread*g=guest_thread_find_locked(t);
    if(!g){mutexUnlock(&g_thread_lock);return ESRCH;}
    if(g->detached||g->joining){mutexUnlock(&g_thread_lock);return EINVAL;}
    g->joining=1;
    mutexUnlock(&g_thread_lock);

    Result r=threadWaitForExit(&g->th);
    if(R_FAILED(r)){
        mutexLock(&g_thread_lock);g->joining=0;mutexUnlock(&g_thread_lock);
        return EINVAL;
    }
    if(ret)*ret=g->ret;
    mutexLock(&g_thread_lock);
    guest_thread_remove_locked(g);
    mutexUnlock(&g_thread_lock);
    (void)threadClose(&g->th);
    free(g);
    return 0;
}

int bionic_pthread_detach(uint64_t t){
    if(!t)return ESRCH;
    ensure_bionic_state_initialized();
    mutexLock(&g_thread_lock);
    GuestThread*g=guest_thread_find_locked(t);
    if(!g){mutexUnlock(&g_thread_lock);return ESRCH;}
    if(g->detached||g->joining){mutexUnlock(&g_thread_lock);return EINVAL;}
    g->detached=1;
    mutexUnlock(&g_thread_lock);
    guest_thread_reap_detached();
    return 0;
}

void bionic_pthread_exit(void*r){
    uint64_t self=bionic_pthread_self();
    ensure_bionic_state_initialized();
    mutexLock(&g_thread_lock);
    GuestThread*g=guest_thread_find_locked(self);
    mutexUnlock(&g_thread_lock);
    guest_thread_run_dtors();
    if(g){
        mutexLock(&g_thread_lock);
        g->ret=r;
        g->finished=1;
        mutexUnlock(&g_thread_lock);
    }
    threadExit();
    __builtin_unreachable();
}
uint64_t bionic_pthread_self(void){return (uint64_t)(uintptr_t)threadGetCurHandle();}
int bionic_pthread_equal(uint64_t a,uint64_t b){return a==b;}
int bionic_pthread_setname_np(uint64_t t,const char*n){(void)t;(void)n;return 0;}
int bionic_pthread_attr_init(void*a){
    if(!a)return EINVAL;
    AndroidPthreadAttr*x=(AndroidPthreadAttr*)a;memset(x,0,sizeof(*x));
    x->stack_size=2u*1024u*1024u;
    return 0;
}
int bionic_pthread_attr_destroy(void*a){if(!a)return EINVAL;memset(a,0,sizeof(AndroidPthreadAttr));return 0;}
int bionic_pthread_attr_setdetachstate(void*a,int v){AndroidPthreadAttr*x=(AndroidPthreadAttr*)a;if(!x)return EINVAL;if(v)x->flags|=1u;else x->flags&=~1u;return 0;}
int bionic_pthread_attr_setstacksize(void*a,size_t n){AndroidPthreadAttr*x=(AndroidPthreadAttr*)a;if(!x||n<0x4000)return EINVAL;x->stack_size=n;return 0;}
int bionic_pthread_attr_getstack(const void*a,void**p,size_t*n){const AndroidPthreadAttr*x=(const AndroidPthreadAttr*)a;if(!x)return EINVAL;if(p)*p=x->stack_base;if(n)*n=x->stack_size;return 0;}
int bionic_pthread_getattr_np(uint64_t t,void*a){
    if(!a)return EINVAL;
    if(!t)return ESRCH;
    int r=bionic_pthread_attr_init(a);
    if(r)return r;
    r=attr_fill_thread_stack(t,(AndroidPthreadAttr*)a);
    if(r){memset(a,0,sizeof(AndroidPthreadAttr));return r;}
    return 0;
}
int bionic_pthread_key_create(unsigned*k,void(*d)(void*)){ensure_bionic_state_initialized();mutexLock(&g_key_lock);unsigned v=g_next_key<TLS_KEYS?g_next_key++:0;mutexUnlock(&g_key_lock);if(!v)return EAGAIN;g_tls_dtors[v]=d;*k=v;return 0;}
int bionic_pthread_key_delete(unsigned k){if(k<TLS_KEYS){g_tls[k]=NULL;g_tls_dtors[k]=NULL;}return 0;}int bionic_pthread_setspecific(unsigned k,const void*v){if(k>=TLS_KEYS)return EINVAL;g_tls[k]=(void*)v;return 0;}void*bionic_pthread_getspecific(unsigned k){return k<TLS_KEYS?g_tls[k]:NULL;}

typedef struct BoehmThreadRecordCompat {
    struct BoehmThreadRecordCompat *next;
    uint64_t pthread_id;
    uint64_t suspend_state;
    uintptr_t stack_ptr;
} BoehmThreadRecordCompat;

typedef struct {
    int used;
    uint64_t pthread_id;
    uintptr_t il2cpp_base;
    BoehmThreadRecordCompat *record;
    uint64_t marker;
} BoehmPausedThread;

#define BOEHM_PAUSED_MAX 64
static BoehmPausedThread g_boehm_paused[BOEHM_PAUSED_MAX];
static int g_boehm_suspend_sig=-1;
static int g_boehm_restart_sig=-1;
static void *g_boehm_suspend_handler;
static void *g_boehm_restart_handler;

static uintptr_t il2cpp_runtime_base(void){
    so_module *m=so_find_named(LIB_IL2CPP);
    return m?(uintptr_t)m->load_virtbase:0;
}

void bionic_note_sigaction(int sig,void *handler){
    uintptr_t base=il2cpp_runtime_base();
    if(!base||!handler)return;
    uintptr_t h=(uintptr_t)handler;
    if(h==base+OFF_IL2CPP_GC_SUSPEND_HANDLER){
        g_boehm_suspend_sig=sig;
        g_boehm_suspend_handler=handler;
    }else if(h==base+OFF_IL2CPP_GC_RESTART_HANDLER){
        g_boehm_restart_sig=sig;
        g_boehm_restart_handler=handler;
    }
}

static BoehmThreadRecordCompat *boehm_find_record(uintptr_t base,uint64_t tid){
    uint32_t h=(uint32_t)(tid>>8)^(uint32_t)tid;
    h^=h>>16;
    h&=0xffu;
    BoehmThreadRecordCompat **table=(BoehmThreadRecordCompat**)(base+OFF_IL2CPP_GC_THREAD_TABLE);
    for(BoehmThreadRecordCompat *r=table[h];r;r=r->next)
        if(r->pthread_id==tid)return r;
    return NULL;
}

static BoehmPausedThread *boehm_pause_find(uint64_t tid){
    for(unsigned i=0;i<BOEHM_PAUSED_MAX;i++)
        if(g_boehm_paused[i].used&&g_boehm_paused[i].pthread_id==tid)return &g_boehm_paused[i];
    return NULL;
}
static BoehmPausedThread *boehm_pause_alloc(uint64_t tid){
    BoehmPausedThread *p=boehm_pause_find(tid);
    if(p)return p;
    for(unsigned i=0;i<BOEHM_PAUSED_MAX;i++)if(!g_boehm_paused[i].used){
        memset(&g_boehm_paused[i],0,sizeof(g_boehm_paused[i]));
        g_boehm_paused[i].used=1;
        g_boehm_paused[i].pthread_id=tid;
        return &g_boehm_paused[i];
    }
    return NULL;
}

static int boehm_validate_thread(uint64_t tid){
    u64 kernel_tid=0;
    if(!tid||R_FAILED(svcGetThreadId(&kernel_tid,(Handle)tid)))return ESRCH;
    return 0;
}

static int boehm_horizon_suspend(uint64_t tid,uintptr_t base){
    int vr=boehm_validate_thread(tid);
    if(vr)return vr;
    if(tid==bionic_pthread_self())return EINVAL;

    BoehmThreadRecordCompat *rec=boehm_find_record(base,tid);
    if(!rec)return ESRCH;

    Result rc=svcSetThreadActivity((Handle)tid,ThreadActivity_Paused);
    if(R_FAILED(rc))return EINVAL;

    ThreadContext ctx;
    memset(&ctx,0,sizeof(ctx));
    rc=svcGetThreadContext3(&ctx,(Handle)tid);
    if(R_FAILED(rc)){
        (void)svcSetThreadActivity((Handle)tid,ThreadActivity_Runnable);
        return EINVAL;
    }

    BoehmPausedThread *ps=boehm_pause_alloc(tid);
    if(!ps){
        (void)svcSetThreadActivity((Handle)tid,ThreadActivity_Runnable);
        return EAGAIN;
    }
    uint64_t marker=__atomic_load_n((uint64_t*)(base+OFF_IL2CPP_GC_WORLD_MARKER),__ATOMIC_ACQUIRE);
    ps->il2cpp_base=base;
    ps->record=rec;
    ps->marker=marker;

    uintptr_t snap=((uintptr_t)ctx.sp-sizeof(ctx))&~(uintptr_t)0xf;
    MemoryInfo mi; u32 pageinfo=0;
    memset(&mi,0,sizeof(mi));
    if(R_FAILED(svcQueryMemory(&mi,&pageinfo,snap)) ||
       snap<(uintptr_t)mi.addr || (uintptr_t)ctx.sp>(uintptr_t)mi.addr+mi.size ||
       !(mi.perm&Perm_W)){
        ps->used=0;
        (void)svcSetThreadActivity((Handle)tid,ThreadActivity_Runnable);
        return EINVAL;
    }
    memcpy((void*)snap,&ctx,sizeof(ctx));
    rec->stack_ptr=snap;
    __atomic_store_n(&rec->suspend_state,marker,__ATOMIC_RELEASE);

    if(bionic_sem_post((void*)(base+OFF_IL2CPP_GC_ACK_SEM))<0){
        ps->used=0;
        (void)svcSetThreadActivity((Handle)tid,ThreadActivity_Runnable);
        return EINVAL;
    }
    return 0;
}

static int boehm_horizon_restart(uint64_t tid,uintptr_t base){
    int vr=boehm_validate_thread(tid);
    if(vr)return vr;
    if(tid==bionic_pthread_self())return EINVAL;

    BoehmPausedThread *ps=boehm_pause_find(tid);
    if(ps&&ps->il2cpp_base==base&&ps->record){

        int restart_ack=__atomic_load_n((int*)(base+OFF_IL2CPP_GC_RESTART_ACK_ENABLED),__ATOMIC_ACQUIRE);
        if(restart_ack){
            __atomic_store_n(&ps->record->suspend_state,ps->marker|1u,__ATOMIC_RELEASE);
            if(bionic_sem_post((void*)(base+OFF_IL2CPP_GC_ACK_SEM))<0)return EINVAL;
        }
    }

    Result rc=svcSetThreadActivity((Handle)tid,ThreadActivity_Runnable);
    if(R_FAILED(rc))return EINVAL;
    if(ps)memset(ps,0,sizeof(*ps));
    return 0;
}

int bionic_pthread_kill(uint64_t t,int sig){
    int vr=boehm_validate_thread(t);
    if(vr)return vr;
    if(sig==0)return 0;

    uintptr_t base=il2cpp_runtime_base();
    if(base&&sig==g_boehm_suspend_sig&&g_boehm_suspend_handler==(void*)(base+OFF_IL2CPP_GC_SUSPEND_HANDLER))
        return boehm_horizon_suspend(t,base);
    if(base&&sig==g_boehm_restart_sig&&g_boehm_restart_handler==(void*)(base+OFF_IL2CPP_GC_RESTART_HANDLER))
        return boehm_horizon_restart(t,base);

    return EINVAL;
}
int bionic_pthread_sigmask(int how,const void*set,void*oldset){
    (void)how;(void)set;

    if(oldset)memset(oldset,0,sizeof(uint64_t));
    return 0;
}

static int bionic_sem_abs_timeout_ns(const struct timespec *abs, uint64_t *out_ns){
    if(!abs||!out_ns||abs->tv_sec<0||abs->tv_nsec<0||abs->tv_nsec>=1000000000L){
        errno=EINVAL;
        return -1;
    }
    struct timespec now;
    if(bionic_clock_gettime(0,&now)<0)return -1;
    int64_t sec=(int64_t)abs->tv_sec-(int64_t)now.tv_sec;
    int64_t nsec=(int64_t)abs->tv_nsec-(int64_t)now.tv_nsec;
    if(nsec<0){sec--;nsec+=1000000000LL;}
    if(sec<0||(sec==0&&nsec<=0)){*out_ns=0;return 1;}
    uint64_t s=(uint64_t)sec;
    if(s>UINT64_MAX/1000000000ULL){*out_ns=UINT64_MAX;return 0;}
    uint64_t ns=s*1000000000ULL+(uint64_t)nsec;
    if(ns<16000ULL)ns=16000ULL;
    *out_ns=ns;
    return 0;
}

int bionic_sem_init(void*k,int shared,unsigned value){
    (void)shared;
    SyncSlot*s=slot_get(k,3,1);
    if(!s){errno=ENOMEM;return -1;}
    mutexLock(&s->m);
    s->count=(int)value;
    mutexUnlock(&s->m);
    return 0;
}
int bionic_sem_destroy(void*k){
    slot_release(k,3);
    return 0;
}
int bionic_sem_post(void*k){
    SyncSlot*s=slot_get(k,3,1);
    if(!s){errno=ENOMEM;return -1;}
    mutexLock(&s->m);s->count++;condvarWakeOne(&s->cv);mutexUnlock(&s->m);return 0;
}
int bionic_sem_wait(void*k){
    SyncSlot*s=slot_get(k,3,1);
    if(!s){errno=ENOMEM;return -1;}
    mutexLock(&s->m);while(s->count<=0)condvarWait(&s->cv,&s->m);s->count--;mutexUnlock(&s->m);return 0;
}
int bionic_sem_trywait(void*k){
    SyncSlot*s=slot_get(k,3,1);
    if(!s){errno=ENOMEM;return -1;}
    mutexLock(&s->m);if(s->count<=0){mutexUnlock(&s->m);errno=EAGAIN;return -1;}s->count--;mutexUnlock(&s->m);return 0;
}
int bionic_sem_timedwait(void*k,const struct timespec*t){
    SyncSlot*s=slot_get(k,3,1);
    if(!s){errno=ENOMEM;return -1;}
    mutexLock(&s->m);
    while(s->count<=0){
        uint64_t ns=0;
        int tr=bionic_sem_abs_timeout_ns(t,&ns);
        if(tr<0){mutexUnlock(&s->m);return -1;}
        if(tr>0){
            mutexUnlock(&s->m);
            errno=BIONIC_ETIMEDOUT;
            return -1;
        }
        Result r=condvarWaitTimeout(&s->cv,&s->m,ns);
        if(R_FAILED(r)){

            mutexUnlock(&s->m);
            errno=BIONIC_ETIMEDOUT;
            return -1;
        }
    }
    s->count--;
    mutexUnlock(&s->m);
    return 0;
}
int bionic_sem_getvalue(void*k,int*v){
    if(!v){errno=EINVAL;return -1;}
    SyncSlot*s=slot_get(k,3,1);
    if(!s){errno=ENOMEM;return -1;}
    mutexLock(&s->m);*v=s->count;mutexUnlock(&s->m);return 0;
}

void bionic_get_mmap_stats(BionicMmapStats *out){
    if(!out)return;
    memset(out,0,sizeof(*out));
    initialize_mmap_state();
    mutexLock(&g_mmap_lock);
    out->used_slots=(unsigned)__builtin_popcountll(g_unity_arena_used);
    out->total_slots=g_unity_arena_slots;
    out->peak_slots=g_unity_arena_peak_slots;
    out->fallback_count=g_unity_fallback_count;
    out->backing_bytes=g_mmap_backing_bytes;
    for(unsigned i=0;i<MMAP_SLOTS;i++){
        const MmapSlot *s=&g_mmaps[i];
        if(!s->used)continue;
        out->live_records++;
        if(s->live_hi>s->live_lo)out->live_bytes+=(size_t)(s->live_hi-s->live_lo);
    }
    mutexUnlock(&g_mmap_lock);
}
