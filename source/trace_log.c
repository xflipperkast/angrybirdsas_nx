#include "trace_log.h"
#include "config.h"
#include <switch.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#if ENABLE_TRACE_LOG
#define TRACE_BANK_BYTES (256u * 1024u)

static FILE *g_trace_file;
static Mutex g_trace_lock;
static int g_trace_lock_ready;
static int g_trace_active;
static size_t g_trace_len[2];
static char g_trace_buf[2][TRACE_BANK_BYTES];
static uint64_t g_trace_start_us;
static uint64_t g_trace_last_pump_us;
static unsigned g_trace_dropped;
static Mutex g_main_block_lock;
static int g_main_block_lock_ready;
static Handle g_main_thread_handle;
static uint64_t g_main_block_begin_us;
static uintptr_t g_main_block_key;
static char g_main_block_kind[48];
static char g_main_block_detail[384];

static int trace_category_enabled(const char *category){
#if TRACE_FILTER_AUDIO_ONLY
    if(!category)return 1;
    static const char *const keep[]={
        "BOOT","UNITY","ANDROID","AUDIO","SCHED","FATAL"
    };
    for(unsigned i=0;i<sizeof(keep)/sizeof(keep[0]);i++)
        if(strcmp(category,keep[i])==0)return 1;
    return 0;
#elif TRACE_FILTER_LEVEL_LOAD
    if(!category)return 1;
    static const char *const keep[]={
        "BOOT","UNITY","FRAME","IO","IOSTAT","OPEN","MMAP","ZLIB","MEM","ANDROID",
        "FATAL","THREAD","SCHED","SYNC","HANG","MAIN","BLOCK","LOAD","SWAP","GLMEM",
        "GLERR","GL","GLWAIT","GLTEX","ETC2BC","PCSAMPLE","FUTEX","AUDIO"
    };
    for(unsigned i=0;i<sizeof(keep)/sizeof(keep[0]);i++)
        if(strcmp(category,keep[i])==0)return 1;
    return 0;
#else
    (void)category;
    return 1;
#endif
}

uint64_t trace_now_us(void){
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;
    return (uint64_t)ts.tv_sec*1000000ull+(uint64_t)ts.tv_nsec/1000ull;
}

static void trace_lock_init(void){
    if(g_trace_lock_ready)return;
    mutexInit(&g_trace_lock);
    g_trace_lock_ready=1;
}

static void main_block_lock_init(void){
    if(g_main_block_lock_ready)return;
    mutexInit(&g_main_block_lock);
    g_main_block_lock_ready=1;
}

int trace_is_main_thread(void){
    return g_main_thread_handle && threadGetCurHandle()==g_main_thread_handle;
}

void trace_main_thread_set(void){
    main_block_lock_init();
    mutexLock(&g_main_block_lock);
    g_main_thread_handle=threadGetCurHandle();
    g_main_block_begin_us=0;
    g_main_block_key=0;
    g_main_block_kind[0]=0;
    g_main_block_detail[0]=0;
    mutexUnlock(&g_main_block_lock);
}

void trace_main_block_begin(const char *kind,const void *key,const char *detail){
    if(!g_main_thread_handle || threadGetCurHandle()!=g_main_thread_handle)return;
    main_block_lock_init();
    mutexLock(&g_main_block_lock);
    g_main_block_begin_us=trace_now_us();
    g_main_block_key=(uintptr_t)key;
    snprintf(g_main_block_kind,sizeof(g_main_block_kind),"%s",kind?kind:"?");
    snprintf(g_main_block_detail,sizeof(g_main_block_detail),"%s",detail?detail:"");
    mutexUnlock(&g_main_block_lock);
}

void trace_main_block_end(void){
    if(!g_main_thread_handle || threadGetCurHandle()!=g_main_thread_handle)return;
    uint64_t begin=0;
    uintptr_t key=0;
    char kind[48]={0};
    char detail[384]={0};
    main_block_lock_init();
    mutexLock(&g_main_block_lock);
    begin=g_main_block_begin_us;
    key=g_main_block_key;
    snprintf(kind,sizeof(kind),"%s",g_main_block_kind);
    snprintf(detail,sizeof(detail),"%s",g_main_block_detail);
    g_main_block_begin_us=0;
    g_main_block_key=0;
    g_main_block_kind[0]=0;
    g_main_block_detail[0]=0;
    mutexUnlock(&g_main_block_lock);
    if(begin){
        uint64_t now=trace_now_us();
        uint64_t us=now>=begin?now-begin:0;
        if(us>=TRACE_MAIN_BLOCK_US)
            trace_log_printf("BLOCK","main kind=%s key=0x%llx time=%lluus%s%s",
                kind[0]?kind:"?",(unsigned long long)key,(unsigned long long)us,
                detail[0]?" detail=":"",detail);
    }
}

void trace_main_block_snapshot(const char *reason){
    uint64_t begin=0;
    uintptr_t key=0;
    char kind[48]={0};
    char detail[384]={0};
    main_block_lock_init();
    mutexLock(&g_main_block_lock);
    begin=g_main_block_begin_us;
    key=g_main_block_key;
    snprintf(kind,sizeof(kind),"%s",g_main_block_kind);
    snprintf(detail,sizeof(detail),"%s",g_main_block_detail);
    mutexUnlock(&g_main_block_lock);
    uint64_t now=trace_now_us();
    if(begin){
        trace_log_printf("MAIN","reason=%s blocked=%s key=0x%llx age=%llums%s%s",
            reason?reason:"?",kind[0]?kind:"?",(unsigned long long)key,
            (unsigned long long)((now>=begin?now-begin:0)/1000ull),
            detail[0]?" detail=":"",detail);
    }else{
        trace_log_printf("MAIN","reason=%s blocked=none state=inside-Unity-or-unwrapped-work",reason?reason:"?");
    }
}

int trace_log_init(void){
    trace_lock_init();
    mutexLock(&g_trace_lock);
    if(g_trace_file){mutexUnlock(&g_trace_lock);return 0;}
    g_trace_file=fopen(TRACE_LOG_PATH,"w");
    if(!g_trace_file){mutexUnlock(&g_trace_lock);return -1;}
    setvbuf(g_trace_file,NULL,_IOFBF,64u*1024u);
    g_trace_active=0;
    g_trace_len[0]=g_trace_len[1]=0;
    g_trace_dropped=0;
    g_trace_start_us=trace_now_us();
    g_trace_last_pump_us=g_trace_start_us;
    time_t wall=time(NULL);
    fprintf(g_trace_file,"---- angrybirdas_nx targeted audio trace %lld ----\n",(long long)wall);
    fprintf(g_trace_file,"format: +milliseconds [thread/core] CATEGORY message\n");
    fflush(g_trace_file);
    mutexUnlock(&g_trace_lock);
    return 0;
}

void trace_log_printf(const char *category,const char *fmt,...){
    if(!g_trace_file||!fmt||!trace_category_enabled(category))return;
    char msg[1024];
    va_list ap;
    va_start(ap,fmt);
    int mn=vsnprintf(msg,sizeof(msg),fmt,ap);
    va_end(ap);
    if(mn<0)return;
    size_t mlen=(size_t)mn;
    if(mlen>=sizeof(msg))mlen=sizeof(msg)-1u;

    uint64_t now=trace_now_us();
    uint64_t rel=now>=g_trace_start_us?now-g_trace_start_us:0;
    Handle tid=threadGetCurHandle();
    unsigned core=svcGetCurrentProcessorNumber();
    char line[1280];
    int n=snprintf(line,sizeof(line),"+%8llu.%03llu [%08x/c%u] %-7s %.*s%s",
        (unsigned long long)(rel/1000ull),(unsigned long long)(rel%1000ull),
        (unsigned)tid,core,category?category:"TRACE",(int)mlen,msg,
        (mlen&&msg[mlen-1]=='\n')?"":"\n");
    if(n<=0)return;
    size_t len=(size_t)n;
    if(len>=sizeof(line))len=sizeof(line)-1u;

    trace_lock_init();
    mutexLock(&g_trace_lock);
    int b=g_trace_active;
    if(len<=TRACE_BANK_BYTES-g_trace_len[b]){
        memcpy(g_trace_buf[b]+g_trace_len[b],line,len);
        g_trace_len[b]+=len;
    }else{
        g_trace_dropped++;
    }
    mutexUnlock(&g_trace_lock);
}

static void trace_pump_force(int force_flush){
    if(!g_trace_file)return;
    uint64_t now=trace_now_us();
    if(!force_flush&&now-g_trace_last_pump_us<TRACE_PUMP_INTERVAL_US)return;

    trace_lock_init();
    mutexLock(&g_trace_lock);
    int old=g_trace_active;
    int next=old^1;
    if(g_trace_len[next]!=0){
        mutexUnlock(&g_trace_lock);
        return;
    }
    size_t len=g_trace_len[old];
    unsigned dropped=g_trace_dropped;
    g_trace_dropped=0;
    g_trace_active=next;
    g_trace_last_pump_us=now;
    mutexUnlock(&g_trace_lock);

    if(len)fwrite(g_trace_buf[old],1,len,g_trace_file);
    if(dropped)fprintf(g_trace_file,"+%8llu.%03llu [logger] TRACE   dropped=%u lines (buffer full)\n",
        (unsigned long long)((now-g_trace_start_us)/1000ull),
        (unsigned long long)((now-g_trace_start_us)%1000ull),dropped);
    if(force_flush)fflush(g_trace_file);

    mutexLock(&g_trace_lock);
    g_trace_len[old]=0;
    mutexUnlock(&g_trace_lock);
}

void trace_log_pump(void){trace_pump_force(0);}
void trace_log_flush(void){trace_pump_force(1);if(g_trace_file)fflush(g_trace_file);}
void trace_log_close(void){
    if(!g_trace_file)return;
    trace_log_flush();
    trace_lock_init();
    mutexLock(&g_trace_lock);
    FILE *f=g_trace_file;
    g_trace_file=NULL;
    mutexUnlock(&g_trace_lock);
    if(f)fclose(f);
}
#else
uint64_t trace_now_us(void){
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;
    return (uint64_t)ts.tv_sec*1000000ull+(uint64_t)ts.tv_nsec/1000ull;
}
int trace_log_init(void){return 0;}
void trace_log_printf(const char *category,const char *fmt,...){(void)category;(void)fmt;}
void trace_log_pump(void){}
void trace_log_flush(void){}
void trace_log_close(void){}
void trace_main_thread_set(void){}
int trace_is_main_thread(void){return 0;}
void trace_main_block_begin(const char *kind,const void *key,const char *detail){(void)kind;(void)key;(void)detail;}
void trace_main_block_end(void){}
void trace_main_block_snapshot(const char *reason){(void)reason;}
#endif
