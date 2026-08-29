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
    fprintf(g_trace_file,"---- angrybirdas_nx diagnostic trace %lld ----\n",(long long)wall);
    fprintf(g_trace_file,"format: +milliseconds [thread/core] CATEGORY message\n");
    fflush(g_trace_file);
    mutexUnlock(&g_trace_lock);
    return 0;
}

void trace_log_printf(const char *category,const char *fmt,...){
    if(!g_trace_file||!fmt)return;
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
#endif
