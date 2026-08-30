#define _GNU_SOURCE
#include "imports.h"
#include "android_ndk.h"
#include "media_stubs.h"
#include "bionic.h"
#include "crash_log.h"
#include "jni_fake.h"
#include "config.h"
#include "fatal.h"
#include "file_bridge.h"
#include "fakefd.h"
#include "input.h"
#include "aaudio_bridge.h"
#include "trace_log.h"
#include "mem_diag.h"
#include "etc2_bc.h"
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>
#include <fnmatch.h>
#include <sys/file.h>

extern int sched_yield(void);

static FILE *guest_stdout,*guest_stderr;
static unsigned char guest_sF[2048];
static const char *g_dlerr="";
static int g_imports_ready;
static char g_prop_name[128],g_prop_value[128];

static uintptr_t generic_stub(void){return 0;}
uintptr_t imports_fallback(void){return(uintptr_t)&generic_stub;}

static int alog_vprint(int p,const char*t,const char*f,va_list a){
    if(p>=5&&f){
        char msg[768];
        va_list copy;
        va_copy(copy,a);
        vsnprintf(msg,sizeof(msg),f,copy);
        va_end(copy);
        trace_log_printf("ANDROID","prio=%d tag=%s %s",p,t?t:"?",msg);
    }
    return 0;
}
static int alog_print(int p,const char*t,const char*f,...){
    va_list a;
    va_start(a,f);
    int rc=alog_vprint(p,t,f,a);
    va_end(a);
    return rc;
}
static int alog_write(int p,const char*t,const char*s){
    if(p>=5&&s)trace_log_printf("ANDROID","prio=%d tag=%s %s",p,t?t:"?",s);
    return 0;
}
static int alog_buf_write(int b,int p,const char*t,const char*s){
    (void)b;
    return alog_write(p,t,s);
}

static void *nx_malloc(size_t n){return malloc(n);}
static void *nx_calloc(size_t n,size_t z){return calloc(n,z);}
static void *nx_realloc(void *old,size_t n){return realloc(old,n);}
static void *nx_memalign(size_t a,size_t n){return memalign(a,n);}

static char g_android_abort_message[1024];
static void android_abort_message(const char*s){
    if(!s){g_android_abort_message[0]=0;return;}
    snprintf(g_android_abort_message,sizeof(g_android_abort_message),"%s",s);
}
static void assert2(const char*file,int line,const char*fn,const char*expr){fatal_error("Android assert: %s:%d %s: %s",file?file:"?",line,fn?fn:"?",expr?expr:"?");}
static void stack_chk_fail(void){fatal_error("guest stack protector failure");}
static int cxa_atexit_stub(void(*f)(void*),void*a,void*d){(void)f;(void)a;(void)d;return 0;} static void cxa_finalize_stub(void*d){(void)d;} static int register_atfork_stub(void*a,void*b,void*c,void*d){(void)a;(void)b;(void)c;(void)d;return 0;}

static const char *prop_value(const char*n){if(!n)return"";if(!strcmp(n,"ro.build.version.sdk"))return"35";if(!strcmp(n,"ro.product.manufacturer"))return"Nintendo";if(!strcmp(n,"ro.product.model"))return"Nintendo Switch";if(!strcmp(n,"ro.product.device"))return"nx";if(!strcmp(n,"ro.build.version.release"))return"15";if(!strcmp(n,"ro.product.cpu.abi"))return"arm64-v8a";return"";}
static int system_property_get(const char*n,char*v){const char*s=prop_value(n);if(v)strcpy(v,s);return(int)strlen(s);} static const void*system_property_find(const char*n){snprintf(g_prop_name,sizeof g_prop_name,"%s",n?n:"");snprintf(g_prop_value,sizeof g_prop_value,"%s",prop_value(n));return g_prop_name;} static int system_property_read(const void*p,char*n,char*v){(void)p;if(n)strcpy(n,g_prop_name);if(v)strcpy(v,g_prop_value);return(int)strlen(g_prop_value);} static int android_api_level(void){return 35;}
#define ANDROID_FD_SETSIZE 1024
#define ANDROID_FDSET_WORD_BITS ((int)(8*sizeof(unsigned long)))
#define ANDROID_FDSET_WORDS (ANDROID_FD_SETSIZE/ANDROID_FDSET_WORD_BITS)
static int android_fd_test(const void *set,int fd){
    if(!set||fd<0||fd>=ANDROID_FD_SETSIZE)return 0;
    const unsigned long *w=(const unsigned long*)set;
    return (int)((w[fd/ANDROID_FDSET_WORD_BITS]>>(fd%ANDROID_FDSET_WORD_BITS))&1ul);
}
static void android_fd_setbit(void *set,int fd){
    if(!set||fd<0||fd>=ANDROID_FD_SETSIZE)return;
    unsigned long *w=(unsigned long*)set;
    w[fd/ANDROID_FDSET_WORD_BITS]|=1ul<<(fd%ANDROID_FDSET_WORD_BITS);
}
static int fd_set_chk(int fd,fd_set*s,size_t z){(void)z;if(fd<0||fd>=ANDROID_FD_SETSIZE){errno=EINVAL;return-1;}android_fd_setbit(s,fd);return 0;}
static int fd_isset_chk(int fd,fd_set*s,size_t z){(void)z;return android_fd_test(s,fd);}
static size_t ctype_mb_cur_max(void){return 1;}
static char *gnu_strerror_r(int e,char*b,size_t n){snprintf(b,n,"%s",strerror(e));return b;}
static char*nx_strcasestr(const char*h,const char*n){if(!*n)return(char*)h;size_t z=strlen(n);for(;*h;h++)if(!strncasecmp(h,n,z))return(char*)h;return NULL;}
static char*nx_basename(char*p){if(!p||!*p)return p;char*s=strrchr(p,'/');return s?s+1:p;}
static void nx_sincos(double x,double*s,double*c){if(s)*s=sin(x);if(c)*c=cos(x);}
static void nx_sincosf(float x,float*s,float*c){if(s)*s=sinf(x);if(c)*c=cosf(x);}
static size_t nx_strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;} static void*nx_memrchr(const void*p,int c,size_t n){const unsigned char*s=p;while(n){n--;if(s[n]==(unsigned char)c)return(void*)(s+n);}return NULL;}
static int nx_prctl(int option,...){(void)option;return 0;} static long nx_process_vm_readv(int pid,const void*l,unsigned long lc,const void*r,unsigned long rc,unsigned long flags){(void)pid;(void)l;(void)lc;(void)r;(void)rc;(void)flags;errno=ENOSYS;return-1;} static long nx_ptrace(int req,...){(void)req;errno=EPERM;return-1;}
static int nx_fork(void){errno=ENOSYS;return-1;} static int nx_execl(const char*p,const char*a,...){(void)p;(void)a;errno=ENOSYS;return-1;} static int nx_waitpid(int p,int*s,int o){(void)p;(void)s;(void)o;errno=ECHILD;return-1;} static int nx_kill(int p,int s){(void)p;(void)s;return 0;}

typedef struct {
    int32_t flags;
    uint32_t pad;
    void *handler;
    uint64_t mask;
    void *restorer;
} AndroidSigaction64;
typedef char android_sigaction64_must_be_32[(sizeof(AndroidSigaction64)==32)?1:-1];

static AndroidSigaction64 g_guest_sigactions[65];

static int nx_sigaction(int sig,const void *new_action,void *old_action){
    if(sig<=0||sig>=65){errno=EINVAL;return-1;}
    if(old_action)memcpy(old_action,&g_guest_sigactions[sig],sizeof(AndroidSigaction64));
    if(new_action){
        memcpy(&g_guest_sigactions[sig],new_action,sizeof(AndroidSigaction64));
        bionic_note_sigaction(sig,g_guest_sigactions[sig].handler);
    }
    return 0;
}
static void(*nx_signal(int sig,void(*handler)(int)))(int){
    if(sig<=0||sig>=65){errno=EINVAL;return SIG_ERR;}
    void(*old)(int)=(void(*)(int))g_guest_sigactions[sig].handler;
    g_guest_sigactions[sig].handler=(void*)handler;
    bionic_note_sigaction(sig,(void*)handler);
    return old;
}
static int nx_raise(int sig){(void)sig;return 0;}
static int nx_sigaltstack(const void*a,void*b){(void)a;(void)b;return 0;}
struct nx_iovec_import { void *iov_base; size_t iov_len; };

static ssize_t nx_read(int fd,void *buf,size_t count){
    if(fakefd_is_range(fd))return fakefd_read(fd,buf,count);
    char path[320]={0};
    (void)file_fd_path(fd,path,sizeof(path));
    char detail[384];
    snprintf(detail,sizeof(detail),"fd=%d req=%zu path=%s",fd,count,path[0]?path:"?");
    trace_main_block_begin("io:read",(const void*)(uintptr_t)fd,detail);
    uint64_t t0=trace_now_us();
    ssize_t got=read(fd,buf,count);
    uint64_t us=trace_now_us()-t0;
    trace_main_block_end();
    if(path[0]&&(strstr(path,"/UnityCache/")||strstr(path,"/assets/bin/Data/")) &&
       (us>=TRACE_ASSET_IO_US||count>=TRACE_LARGE_IO_BYTES))
        trace_log_printf("IO","read fd=%d req=%zu got=%lld time=%lluus path=%s",fd,count,(long long)got,(unsigned long long)us,path);
    return got;
}
static int nx_file_open_profile(const char *p,int flags,...){
    mode_t mode=0;
    if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=(mode_t)va_arg(ap,int);va_end(ap);}
    return (flags&O_CREAT)?file_open(p,flags,mode):file_open(p,flags);
}
static FILE *nx_file_fopen_profile(const char *p,const char *m){return file_fopen(p,m);}
static size_t nx_fread_profile(void *ptr,size_t size,size_t nmemb,FILE *stream){
    int fd=stream?fileno(stream):-1;
    size_t requested=size*nmemb;
    char path[320]={0};
    if(fd>=0)(void)file_fd_path(fd,path,sizeof(path));
    char detail[384];
    snprintf(detail,sizeof(detail),"fd=%d req=%zu path=%s",fd,requested,path[0]?path:"?");
    trace_main_block_begin("io:fread",(const void*)(uintptr_t)(fd>=0?fd:0),detail);
    uint64_t t0=trace_now_us();
    size_t got=fread(ptr,size,nmemb,stream);
    uint64_t us=trace_now_us()-t0;
    trace_main_block_end();
    int asset=path[0]&&(strstr(path,"/UnityCache/")||strstr(path,"/assets/bin/Data/"));
    if(us>=TRACE_SLOW_IO_US || (asset&&(us>=TRACE_ASSET_IO_US||requested>=TRACE_LARGE_IO_BYTES)))
        trace_log_printf("IO","fread fd=%d req=%zu got=%zu time=%lluus path=%s",fd,requested,size*got,(unsigned long long)us,path[0]?path:"?");
    return got;
}
static int nx_inflate_profile(z_streamp strm,int flush){
    uInt in0=strm?strm->avail_in:0;
    uInt out0=strm?strm->avail_out:0;
    char detail[96];
    snprintf(detail,sizeof(detail),"in=%u out=%u flush=%d",(unsigned)in0,(unsigned)out0,flush);
    trace_main_block_begin("cpu:zlib-inflate",strm,detail);
    uint64_t t0=trace_now_us();
    int rc=inflate(strm,flush);
    uint64_t us=trace_now_us()-t0;
    trace_main_block_end();
    if(us>=5000ull)trace_log_printf("ZLIB","inflate rc=%d in=%u out=%u time=%lluus",rc,
        (unsigned)(in0-(strm?strm->avail_in:in0)),(unsigned)(out0-(strm?strm->avail_out:out0)),
        (unsigned long long)us);
    return rc;
}

static ssize_t nx_write(int fd,const void *buf,size_t count){
    if(fakefd_is_range(fd))return fakefd_write(fd,buf,count);
    return write(fd,buf,count);
}
static int nx_close(int fd){
    if(fakefd_is_range(fd))return fakefd_close(fd);
    int rc=close(fd);
    if(rc==0)file_note_fd_closed(fd);
    return rc;
}
static off_t nx_lseek(int fd,off_t offset,int whence){
    if(fakefd_is_range(fd)){(void)offset;(void)whence;errno=ESPIPE;return(off_t)-1;}
    return lseek(fd,offset,whence);
}
static int nx_dup(int fd){
    if(fakefd_is_range(fd))return fakefd_dup(fd,ASNX_FAKE_FD_BASE,0);
    int n=dup(fd);
    if(n>=0)file_note_fd_duplicated(fd,n);
    return n;
}
static int nx_dup2(int oldfd,int newfd){
    if(fakefd_is_range(oldfd)){
        if(!fakefd_is_range(newfd)){errno=ENOSYS;return-1;}
        return fakefd_dup2(oldfd,newfd);
    }
    if(fakefd_is_range(newfd)){if(fakefd_is_open(newfd))fakefd_close(newfd);errno=EINVAL;return-1;}
    int n=dup2(oldfd,newfd);
    if(n>=0)file_note_fd_duplicated(oldfd,n);
    return n;
}
static ssize_t nx_writev(int fd,const struct nx_iovec_import *v,int count){
    if(!v||count<0){errno=EINVAL;return-1;}
    ssize_t total=0;
    for(int i=0;i<count;i++){
        const unsigned char *p=(const unsigned char*)v[i].iov_base; size_t left=v[i].iov_len;
        while(left){ssize_t n=nx_write(fd,p,left);if(n<0)return total?total:-1;if(n==0)return total;p+=n;left-=(size_t)n;total+=n;}
    }
    return total;
}
static ssize_t nx_readv(int fd,const struct nx_iovec_import *v,int count){
    if(!v||count<0){errno=EINVAL;return-1;}
    ssize_t total=0;
    for(int i=0;i<count;i++){
        unsigned char *p=(unsigned char*)v[i].iov_base; size_t left=v[i].iov_len;
        while(left){size_t requested=left;ssize_t n=nx_read(fd,p,requested);if(n<0)return total?total:-1;if(n==0)return total;p+=n;left-=(size_t)n;total+=n;if((size_t)n<requested)return total;}
    }
    return total;
}
static int nx_getpriority(int which,int who){(void)which;(void)who;return 0;}
static int nx_setpriority(int which,int who,int prio){(void)which;(void)who;(void)prio;return 0;}
static int nx_posix_memalign(void **memptr,size_t alignment,size_t size){
    if(!memptr)return EINVAL;
    if(alignment<sizeof(void*)||(alignment&(alignment-1))!=0||(alignment%sizeof(void*))!=0)return EINVAL;
    void *p=memalign(alignment,size?size:1);
    if(!p)return ENOMEM;
    *memptr=p;
    return 0;
}
static ssize_t nx_pread(int fd,void *buf,size_t count,off_t offset){
    if(fakefd_is_range(fd)){errno=ESPIPE;return -1;}
    return file_pread(fd,buf,count,offset);
}
static ssize_t nx_pwrite(int fd,const void *buf,size_t count,off_t offset){
    if(fakefd_is_range(fd)){errno=ESPIPE;return -1;}
    return file_pwrite(fd,buf,count,offset);
}

static int nx_ftruncate(int fd,off_t length){
    int rc=ftruncate(fd,length);
    int e=rc<0?errno:0;
    errno=e;
    return rc;
}

static int nx_fsync(int fd){
    int rc=fsync(fd);
    int e=rc<0?errno:0;
    errno=e;
    return rc;
}
static int nx_pipe(int fds[2]){return fakefd_pipe(fds);}
static int nx_flock(int fd,int operation){(void)fd;(void)operation;return 0;}
static unsigned nx_getuid(void){return 10000u;}
static unsigned nx_geteuid(void){return 10000u;}
static unsigned nx_getegid(void){return 10000u;}
static int nx_getpagesize(void){return 0x1000;}

__attribute__((noreturn)) static void nx_guest_exit(int code){
    fatal_error("Android guest called exit(%d)",code);
    __builtin_unreachable();
}
__attribute__((noreturn)) static void nx_guest_abort(void){
#if defined(__aarch64__)
    void *caller=__builtin_extract_return_addr(__builtin_return_address(0));
    if(g_android_abort_message[0])
        fatal_error("Android guest called abort() caller=%p: %s",caller,g_android_abort_message);
    fatal_error("Android guest called abort() caller=%p",caller);
#else
    if(g_android_abort_message[0])
        fatal_error("Android guest called abort(): %s",g_android_abort_message);
    fatal_error("Android guest called abort()");
#endif
    __builtin_unreachable();
}
__attribute__((noreturn)) static void nx_guest__exit(int code){
    fatal_error("Android guest called _exit(%d)",code);
    __builtin_unreachable();
}
static int nx_puts(const char *s){(void)s;return 0;}
static int nx_strerror_r(int err,char *buf,size_t len){
    if(!buf||len==0)return ERANGE;
    const char *s=strerror(err);
    if(!s)s="Unknown error";
    size_t n=strlen(s);
    if(n>=len){memcpy(buf,s,len-1);buf[len-1]=0;return ERANGE;}
    memcpy(buf,s,n+1);
    return 0;
}
static int nx_link(const char *oldpath,const char *newpath){
    (void)oldpath;(void)newpath;
    errno=EOPNOTSUPP;
    return -1;
}

enum {
    ANDROID_F_DUPFD=0, ANDROID_F_GETFD=1, ANDROID_F_SETFD=2,
    ANDROID_F_GETFL=3, ANDROID_F_SETFL=4, ANDROID_F_GETLK=5,
    ANDROID_F_SETLK=6, ANDROID_F_SETLKW=7, ANDROID_F_DUPFD_CLOEXEC=1030
};
#define NX_FD_TRACK_MAX 1024
static int g_fcntl_fdflags[NX_FD_TRACK_MAX];
static int g_fcntl_flflags[NX_FD_TRACK_MAX];
static int nx_fcntl(int fd,int cmd,...){
    if(fd<0){errno=EBADF;return -1;}
    int arg=0;
    int needs_arg=(cmd==ANDROID_F_SETFD||cmd==ANDROID_F_SETFL||cmd==ANDROID_F_DUPFD||cmd==ANDROID_F_DUPFD_CLOEXEC);
    if(needs_arg){va_list ap;va_start(ap,cmd);arg=va_arg(ap,int);va_end(ap);}
    if(fakefd_is_range(fd))return fakefd_fcntl(fd,cmd,arg);
    switch(cmd){
        case ANDROID_F_GETFD:
            return fd<NX_FD_TRACK_MAX?g_fcntl_fdflags[fd]:0;
        case ANDROID_F_GETFL:
            return fd<NX_FD_TRACK_MAX?g_fcntl_flflags[fd]:0;
        case ANDROID_F_SETFD:
        case ANDROID_F_SETFL:
        case ANDROID_F_DUPFD:
        case ANDROID_F_DUPFD_CLOEXEC:
            break;
        case ANDROID_F_GETLK:
        case ANDROID_F_SETLK:
        case ANDROID_F_SETLKW:

            return 0;
        default:
            errno=EINVAL;
            return -1;
    }
    if(cmd==ANDROID_F_SETFD){if(fd<NX_FD_TRACK_MAX)g_fcntl_fdflags[fd]=arg;return 0;}
    if(cmd==ANDROID_F_SETFL){if(fd<NX_FD_TRACK_MAX)g_fcntl_flflags[fd]=arg;return 0;}
    if(cmd==ANDROID_F_DUPFD||cmd==ANDROID_F_DUPFD_CLOEXEC){
        int n=dup(fd);
        if(n>=0){
            file_note_fd_duplicated(fd,n);
            if(n<NX_FD_TRACK_MAX){
                g_fcntl_fdflags[n]=(cmd==ANDROID_F_DUPFD_CLOEXEC)?1:(fd<NX_FD_TRACK_MAX?g_fcntl_fdflags[fd]:0);
                g_fcntl_flflags[n]=fd<NX_FD_TRACK_MAX?g_fcntl_flflags[fd]:0;
            }
        }
        (void)arg;
        return n;
    }
    errno=EINVAL;
    return -1;
}

static short nx_fake_poll_revents(int fd,short events){
    if(!fakefd_is_open(fd))return POLLNVAL;
    int r=fakefd_ready(fd);short out=0;
    if((r&ASNX_FD_READY_READ)&&(events&(POLLIN|POLLPRI)))out|=(short)(events&(POLLIN|POLLPRI));
    if((r&ASNX_FD_READY_WRITE)&&(events&POLLOUT))out|=POLLOUT;
    if(r&ASNX_FD_READY_HUP)out|=POLLHUP;
    if(r&ASNX_FD_READY_ERR)out|=POLLERR;
    return out;
}
static int nx_poll(struct pollfd *fds,nfds_t nfds,int timeout){
    if(!fds&&nfds){errno=EFAULT;return-1;}
    int has_fake=0;
    for(nfds_t i=0;i<nfds;i++)if(fakefd_is_range(fds[i].fd)){has_fake=1;break;}
    if(!has_fake)return poll(fds,nfds,timeout);
    int remaining=timeout;
    for(;;){
        int ready=0;
        for(nfds_t i=0;i<nfds;i++){
            short rev=0;
            if(fds[i].fd<0)rev=0;
            else if(fakefd_is_range(fds[i].fd))rev=nx_fake_poll_revents(fds[i].fd,fds[i].events);
            else {struct pollfd one=fds[i];one.revents=0;int pr=poll(&one,1,0);if(pr<0)return-1;if(pr>0)rev=one.revents;}
            fds[i].revents=rev;
            if(rev)ready++;
        }
        if(ready||timeout==0)return ready;
        if(timeout>0&&remaining<=0)return 0;
        int sleep_ms=(timeout<0||remaining>1)?1:remaining;
        if(sleep_ms<=0)return 0;
        svcSleepThread((uint64_t)sleep_ms*1000000ULL);
        if(timeout>0)remaining-=sleep_ms;
    }
}
static void android_fd_copy_words(unsigned long dst[ANDROID_FDSET_WORDS],const void *src){
    if(src)memcpy(dst,src,sizeof(unsigned long)*ANDROID_FDSET_WORDS);else memset(dst,0,sizeof(unsigned long)*ANDROID_FDSET_WORDS);
}
static int words_test(const unsigned long w[ANDROID_FDSET_WORDS],int fd){
    if(fd<0||fd>=ANDROID_FD_SETSIZE)return 0;
    return(int)((w[fd/ANDROID_FDSET_WORD_BITS]>>(fd%ANDROID_FDSET_WORD_BITS))&1ul);
}
static void words_set(unsigned long w[ANDROID_FDSET_WORDS],int fd){
    if(fd>=0&&fd<ANDROID_FD_SETSIZE)w[fd/ANDROID_FDSET_WORD_BITS]|=1ul<<(fd%ANDROID_FDSET_WORD_BITS);
}
static int nx_select(int n,fd_set *readset,fd_set *writeset,fd_set *exceptset,struct timeval *timeout){
    if(n<0){errno=EINVAL;return-1;}
    if(n>ANDROID_FD_SETSIZE)n=ANDROID_FD_SETSIZE;
    unsigned long in_r[ANDROID_FDSET_WORDS],in_w[ANDROID_FDSET_WORDS],in_e[ANDROID_FDSET_WORDS];
    android_fd_copy_words(in_r,readset);android_fd_copy_words(in_w,writeset);android_fd_copy_words(in_e,exceptset);
    int has_fake=0;
    for(int fd=ASNX_FAKE_FD_BASE;fd<n&&fd<ASNX_FAKE_FD_BASE+ASNX_FAKE_FD_COUNT;fd++)
        if((words_test(in_r,fd)||words_test(in_w,fd)||words_test(in_e,fd))&&fakefd_is_range(fd)){has_fake=1;break;}
    if(!has_fake)return select(n,readset,writeset,exceptset,timeout);
    long long remaining_us=-1;
    if(timeout){
        if(timeout->tv_sec<0||timeout->tv_usec<0){errno=EINVAL;return-1;}
        remaining_us=(long long)timeout->tv_sec*1000000LL+(long long)timeout->tv_usec;
    }
    for(;;){
        fd_set hr,hw,he;FD_ZERO(&hr);FD_ZERO(&hw);FD_ZERO(&he);int maxreal=-1;
        for(int fd=0;fd<n;fd++){
            if(fakefd_is_range(fd))continue;
            int any=0;
            if(words_test(in_r,fd)){FD_SET(fd,&hr);any=1;}
            if(words_test(in_w,fd)){FD_SET(fd,&hw);any=1;}
            if(words_test(in_e,fd)){FD_SET(fd,&he);any=1;}
            if(any)maxreal=fd;
        }
        struct timeval zero={0,0};
        int host_ready=select(maxreal+1,readset?&hr:NULL,writeset?&hw:NULL,exceptset?&he:NULL,&zero);
        if(host_ready<0)return-1;
        unsigned long out_r[ANDROID_FDSET_WORDS]={0},out_w[ANDROID_FDSET_WORDS]={0},out_e[ANDROID_FDSET_WORDS]={0};
        int ready=0;
        for(int fd=0;fd<n;fd++){
            int this_ready=0;
            if(fakefd_is_range(fd)){
                int state=fakefd_ready(fd);
                if(words_test(in_r,fd)&&(state&(ASNX_FD_READY_READ|ASNX_FD_READY_HUP|ASNX_FD_READY_ERR))){words_set(out_r,fd);this_ready=1;}
                if(words_test(in_w,fd)&&(state&(ASNX_FD_READY_WRITE|ASNX_FD_READY_ERR))){words_set(out_w,fd);this_ready=1;}
                if(words_test(in_e,fd)&&(state&ASNX_FD_READY_ERR)){words_set(out_e,fd);this_ready=1;}
            }else{
                if(readset&&FD_ISSET(fd,&hr)){words_set(out_r,fd);this_ready=1;}
                if(writeset&&FD_ISSET(fd,&hw)){words_set(out_w,fd);this_ready=1;}
                if(exceptset&&FD_ISSET(fd,&he)){words_set(out_e,fd);this_ready=1;}
            }
            if(this_ready)ready++;
        }
        if(ready||remaining_us==0){
            if(readset)memcpy(readset,out_r,sizeof(out_r));
            if(writeset)memcpy(writeset,out_w,sizeof(out_w));
            if(exceptset)memcpy(exceptset,out_e,sizeof(out_e));
            if(timeout&&remaining_us>=0){timeout->tv_sec=(time_t)(remaining_us/1000000LL);timeout->tv_usec=(suseconds_t)(remaining_us%1000000LL);}
            return ready;
        }
        long long sleep_us=(remaining_us<0||remaining_us>1000)?1000:remaining_us;
        if(sleep_us<=0)continue;
        svcSleepThread((uint64_t)sleep_us*1000ULL);
        if(remaining_us>0)remaining_us-=sleep_us;
    }
}

#define NX_FNM_PATHNAME 0x01
#define NX_FNM_NOESCAPE 0x02
#define NX_FNM_PERIOD 0x04
#define NX_FNM_LEADING_DIR 0x08
#define NX_FNM_CASEFOLD 0x10
#define NX_FNM_NOMATCH 1
static int fnm_eq(unsigned char a,unsigned char b,int flags){
    if(flags&NX_FNM_CASEFOLD){a=(unsigned char)tolower(a);b=(unsigned char)tolower(b);}
    return a==b;
}
static int fnm_leading_period(const char *s,const char *start,int flags){
    if(!(flags&NX_FNM_PERIOD)||*s!='.')return 0;
    return s==start||((flags&NX_FNM_PATHNAME)&&s>start&&s[-1]=='/');
}
static int fnm_class(const char **pp,unsigned char c,int flags){
    const char *p=*pp;int neg=0,hit=0;
    if(*p=='!'||*p=='^'){neg=1;p++;}
    if(*p==']'){if(fnm_eq(']',c,flags))hit=1;p++;}
    while(*p&&*p!=']'){
        unsigned char a=(unsigned char)*p++;
        if(a=='\\'&&!(flags&NX_FNM_NOESCAPE)&&*p)a=(unsigned char)*p++;
        if(*p=='-'&&p[1]&&p[1]!=']'){
            p++;unsigned char b=(unsigned char)*p++;
            if(b=='\\'&&!(flags&NX_FNM_NOESCAPE)&&*p)b=(unsigned char)*p++;
            unsigned char cc=c,aa=a,bb=b;
            if(flags&NX_FNM_CASEFOLD){cc=(unsigned char)tolower(cc);aa=(unsigned char)tolower(aa);bb=(unsigned char)tolower(bb);}
            if(aa<=cc&&cc<=bb)hit=1;
        }else if(fnm_eq(a,c,flags))hit=1;
    }
    if(*p!=']')return -1;
    *pp=p+1;
    return neg?!hit:hit;
}
static int nx_fnmatch_impl(const char *pat,const char *s,const char *start,int flags){
    while(*pat){
        unsigned char pc=(unsigned char)*pat++;
        if(pc=='*'){
            while(*pat=='*')pat++;
            if(fnm_leading_period(s,start,flags))return NX_FNM_NOMATCH;
            if(!*pat){
                if(!(flags&NX_FNM_PATHNAME))return 0;
                if(!strchr(s,'/'))return 0;
                return (flags&NX_FNM_LEADING_DIR)?0:NX_FNM_NOMATCH;
            }
            for(;;){
                if(nx_fnmatch_impl(pat,s,start,flags)==0)return 0;
                if(!*s||((flags&NX_FNM_PATHNAME)&&*s=='/'))break;
                s++;
            }
            return NX_FNM_NOMATCH;
        }
        if(!*s)return NX_FNM_NOMATCH;
        if(pc=='?'){
            if(((flags&NX_FNM_PATHNAME)&&*s=='/')||fnm_leading_period(s,start,flags))return NX_FNM_NOMATCH;
            s++;continue;
        }
        if(pc=='['){
            if(((flags&NX_FNM_PATHNAME)&&*s=='/')||fnm_leading_period(s,start,flags))return NX_FNM_NOMATCH;
            const char *save=pat;int r=fnm_class(&pat,(unsigned char)*s,flags);
            if(r<0){pat=save;pc='[';}else{if(!r)return NX_FNM_NOMATCH;s++;continue;}
        }
        if(pc=='\\'&&!(flags&NX_FNM_NOESCAPE)&&*pat)pc=(unsigned char)*pat++;
        if((flags&NX_FNM_PATHNAME)&&pc=='/'&&*s!='/')return NX_FNM_NOMATCH;
        if(!fnm_eq(pc,(unsigned char)*s,flags))return NX_FNM_NOMATCH;
        s++;
    }
    if(!*s)return 0;
    if((flags&NX_FNM_LEADING_DIR)&&*s=='/')return 0;
    return NX_FNM_NOMATCH;
}
static int nx_fnmatch(const char *pattern,const char *string,int flags){
    if(!pattern||!string){errno=EINVAL;return NX_FNM_NOMATCH;}
    return nx_fnmatch_impl(pattern,string,string,flags);
}
static int nx_ioctl(int fd,unsigned long req,...){(void)fd;(void)req;errno=ENOTTY;return-1;}

typedef struct {
    char *pw_name;
    char *pw_passwd;
    uint32_t pw_uid;
    uint32_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
} AndroidPasswd;
_Static_assert(sizeof(AndroidPasswd)==48,"Android LP64 struct passwd size");
_Static_assert(offsetof(AndroidPasswd,pw_dir)==0x20,"Android LP64 passwd pw_dir offset");

static char g_pw_name[]="u0_a0";
static char g_pw_passwd[]="x";
static char g_pw_gecos[]="All Stars";
static char g_pw_dir[]=GAME_HOME;
static char g_pw_shell[]="/system/bin/sh";
static AndroidPasswd g_passwd={
    g_pw_name,g_pw_passwd,10000u,10000u,g_pw_gecos,g_pw_dir,g_pw_shell
};

static void *nx_getpwuid(unsigned uid){
    g_passwd.pw_uid=uid;
    g_passwd.pw_gid=uid;
    return &g_passwd;
}

static int passwd_copy_string(char **out,char **cursor,size_t *left,const char *src){
    size_t need=strlen(src)+1;
    if(need>*left)return ERANGE;
    memcpy(*cursor,src,need);
    *out=*cursor;
    *cursor+=need;
    *left-=need;
    return 0;
}

static int nx_getpwuid_r(unsigned uid,void *pwd,char *buf,size_t n,void **result){
    if(result)*result=NULL;
    if(!pwd||!buf||!result)return EINVAL;
    AndroidPasswd *p=(AndroidPasswd*)pwd;
    char *cursor=buf;
    size_t left=n;
    memset(p,0,sizeof(*p));
    int r=passwd_copy_string(&p->pw_name,&cursor,&left,g_pw_name);
    if(!r)r=passwd_copy_string(&p->pw_passwd,&cursor,&left,g_pw_passwd);
    if(!r)r=passwd_copy_string(&p->pw_gecos,&cursor,&left,g_pw_gecos);
    if(!r)r=passwd_copy_string(&p->pw_dir,&cursor,&left,g_pw_dir);
    if(!r)r=passwd_copy_string(&p->pw_shell,&cursor,&left,g_pw_shell);
    if(r)return r;
    p->pw_uid=uid;
    p->pw_gid=uid;
    *result=p;
    return 0;
}
static int nx_utime(const char*p,const void*t){(void)p;(void)t;return 0;}
static int nx_utimes(const char*p,const void*t){(void)p;(void)t;return 0;}
static int nx_futimens(int fd,const struct timespec*t){(void)fd;(void)t;return 0;}
static void nx_openlog(const char*ident,int opt,int facility){(void)ident;(void)opt;(void)facility;}
static void nx_closelog(void){}
static void nx_syslog(int pri,const char*fmt,...){(void)pri;(void)fmt;}
static unsigned long long g_lrand48=0x1234abcd330eULL;
static void nx_srand48(long seed){g_lrand48=(((unsigned long long)(unsigned long)seed)<<16)|0x330eULL;}
static long nx_lrand48(void){g_lrand48=(0x5deece66dULL*g_lrand48+0xbULL)&((1ULL<<48)-1);return(long)(g_lrand48>>17);}
static int nx_gethostname(char*n,size_t z){if(!n||!z){errno=EINVAL;return-1;}nx_strlcpy(n,"switch",z);return 0;}

static unsigned nx_if_nametoindex(const char *name){
    if(!name||!*name){errno=ENXIO;return 0;}
    if(!strcmp(name,"lo")||!strcmp(name,"lo0"))return 1;
    if(!strcmp(name,"wlan0")||!strcmp(name,"wlan1")||!strcmp(name,"eth0")||!strcmp(name,"en0"))return 2;
    errno=ENXIO;
    return 0;
}
typedef struct { char sysname[65],nodename[65],release[65],version[65],machine[65],domainname[65]; } AndroidUtsname;
static int nx_uname(void*p){if(!p){errno=EFAULT;return-1;}AndroidUtsname*u=p;memset(u,0,sizeof(*u));nx_strlcpy(u->sysname,"Linux",sizeof(u->sysname));nx_strlcpy(u->nodename,"switch",sizeof(u->nodename));nx_strlcpy(u->release,"5.10.0",sizeof(u->release));nx_strlcpy(u->version,"Android-compatible Switch wrapper",sizeof(u->version));nx_strlcpy(u->machine,"aarch64",sizeof(u->machine));nx_strlcpy(u->domainname,"localdomain",sizeof(u->domainname));return 0;}
static ssize_t nx_sendfile(int outfd,int infd,int64_t*off,size_t count){unsigned char b[8192];size_t done=0;while(done<count){size_t want=count-done;if(want>sizeof(b))want=sizeof(b);ssize_t n=off?nx_pread(infd,b,want,(off_t)*off):read(infd,b,want);if(n<0)return done?(ssize_t)done:-1;if(n==0)break;size_t pos=0;while(pos<(size_t)n){ssize_t w=write(outfd,b+pos,(size_t)n-pos);if(w<0)return done?(ssize_t)done:-1;if(!w)return(ssize_t)done;pos+=(size_t)w;}done+=(size_t)n;if(off)*off+=n;if((size_t)n<want)break;}return(ssize_t)done;}

#define GUEST_SIGSET_BYTES 8
static int nx_sigemptyset(void*s){if(!s){errno=EINVAL;return-1;}*(uint64_t*)s=0;return 0;}
static int nx_sigfillset(void*s){if(!s){errno=EINVAL;return-1;}*(uint64_t*)s=UINT64_MAX;return 0;}
static int nx_sigaddset(void*s,int sig){if(!s||sig<=0||sig>64){errno=EINVAL;return-1;}*(uint64_t*)s|=1ull<<(sig-1);return 0;}
static int nx_sigdelset(void*s,int sig){if(!s||sig<=0||sig>64){errno=EINVAL;return-1;}*(uint64_t*)s&=~(1ull<<(sig-1));return 0;}
static int nx_sigsuspend(const void*s){(void)s;errno=EINTR;return-1;}

static int nx_utf8_decode(const unsigned char *p,size_t n,uint32_t *cp,size_t *used){
    if (!n) return 0;
    unsigned c = p[0];
    if(c<0x80){*cp=c;*used=1;return 1;}
    if((c&0xe0)==0xc0&&n>=2&&(p[1]&0xc0)==0x80){uint32_t v=((c&0x1f)<<6)|(p[1]&0x3f);if(v<0x80)return-1;*cp=v;*used=2;return 1;}
    if((c&0xf0)==0xe0&&n>=3&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80){uint32_t v=((c&0x0f)<<12)|((p[1]&0x3f)<<6)|(p[2]&0x3f);if(v<0x800||(v>=0xd800&&v<=0xdfff))return-1;*cp=v;*used=3;return 1;}
    if((c&0xf8)==0xf0&&n>=4&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80&&(p[3]&0xc0)==0x80){uint32_t v=((c&7)<<18)|((p[1]&0x3f)<<12)|((p[2]&0x3f)<<6)|(p[3]&0x3f);if(v<0x10000||v>0x10ffff)return-1;*cp=v;*used=4;return 1;}
    return -1;
}
static size_t nx_mbsnrtowcs(wchar_t *dst,const char **src,size_t nms,size_t len,mbstate_t *ps){
    (void)ps;if(!src||!*src){errno=EINVAL;return(size_t)-1;}const unsigned char*p=(const unsigned char*)*src;size_t left=nms,out=0;
    while(left){uint32_t cp=0;size_t used=0;int r=nx_utf8_decode(p,left,&cp,&used);if(r<=0){errno=EILSEQ;return(size_t)-1;}if(cp==0){if(dst&&out<len)dst[out]=0;if(dst)*src=NULL;return out;}if(dst&&out>=len){*src=(const char*)p;return out;}if(dst)dst[out]=(wchar_t)cp;out++;p+=used;left-=used;}
    if (dst) *src = (const char*)p;
    return out;
}
static size_t nx_utf8_encode(uint32_t cp,unsigned char out[4]){if(cp<0x80){out[0]=(unsigned char)cp;return 1;}if(cp<0x800){out[0]=0xc0|(cp>>6);out[1]=0x80|(cp&0x3f);return 2;}if(cp>=0xd800&&cp<=0xdfff)return 0;if(cp<0x10000){out[0]=0xe0|(cp>>12);out[1]=0x80|((cp>>6)&0x3f);out[2]=0x80|(cp&0x3f);return 3;}if(cp<=0x10ffff){out[0]=0xf0|(cp>>18);out[1]=0x80|((cp>>12)&0x3f);out[2]=0x80|((cp>>6)&0x3f);out[3]=0x80|(cp&0x3f);return 4;}return 0;}
static size_t nx_wcsnrtombs(char *dst,const wchar_t **src,size_t nwc,size_t len,mbstate_t *ps){
    (void)ps;if(!src||!*src){errno=EINVAL;return(size_t)-1;}const wchar_t*p=*src;size_t out=0;
    for(size_t i=0;i<nwc;i++,p++){uint32_t cp=(uint32_t)*p;if(cp==0){if(dst&&out<len)dst[out]=0;if(dst)*src=NULL;return out;}unsigned char enc[4];size_t n=nx_utf8_encode(cp,enc);if(!n){errno=EILSEQ;return(size_t)-1;}if(dst&&out+n>len){*src=p;return out;}if(dst)memcpy(dst+out,enc,n);out+=n;}
    if (dst) *src = p;
    return out;
}
static void *nx_newlocale(int mask,const char*name,void*base){(void)mask;(void)name;return base?base:(void*)1;}
static void *nx_uselocale(void*loc){(void)loc;return(void*)1;}
static void nx_freelocale(void*loc){(void)loc;}
static size_t nx_strftime_l(char*s,size_t n,const char*f,const struct tm*t,void*l){(void)l;return strftime(s,n,f,t);}
static long long nx_strtoll_l(const char*s,char**e,int b,void*l){(void)l;return strtoll(s,e,b);}
static unsigned long long nx_strtoull_l(const char*s,char**e,int b,void*l){(void)l;return strtoull(s,e,b);}
static long double nx_strtold_l(const char*s,char**e,void*l){(void)l;return strtold(s,e);}
static int nx_strcoll_l(const char*a,const char*b,void*l){(void)l;return strcoll(a,b);}
static size_t nx_strxfrm_l(char*d,const char*s,size_t n,void*l){(void)l;return strxfrm(d,s,n);}
static int nx_wcscoll_l(const wchar_t*a,const wchar_t*b,void*l){(void)l;return wcscoll(a,b);}
static size_t nx_wcsxfrm_l(wchar_t*d,const wchar_t*s,size_t n,void*l){(void)l;return wcsxfrm(d,s,n);}
static int nx_iswlower_l(wint_t c,void*l){(void)l;return iswlower(c);}
static int nx_iswspace_l(wint_t c,void*l){(void)l;return iswspace(c);}
static int nx_iswprint_l(wint_t c,void*l){(void)l;return iswprint(c);}
static int nx_iswcntrl_l(wint_t c,void*l){(void)l;return iswcntrl(c);}
static int nx_iswupper_l(wint_t c,void*l){(void)l;return iswupper(c);}
static int nx_iswalpha_l(wint_t c,void*l){(void)l;return iswalpha(c);}
static int nx_iswdigit_l(wint_t c,void*l){(void)l;return iswdigit(c);}
static int nx_iswpunct_l(wint_t c,void*l){(void)l;return iswpunct(c);}
static int nx_iswxdigit_l(wint_t c,void*l){(void)l;return iswxdigit(c);}
static int nx_iswblank_l(wint_t c,void*l){(void)l;return iswblank(c);}
static wint_t nx_towupper_l(wint_t c,void*l){(void)l;return towupper(c);}
static wint_t nx_towlower_l(wint_t c,void*l){(void)l;return towlower(c);}

static int nx_pthread_attr8_init(void *a) {
    if (a) *(uint64_t *)a = 0;
    return 0;
}
static int nx_attr_noop(void *a) { (void)a; return 0; }
static int nx_attr_settype(void *a, int t) {

    if (a) *(uint64_t *)a = (uint64_t)(unsigned)t;
    return 0;
}
static int nx_condattr_setclock(void *a, int c) {
    if (!a || (c != 0 && c != 1)) return EINVAL;
    uint64_t attr = *(uint64_t *)a;
    attr = (attr & ~2ull) | ((uint64_t)(unsigned)c << 1);
    *(uint64_t *)a = attr;
    return 0;
}

static void *HANDLE_IMPORTS=(void*)0x494d5054u;
static int nx_system_library_name(const char *name){
    return name && (strstr(name,"libEGL")||strstr(name,"libGLES")||strstr(name,"libandroid")||
        strstr(name,"libmediandk")||strstr(name,"libaaudio")||strstr(name,"libOpenSLES")||
        strstr(name,"liblog")||strstr(name,"libc.so")||strstr(name,"libm.so")||strstr(name,"libdl.so"));
}
static void *nx_dlopen(const char*name,int flags){
    (void)flags; g_dlerr=NULL;
    if(!name)return HANDLE_IMPORTS;
    so_module*m=so_find_named(name);
    if(m)return m;
    if(nx_system_library_name(name))return HANDLE_IMPORTS;
    g_dlerr="module not present";
    return NULL;
}
static int nx_dlclose(void*h){if(!h){g_dlerr="invalid handle";return-1;}g_dlerr=NULL;return 0;}
static const char*nx_dlerror(void){const char*e=g_dlerr;g_dlerr=NULL;return e;}

#if ENABLE_GFX_DIAGNOSTICS
/* Graphics/memory tracing and bounded OOM recovery.
 *
 * Normal GL errors remain host-visible. For the two texture allocation/upload
 * paths proven to fail under Mesa pressure, an OOM is consumed only when a
 * synchronous reclaim is followed by one immediate retry. If that retry also
 * fails, its error is left pending for Unity exactly as the host produced it.
 */
typedef struct {
    uint64_t seq;
    const char *op;
} GfxDiagLastOp;

static __thread GfxDiagLastOp g_gfxdiag_last;
static uint64_t g_gfx_seq;

typedef GLenum (*NxGlGetErrorFn)(void);
typedef const GLubyte *(*NxGlGetStringFn)(GLenum);
typedef const GLubyte *(*NxGlGetStringiFn)(GLenum,GLuint);
typedef void (*NxGlGetIntegervFn)(GLenum,GLint*);
typedef void (*NxGlCompressedTexImage2DFn)(GLenum,GLint,GLenum,GLsizei,GLsizei,GLint,GLsizei,const void*);
typedef void (*NxGlCompressedTexImage3DFn)(GLenum,GLint,GLenum,GLsizei,GLsizei,GLsizei,GLint,GLsizei,const void*);
typedef void (*NxGlCompressedTexSubImage2DFn)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLsizei,const void*);
typedef void (*NxGlTexImage2DFn)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
typedef void (*NxGlTexImage3DFn)(GLenum,GLint,GLint,GLsizei,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
typedef void (*NxGlTexSubImage2DFn)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*);
typedef void (*NxGlTexStorage2DFn)(GLenum,GLsizei,GLenum,GLsizei,GLsizei);
typedef void (*NxGlTexStorage3DFn)(GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLsizei);
typedef void (*NxGlTexStorage2DMultisampleFn)(GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLboolean);
typedef void (*NxGlTexStorage3DMultisampleFn)(GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLsizei,GLboolean);
typedef void (*NxGlRenderbufferStorageFn)(GLenum,GLenum,GLsizei,GLsizei);
typedef void (*NxGlRenderbufferStorageMultisampleFn)(GLenum,GLsizei,GLenum,GLsizei,GLsizei);
typedef void (*NxGlBufferDataFn)(GLenum,ptrdiff_t,const void*,GLenum);
typedef void (*NxGlGenerateMipmapFn)(GLenum);
typedef GLuint (*NxGlCreateShaderFn)(GLenum);
typedef GLuint (*NxGlCreateProgramFn)(void);
typedef void (*NxGlCompileShaderFn)(GLuint);
typedef void (*NxGlLinkProgramFn)(GLuint);
typedef void (*NxGlDeleteTexturesFn)(GLsizei,const GLuint*);
typedef void (*NxGlGetShaderivFn)(GLuint,GLenum,GLint*);
typedef void (*NxGlGetProgramivFn)(GLuint,GLenum,GLint*);

#ifndef GL_DEBUG_OUTPUT
#define GL_DEBUG_OUTPUT 0x92E0
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_EXTENSIONS
#define GL_EXTENSIONS 0x1F03
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#endif
#ifndef GL_DEBUG_TYPE_ERROR
#define GL_DEBUG_TYPE_ERROR 0x824C
#endif
#ifndef GL_DEBUG_SEVERITY_HIGH
#define GL_DEBUG_SEVERITY_HIGH 0x9146
#endif
typedef void (*NxGlDebugProc)(GLenum,GLenum,GLuint,GLenum,GLsizei,const GLchar*,const void*);
typedef void (*NxGlDebugMessageCallbackFn)(NxGlDebugProc,const void*);
typedef void (*NxGlFinishFn)(void);
typedef void (*NxGlFlushFn)(void);
typedef void *NxGlSync;
typedef NxGlSync (*NxGlFenceSyncFn)(GLenum,GLbitfield);
typedef GLenum (*NxGlClientWaitSyncFn)(NxGlSync,GLbitfield,uint64_t);
typedef void (*NxGlDeleteSyncFn)(NxGlSync);
typedef void (*NxGlReleaseShaderCompilerFn)(void);
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif

static NxGlGetErrorFn g_real_glGetError;
static NxGlGetStringFn g_real_glGetString;
static NxGlGetStringiFn g_real_glGetStringi;
static NxGlGetIntegervFn g_real_glGetIntegerv;
static NxGlCompressedTexImage2DFn g_real_glCompressedTexImage2D;
static NxGlCompressedTexImage3DFn g_real_glCompressedTexImage3D;
static NxGlCompressedTexSubImage2DFn g_real_glCompressedTexSubImage2D;
static NxGlTexImage2DFn g_real_glTexImage2D;
static NxGlTexImage3DFn g_real_glTexImage3D;
static NxGlTexSubImage2DFn g_real_glTexSubImage2D;
static NxGlTexStorage2DFn g_real_glTexStorage2D;
static NxGlTexStorage3DFn g_real_glTexStorage3D;
static NxGlTexStorage2DMultisampleFn g_real_glTexStorage2DMultisample;
static NxGlTexStorage3DMultisampleFn g_real_glTexStorage3DMultisample;
static NxGlRenderbufferStorageFn g_real_glRenderbufferStorage;
static NxGlRenderbufferStorageMultisampleFn g_real_glRenderbufferStorageMultisample;
static NxGlBufferDataFn g_real_glBufferData;
static NxGlGenerateMipmapFn g_real_glGenerateMipmap;
static NxGlCreateShaderFn g_real_glCreateShader;
static NxGlCreateProgramFn g_real_glCreateProgram;
static NxGlCompileShaderFn g_real_glCompileShader;
static NxGlLinkProgramFn g_real_glLinkProgram;
static NxGlDeleteTexturesFn g_real_glDeleteTextures;
static NxGlGetShaderivFn g_real_glGetShaderiv;
static NxGlGetProgramivFn g_real_glGetProgramiv;
static NxGlDebugMessageCallbackFn g_real_glDebugMessageCallback;
static NxGlFinishFn g_real_glFinish;
static NxGlFlushFn g_real_glFlush;
static NxGlFenceSyncFn g_real_glFenceSync;
static NxGlClientWaitSyncFn g_real_glClientWaitSync;
static NxGlDeleteSyncFn g_real_glDeleteSync;
static NxGlReleaseShaderCompilerFn g_real_glReleaseShaderCompiler;
static unsigned g_gfxdebug_install_attempted;
static uint64_t g_gfxdebug_oom_seq;
static unsigned g_gfx_reclaim_deleted_pending;
static unsigned g_gfx_reclaim_fenced_deletes;
static NxGlSync g_gfx_reclaim_fence;
static Mutex g_etc2_bc_lock;
static void *g_etc2_bc_scratch;
static size_t g_etc2_bc_scratch_size;
static int g_etc2_bc_available=-1;
static uint64_t g_etc2_bc_count;
static uint64_t g_etc2_bc_bytes;

static void *nx_raw_gles_proc(const char *name){
    return name ? (void*)eglGetProcAddress(name) : NULL;
}

static GLenum gfxdiag_host_error(void){
    if(!g_real_glGetError) g_real_glGetError=(NxGlGetErrorFn)nx_raw_gles_proc("glGetError");
    if(!g_real_glGetError) g_real_glGetError=(NxGlGetErrorFn)&glGetError;
    return g_real_glGetError ? g_real_glGetError() : GL_NO_ERROR;
}

static uint64_t gfxdiag_next_seq(void){
    return __atomic_add_fetch(&g_gfx_seq,1u,__ATOMIC_RELAXED);
}
static void nx_gl_debug_callback(GLenum source,GLenum type,GLuint id,GLenum severity,GLsizei length,
        const GLchar *message,const void *user){
    (void)length; (void)user;
    if(type!=GL_DEBUG_TYPE_ERROR && severity!=GL_DEBUG_SEVERITY_HIGH) return;
    if(message && (strstr(message,"OUT_OF_MEMORY") || strcasestr(message,"out of memory")))
        __atomic_store_n(&g_gfxdebug_oom_seq,g_gfxdiag_last.seq,__ATOMIC_RELAXED);
    trace_log_printf("GLDBG","seq=%llu op=%s src=0x%x type=0x%x id=%u severity=0x%x msg=%.420s",
        (unsigned long long)g_gfxdiag_last.seq,g_gfxdiag_last.op?g_gfxdiag_last.op:"?",
        source,type,id,severity,message?message:"?");
}

static void gfxdiag_install_debug_callback(void){
    if(g_gfxdebug_install_attempted) return;
    g_gfxdebug_install_attempted=1;
    g_real_glDebugMessageCallback=(NxGlDebugMessageCallbackFn)nx_raw_gles_proc("glDebugMessageCallback");
    if(!g_real_glDebugMessageCallback)
        g_real_glDebugMessageCallback=(NxGlDebugMessageCallbackFn)nx_raw_gles_proc("glDebugMessageCallbackKHR");
    if(!g_real_glDebugMessageCallback){trace_log_printf("GL","debug callback unavailable");return;}
    g_real_glDebugMessageCallback(nx_gl_debug_callback,NULL);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    trace_log_printf("GL","debug callback installed");
}

static void gfxdiag_async_resolve(void){
    if(!g_real_glFlush)g_real_glFlush=(NxGlFlushFn)nx_raw_gles_proc("glFlush");
    if(!g_real_glFlush)g_real_glFlush=(NxGlFlushFn)&glFlush;
    if(!g_real_glFenceSync)g_real_glFenceSync=(NxGlFenceSyncFn)nx_raw_gles_proc("glFenceSync");
    if(!g_real_glClientWaitSync)g_real_glClientWaitSync=(NxGlClientWaitSyncFn)nx_raw_gles_proc("glClientWaitSync");
    if(!g_real_glDeleteSync)g_real_glDeleteSync=(NxGlDeleteSyncFn)nx_raw_gles_proc("glDeleteSync");
}

static void gfxdiag_async_fence_clear(void){
    if(g_gfx_reclaim_fence){
        gfxdiag_async_resolve();
        if(g_real_glDeleteSync)g_real_glDeleteSync(g_gfx_reclaim_fence);
        g_gfx_reclaim_fence=NULL;
    }
    g_gfx_reclaim_fenced_deletes=0;
}

static void gfxdiag_async_submit(const char *reason){
    if(g_gfx_reclaim_fence || g_gfx_reclaim_deleted_pending<GFX_RECLAIM_DELETE_THRESHOLD)return;
    gfxdiag_async_resolve();
    if(!g_real_glFenceSync || !g_real_glClientWaitSync || !g_real_glDeleteSync){
        if(g_real_glFlush)g_real_glFlush();
        return;
    }
    g_gfx_reclaim_fence=g_real_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
    if(!g_gfx_reclaim_fence)return;
    g_gfx_reclaim_fenced_deletes=g_gfx_reclaim_deleted_pending;
    if(g_real_glFlush)g_real_glFlush();
    trace_log_printf("GLMEM","reclaim fence submit reason=%s pending=%u",reason?reason:"?",g_gfx_reclaim_deleted_pending);
}

static int gfxdiag_async_poll(const char *reason){
    if(!g_gfx_reclaim_fence)return 1;
    gfxdiag_async_resolve();
    if(!g_real_glClientWaitSync)return 0;
    GLenum r=g_real_glClientWaitSync(g_gfx_reclaim_fence,0,0);
    if(r==GL_ALREADY_SIGNALED || r==GL_CONDITION_SATISFIED){
        unsigned retired=g_gfx_reclaim_fenced_deletes;
        gfxdiag_async_fence_clear();
        if(g_gfx_reclaim_deleted_pending>=retired)g_gfx_reclaim_deleted_pending-=retired;
        else g_gfx_reclaim_deleted_pending=0;
        trace_log_printf("GLMEM","reclaim fence retired reason=%s retired=%u pending=%u",reason?reason:"?",retired,g_gfx_reclaim_deleted_pending);
        return 1;
    }
    if(r==GL_WAIT_FAILED){
        (void)reason;
        gfxdiag_async_fence_clear();
    }
    return 0;
}

static void gfxdiag_finish_reclaim(const char *reason,int release_shader){
    if(!g_real_glFinish)g_real_glFinish=(NxGlFinishFn)nx_raw_gles_proc("glFinish");
    if(!g_real_glFinish)g_real_glFinish=(NxGlFinishFn)&glFinish;
    if(!g_real_glReleaseShaderCompiler)
        g_real_glReleaseShaderCompiler=(NxGlReleaseShaderCompilerFn)nx_raw_gles_proc("glReleaseShaderCompiler");
    if(!g_real_glReleaseShaderCompiler)g_real_glReleaseShaderCompiler=(NxGlReleaseShaderCompilerFn)&glReleaseShaderCompiler;
    trace_main_block_begin("gl:forced-reclaim-finish",NULL,reason);
    uint64_t finish_t0=trace_now_us();
    if(g_real_glFinish)g_real_glFinish();
    uint64_t finish_us=trace_now_us()-finish_t0;
    trace_main_block_end();
    if(finish_us>=TRACE_SLOW_IO_US)trace_log_printf("GLWAIT","forced reclaim glFinish reason=%s time=%lluus",reason?reason:"?",(unsigned long long)finish_us);
    if(release_shader && g_real_glReleaseShaderCompiler)g_real_glReleaseShaderCompiler();
    gfxdiag_async_fence_clear();
    g_gfx_reclaim_deleted_pending=0;
    trace_log_printf("GLMEM","forced reclaim reason=%s release_shader=%d",reason?reason:"?",release_shader);
}

static void nx_glFinish_profile(void){
    if(!g_real_glFinish)g_real_glFinish=(NxGlFinishFn)nx_raw_gles_proc("glFinish");
    if(!g_real_glFinish)g_real_glFinish=(NxGlFinishFn)&glFinish;
    trace_main_block_begin("gl:glFinish",NULL,NULL);
    uint64_t t0=trace_now_us();
    if(g_real_glFinish)g_real_glFinish();
    uint64_t us=trace_now_us()-t0;
    trace_main_block_end();
    if(us>=TRACE_SLOW_IO_US)trace_log_printf("GLWAIT","glFinish time=%lluus",(unsigned long long)us);
}

static GLenum nx_glClientWaitSync_profile(NxGlSync sync,GLbitfield flags,uint64_t timeout){
    if(!g_real_glClientWaitSync)g_real_glClientWaitSync=(NxGlClientWaitSyncFn)nx_raw_gles_proc("glClientWaitSync");
    char detail[96];
    snprintf(detail,sizeof(detail),"flags=0x%x timeout=%llu",(unsigned)flags,(unsigned long long)timeout);
    trace_main_block_begin("gl:clientWaitSync",sync,detail);
    uint64_t t0=trace_now_us();
    GLenum r=g_real_glClientWaitSync?g_real_glClientWaitSync(sync,flags,timeout):GL_WAIT_FAILED;
    uint64_t us=trace_now_us()-t0;
    trace_main_block_end();
    if(us>=TRACE_SLOW_IO_US)trace_log_printf("GLWAIT","clientWaitSync time=%lluus result=0x%x timeout=%llu",
        (unsigned long long)us,r,(unsigned long long)timeout);
    return r;
}

static unsigned gfxdiag_drain_host_errors(void){
    unsigned count=0;
    for(unsigned i=0;i<16u;i++){
        GLenum e=gfxdiag_host_error();
        if(e==GL_NO_ERROR)break;
        count++;
    }
    return count;
}

static int gfxdiag_etc2_bc_mode(GLenum format){
    if(format==0x9274u)return ETC2_BC_RGB8;
    if(format==0x9278u)return ETC2_BC_RGBA8;
    return 0;
}

static GLenum gfxdiag_etc2_bc_host_format(GLenum format){
    return format==0x9274u ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT :
           format==0x9278u ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : format;
}

static int gfxdiag_etc2_bc_supported(void){
#if ENABLE_ETC2_BC_TRANSCODE
    if(g_etc2_bc_available>=0)return g_etc2_bc_available;
    if(!g_real_glGetString)g_real_glGetString=(NxGlGetStringFn)nx_raw_gles_proc("glGetString");
    if(!g_real_glGetString)g_real_glGetString=(NxGlGetStringFn)&glGetString;
    if(!g_real_glGetStringi)g_real_glGetStringi=(NxGlGetStringiFn)nx_raw_gles_proc("glGetStringi");
    if(!g_real_glGetIntegerv)g_real_glGetIntegerv=(NxGlGetIntegervFn)nx_raw_gles_proc("glGetIntegerv");
    if(!g_real_glGetIntegerv)g_real_glGetIntegerv=(NxGlGetIntegervFn)&glGetIntegerv;
    int ok=0;
    const char *matched=NULL;
    if(g_real_glGetStringi && g_real_glGetIntegerv){
        GLint n=0;
        g_real_glGetIntegerv(GL_NUM_EXTENSIONS,&n);
        for(GLint i=0;i<n;i++){
            const char *e=(const char*)g_real_glGetStringi(GL_EXTENSIONS,(GLuint)i);
            if(e && (!strcmp(e,"GL_EXT_texture_compression_s3tc") ||
                     !strcmp(e,"GL_NV_texture_compression_s3tc"))){ok=1;matched=e;break;}
        }
    }
    if(!ok && g_real_glGetString){
        const char *ext=(const char*)g_real_glGetString(GL_EXTENSIONS);
        if(ext && strstr(ext,"GL_EXT_texture_compression_s3tc")){ok=1;matched="GL_EXT_texture_compression_s3tc";}
        else if(ext && strstr(ext,"GL_NV_texture_compression_s3tc")){ok=1;matched="GL_NV_texture_compression_s3tc";}
    }
    g_etc2_bc_available=ok?1:0;
    trace_log_printf("ETC2BC","startup enabled=%d s3tc=%d extension=%s",g_etc2_bc_available,ok,matched?matched:"none");
    return g_etc2_bc_available;
#else
    g_etc2_bc_available=0;
    return 0;
#endif
}

static int gfxdiag_etc2_bc_reserve(size_t need){
    if(need<=g_etc2_bc_scratch_size)return 1;
    size_t cap=(need+65535u)&~(size_t)65535u;
    void *p=realloc(g_etc2_bc_scratch,cap);
    if(!p){
        trace_log_printf("ETC2BC","scratch allocation failed need=%lluKB free=%lluKB",
            (unsigned long long)(cap>>10),(unsigned long long)(mem_diag_process_free_bytes()>>10));
        return 0;
    }
    g_etc2_bc_scratch=p;
    g_etc2_bc_scratch_size=cap;
    trace_log_printf("ETC2BC","scratch resized=%lluKB",(unsigned long long)(cap>>10));
    return 1;
}

/* Returns 1 with g_etc2_bc_lock held. Caller must unlock after the GL call. */
static int gfxdiag_etc2_bc_prepare(GLenum format,GLsizei width,GLsizei height,GLsizei imageSize,const void *data,
        GLenum *host_format,GLsizei *host_size,const void **host_data,uint64_t *cpu_us){
    int mode=gfxdiag_etc2_bc_mode(format);
    if(!mode||!gfxdiag_etc2_bc_supported()||width<=0||height<=0||imageSize<0)return 0;
    size_t need=etc2_bc_image_size(mode,width,height);
    if(!need||need>(size_t)INT_MAX||(size_t)imageSize<need){
        trace_log_printf("ETC2BC","skip malformed fmt=0x%x %dx%d imageSize=%d expected=%llu",
            format,width,height,imageSize,(unsigned long long)need);
        return 0;
    }
    mutexLock(&g_etc2_bc_lock);
    uint64_t t0=trace_now_us();
    if(data){
        if(!gfxdiag_etc2_bc_reserve(need) ||
           !etc2_bc_transcode(g_etc2_bc_scratch,g_etc2_bc_scratch_size,data,(size_t)imageSize,width,height,mode)){
            mutexUnlock(&g_etc2_bc_lock);
            trace_log_printf("ETC2BC","transcode failed fmt=0x%x %dx%d bytes=%d",format,width,height,imageSize);
            return 0;
        }
        *host_data=g_etc2_bc_scratch;
    }else *host_data=NULL;
    *host_format=gfxdiag_etc2_bc_host_format(format);
    *host_size=(GLsizei)need;
    uint64_t us=trace_now_us()-t0;
    if(cpu_us)*cpu_us=us;
    uint64_t count=__atomic_add_fetch(&g_etc2_bc_count,1u,__ATOMIC_RELAXED);
    uint64_t total=__atomic_add_fetch(&g_etc2_bc_bytes,(uint64_t)need,__ATOMIC_RELAXED);
    if(count<=8u || (count&255u)==0u || us>=10000ull)
        trace_log_printf("ETC2BC","count=%llu src=0x%x dst=0x%x %dx%d bytes=%llu cpu=%lluus total=%lluMB",
            (unsigned long long)count,format,*host_format,width,height,(unsigned long long)need,
            (unsigned long long)us,(unsigned long long)(total>>20));
    return 1;
}

static int gfxdiag_is_large_etc2_rgba(GLenum internalformat,GLsizei width,GLsizei height){
    if(internalformat!=0x9274u && internalformat!=0x9275u &&
       internalformat!=0x9278u && internalformat!=0x9279u)return 0;
    if(width<=0||height<=0)return 0;
    return (uint64_t)(uint32_t)width*(uint64_t)(uint32_t)height >= (uint64_t)GFX_RECLAIM_ETC2_MIN_PIXELS;
}

static int gfxdiag_is_trace_texture(GLsizei width,GLsizei height){
    if(width<=0||height<=0)return 0;
    return (uint64_t)(uint32_t)width*(uint64_t)(uint32_t)height >= (uint64_t)TRACE_LARGE_TEXTURE_PIXELS;
}


static void gfxdiag_note_simple(const char *op){
    g_gfxdiag_last.seq=gfxdiag_next_seq();
    g_gfxdiag_last.op=op;
}

static void gfxdiag_note_texstorage2d(GLenum target,GLsizei levels,GLenum internalformat,
        GLsizei width,GLsizei height){
    (void)target; (void)levels; (void)internalformat; (void)width; (void)height;
    gfxdiag_note_simple("glTexStorage2D");
}

#define GFXDIAG_RUN_RETRY(OP,CALL_STMT) do { gfxdiag_note_simple((OP)); CALL_STMT; } while(0)

static GLenum nx_glGetError(void){
    GLenum e=gfxdiag_host_error();
    if(e!=GL_NO_ERROR)trace_log_printf("GLERR","error=0x%x seq=%llu op=%s free=%lluKB",e,
        (unsigned long long)g_gfxdiag_last.seq,g_gfxdiag_last.op?g_gfxdiag_last.op:"?",
        (unsigned long long)(mem_diag_process_free_bytes()>>10));
    return e;
}

static const GLubyte *nx_glGetString(GLenum name){
    if(!g_real_glGetString) g_real_glGetString=(NxGlGetStringFn)nx_raw_gles_proc("glGetString");
    if(!g_real_glGetString) g_real_glGetString=(NxGlGetStringFn)&glGetString;
    const GLubyte *v=g_real_glGetString ? g_real_glGetString(name) : NULL;
    static unsigned logged;
    unsigned bit=name==GL_VENDOR?1u:name==GL_RENDERER?2u:name==GL_VERSION?4u:0u;
    if(bit&&!(logged&bit)){logged|=bit;trace_log_printf("GL","string 0x%x = %s",name,v?(const char*)v:"(null)");}
    if(name==GL_VERSION) gfxdiag_install_debug_callback();
    return v;
}

static void nx_glCompressedTexImage2D(GLenum target,GLint level,GLenum internalformat,
        GLsizei width,GLsizei height,GLint border,GLsizei imageSize,const void *data){
    if(!g_real_glCompressedTexImage2D)
        g_real_glCompressedTexImage2D=(NxGlCompressedTexImage2DFn)nx_raw_gles_proc("glCompressedTexImage2D");
    if(!g_real_glCompressedTexImage2D)return;

    GLenum host_format=internalformat;
    GLsizei host_size=imageSize;
    const void *host_data=data;
    uint64_t transcode_us=0;
    int bc=gfxdiag_etc2_bc_prepare(internalformat,width,height,imageSize,data,
        &host_format,&host_size,&host_data,&transcode_us);

    gfxdiag_note_simple("glCompressedTexImage2D");
    uint64_t t0=trace_now_us();
    g_real_glCompressedTexImage2D(target,level,host_format,width,height,border,host_size,host_data);
    uint64_t gl_us=trace_now_us()-t0;
    if(bc)mutexUnlock(&g_etc2_bc_lock);

    if(bc||gfxdiag_is_trace_texture(width,height)||gl_us>=4000ull)
        trace_log_printf("GLTEX","compressed2D seq=%llu level=%d fmt=0x%x host=0x%x %dx%d bytes=%d cpu=%lluus gl=%lluus bc=%d",
            (unsigned long long)g_gfxdiag_last.seq,level,internalformat,host_format,width,height,host_size,
            (unsigned long long)transcode_us,(unsigned long long)gl_us,bc);
}
static void nx_glCompressedTexImage3D(GLenum target,GLint level,GLenum internalformat,
        GLsizei width,GLsizei height,GLsizei depth,GLint border,GLsizei imageSize,const void *data){
    if(!g_real_glCompressedTexImage3D)
        g_real_glCompressedTexImage3D=(NxGlCompressedTexImage3DFn)nx_raw_gles_proc("glCompressedTexImage3D");
    if(!g_real_glCompressedTexImage3D)return;
    GFXDIAG_RUN_RETRY("glCompressedTexImage3D",
        g_real_glCompressedTexImage3D(target,level,internalformat,width,height,depth,border,imageSize,data));
}

static void nx_glTexImage2D(GLenum target,GLint level,GLint internalformat,
        GLsizei width,GLsizei height,GLint border,GLenum format,GLenum type,const void *data){
    if(!g_real_glTexImage2D) g_real_glTexImage2D=(NxGlTexImage2DFn)nx_raw_gles_proc("glTexImage2D");
    if(!g_real_glTexImage2D)return;
    gfxdiag_note_simple("glTexImage2D");
    uint64_t t0=trace_now_us();
    g_real_glTexImage2D(target,level,internalformat,width,height,border,format,type,data);
    uint64_t us=trace_now_us()-t0;
    if(gfxdiag_is_trace_texture(width,height)||us>=4000ull)
        trace_log_printf("GLTEX","texImage2D seq=%llu level=%d ifmt=0x%x fmt=0x%x type=0x%x %dx%d time=%lluus",
            (unsigned long long)g_gfxdiag_last.seq,level,internalformat,format,type,width,height,(unsigned long long)us);
}

static void nx_glTexImage3D(GLenum target,GLint level,GLint internalformat,
        GLsizei width,GLsizei height,GLsizei depth,GLint border,GLenum format,GLenum type,const void *data){
    if(!g_real_glTexImage3D) g_real_glTexImage3D=(NxGlTexImage3DFn)nx_raw_gles_proc("glTexImage3D");
    if(!g_real_glTexImage3D)return;
    GFXDIAG_RUN_RETRY("glTexImage3D",
        g_real_glTexImage3D(target,level,internalformat,width,height,depth,border,format,type,data));
}

static void nx_glTexSubImage2D(GLenum target,GLint level,GLint xoffset,GLint yoffset,
        GLsizei width,GLsizei height,GLenum format,GLenum type,const void *data){
    if(!g_real_glTexSubImage2D) g_real_glTexSubImage2D=(NxGlTexSubImage2DFn)nx_raw_gles_proc("glTexSubImage2D");
    if(!g_real_glTexSubImage2D)return;
    GFXDIAG_RUN_RETRY("glTexSubImage2D",
        g_real_glTexSubImage2D(target,level,xoffset,yoffset,width,height,format,type,data));
}

static void nx_glTexStorage2D(GLenum target,GLsizei levels,GLenum internalformat,GLsizei width,GLsizei height){
    if(!g_real_glTexStorage2D) g_real_glTexStorage2D=(NxGlTexStorage2DFn)nx_raw_gles_proc("glTexStorage2D");
    if(!g_real_glTexStorage2D)return;

    GLenum host_internalformat=internalformat;
    int bc=0;
    if(gfxdiag_etc2_bc_mode(internalformat) && gfxdiag_etc2_bc_supported()){
        host_internalformat=gfxdiag_etc2_bc_host_format(internalformat);
        bc=1;
    }

    int large_etc2=gfxdiag_is_large_etc2_rgba(internalformat,width,height);
    if(large_etc2){
        (void)gfxdiag_async_poll("before-ETC2");
        if(g_gfx_reclaim_deleted_pending>=GFX_RECLAIM_PREALLOC_FORCE_THRESHOLD){
            trace_log_printf("GLMEM","prealloc reclaim fmt=0x%x host=0x%x levels=%d %dx%d pending_delete=%u",
                internalformat,host_internalformat,levels,width,height,g_gfx_reclaim_deleted_pending);
            gfxdiag_finish_reclaim("pre-large-ETC2",0);
        }
    }

    gfxdiag_note_texstorage2d(target,levels,internalformat,width,height);
    uint64_t first_seq=g_gfxdiag_last.seq;
    uint64_t t0=trace_now_us();
    int retried=0;
    int retry_failed=0;
    __atomic_store_n(&g_gfxdebug_oom_seq,0u,__ATOMIC_RELAXED);
    g_real_glTexStorage2D(target,levels,host_internalformat,width,height);

    if(__atomic_load_n(&g_gfxdebug_oom_seq,__ATOMIC_RELAXED)==first_seq){
        trace_log_printf("GLMEM","OOM texStorage2D seq=%llu fmt=0x%x host=0x%x levels=%d %dx%d free=%lluKB pending_delete=%u",
            (unsigned long long)first_seq,internalformat,host_internalformat,levels,width,height,
            (unsigned long long)(mem_diag_process_free_bytes()>>10),g_gfx_reclaim_deleted_pending);
        (void)gfxdiag_drain_host_errors();
        gfxdiag_finish_reclaim("texstorage2d-oom-retry",1);

        gfxdiag_note_texstorage2d(target,levels,internalformat,width,height);
        uint64_t retry_seq=g_gfxdiag_last.seq;
        __atomic_store_n(&g_gfxdebug_oom_seq,0u,__ATOMIC_RELAXED);
        retried=1;
        g_real_glTexStorage2D(target,levels,host_internalformat,width,height);
        if(__atomic_load_n(&g_gfxdebug_oom_seq,__ATOMIC_RELAXED)==retry_seq){
            retry_failed=1;
            trace_log_printf("GLMEM","OOM retry failed texStorage2D seq=%llu fmt=0x%x host=0x%x levels=%d %dx%d pending_delete=%u",
                (unsigned long long)retry_seq,internalformat,host_internalformat,levels,width,height,g_gfx_reclaim_deleted_pending);
        }
    }
    uint64_t us=trace_now_us()-t0;
    if(bc||gfxdiag_is_trace_texture(width,height)||us>=4000ull||retried)
        trace_log_printf("GLTEX","storage2D seq=%llu fmt=0x%x host=0x%x levels=%d %dx%d time=%lluus bc=%d retried=%d retry_failed=%d pending_delete=%u",
            (unsigned long long)g_gfxdiag_last.seq,internalformat,host_internalformat,levels,width,height,
            (unsigned long long)us,bc,retried,retry_failed,g_gfx_reclaim_deleted_pending);
}
static void nx_glDeleteTextures(GLsizei n,const GLuint *textures){
    if(!g_real_glDeleteTextures)g_real_glDeleteTextures=(NxGlDeleteTexturesFn)nx_raw_gles_proc("glDeleteTextures");
    if(!g_real_glDeleteTextures)return;
    gfxdiag_note_simple("glDeleteTextures");
    g_real_glDeleteTextures(n,textures);
    if(n>0){
        unsigned add=(unsigned)n;
        unsigned room=UINT_MAX-g_gfx_reclaim_deleted_pending;
        g_gfx_reclaim_deleted_pending += add>room ? room : add;
        if(!g_gfx_reclaim_fence && g_gfx_reclaim_deleted_pending>=GFX_RECLAIM_DELETE_THRESHOLD)
            gfxdiag_async_submit("delete-threshold");
    }
}

static void nx_glTexStorage3D(GLenum target,GLsizei levels,GLenum internalformat,GLsizei width,GLsizei height,GLsizei depth){
    if(!g_real_glTexStorage3D) g_real_glTexStorage3D=(NxGlTexStorage3DFn)nx_raw_gles_proc("glTexStorage3D");
    if(!g_real_glTexStorage3D)return;
    GFXDIAG_RUN_RETRY("glTexStorage3D",g_real_glTexStorage3D(target,levels,internalformat,width,height,depth));
}

static void nx_glTexStorage2DMultisample(GLenum target,GLsizei samples,GLenum internalformat,
        GLsizei width,GLsizei height,GLboolean fixedsamplelocations){
    if(!g_real_glTexStorage2DMultisample)
        g_real_glTexStorage2DMultisample=(NxGlTexStorage2DMultisampleFn)nx_raw_gles_proc("glTexStorage2DMultisample");
    if(!g_real_glTexStorage2DMultisample)return;
    GFXDIAG_RUN_RETRY("glTexStorage2DMultisample",
        g_real_glTexStorage2DMultisample(target,samples,internalformat,width,height,fixedsamplelocations));
}

static void nx_glTexStorage3DMultisample(GLenum target,GLsizei samples,GLenum internalformat,
        GLsizei width,GLsizei height,GLsizei depth,GLboolean fixedsamplelocations){
    if(!g_real_glTexStorage3DMultisample)
        g_real_glTexStorage3DMultisample=(NxGlTexStorage3DMultisampleFn)nx_raw_gles_proc("glTexStorage3DMultisample");
    if(!g_real_glTexStorage3DMultisample)return;
    GFXDIAG_RUN_RETRY("glTexStorage3DMultisample",
        g_real_glTexStorage3DMultisample(target,samples,internalformat,width,height,depth,fixedsamplelocations));
}

static void nx_glRenderbufferStorage(GLenum target,GLenum internalformat,GLsizei width,GLsizei height){
    if(!g_real_glRenderbufferStorage)
        g_real_glRenderbufferStorage=(NxGlRenderbufferStorageFn)nx_raw_gles_proc("glRenderbufferStorage");
    if(!g_real_glRenderbufferStorage)return;
    GFXDIAG_RUN_RETRY("glRenderbufferStorage",g_real_glRenderbufferStorage(target,internalformat,width,height));
}

static void nx_glRenderbufferStorageMultisample(GLenum target,GLsizei samples,GLenum internalformat,
        GLsizei width,GLsizei height){
    if(!g_real_glRenderbufferStorageMultisample)
        g_real_glRenderbufferStorageMultisample=(NxGlRenderbufferStorageMultisampleFn)nx_raw_gles_proc("glRenderbufferStorageMultisample");
    if(!g_real_glRenderbufferStorageMultisample)return;
    GFXDIAG_RUN_RETRY("glRenderbufferStorageMultisample",
        g_real_glRenderbufferStorageMultisample(target,samples,internalformat,width,height));
}

static void nx_glBufferData(GLenum target,ptrdiff_t size,const void *data,GLenum usage){
    if(!g_real_glBufferData) g_real_glBufferData=(NxGlBufferDataFn)nx_raw_gles_proc("glBufferData");
    if(!g_real_glBufferData)return;
    gfxdiag_note_simple("glBufferData");
    g_real_glBufferData(target,size,data,usage);
}

static void nx_glGenerateMipmap(GLenum target){
    if(!g_real_glGenerateMipmap) g_real_glGenerateMipmap=(NxGlGenerateMipmapFn)nx_raw_gles_proc("glGenerateMipmap");
    if(!g_real_glGenerateMipmap)return;
    gfxdiag_note_simple("glGenerateMipmap");
    uint64_t t0=trace_now_us();
    g_real_glGenerateMipmap(target);
    uint64_t us=trace_now_us()-t0;
    if(us>=5000ull)trace_log_printf("GL","mipmap seq=%llu target=0x%x time=%lluus",
        (unsigned long long)g_gfxdiag_last.seq,target,(unsigned long long)us);
}

static GLuint nx_glCreateShader(GLenum type){
    if(!g_real_glCreateShader) g_real_glCreateShader=(NxGlCreateShaderFn)nx_raw_gles_proc("glCreateShader");
    if(!g_real_glCreateShader)return 0;
    gfxdiag_note_simple("glCreateShader");
    GLuint shader=g_real_glCreateShader(type);
    return shader;
}

static GLuint nx_glCreateProgram(void){
    if(!g_real_glCreateProgram) g_real_glCreateProgram=(NxGlCreateProgramFn)nx_raw_gles_proc("glCreateProgram");
    if(!g_real_glCreateProgram)return 0;
    gfxdiag_note_simple("glCreateProgram");
    GLuint program=g_real_glCreateProgram();
    return program;
}

static void nx_glCompileShader(GLuint shader){
    if(!g_real_glCompileShader) g_real_glCompileShader=(NxGlCompileShaderFn)nx_raw_gles_proc("glCompileShader");
    if(!g_real_glCompileShader)return;
    gfxdiag_note_simple("glCompileShader");
    g_real_glCompileShader(shader);
}

static void nx_glLinkProgram(GLuint program){
    if(!g_real_glLinkProgram) g_real_glLinkProgram=(NxGlLinkProgramFn)nx_raw_gles_proc("glLinkProgram");
    if(!g_real_glLinkProgram)return;
    gfxdiag_note_simple("glLinkProgram");
    g_real_glLinkProgram(program);
}

static void nx_glGetShaderiv(GLuint shader,GLenum pname,GLint *params){
    if(!g_real_glGetShaderiv)g_real_glGetShaderiv=(NxGlGetShaderivFn)nx_raw_gles_proc("glGetShaderiv");
    if(g_real_glGetShaderiv)g_real_glGetShaderiv(shader,pname,params);
}

static void nx_glGetProgramiv(GLuint program,GLenum pname,GLint *params){
    if(!g_real_glGetProgramiv)g_real_glGetProgramiv=(NxGlGetProgramivFn)nx_raw_gles_proc("glGetProgramiv");
    if(g_real_glGetProgramiv)g_real_glGetProgramiv(program,pname,params);
}

static void nx_glCompressedTexSubImage2D(GLenum target,GLint level,GLint xoffset,GLint yoffset,GLsizei width,GLsizei height,GLenum format,GLsizei imageSize,const void *data){
    if(!g_real_glCompressedTexSubImage2D)g_real_glCompressedTexSubImage2D=(NxGlCompressedTexSubImage2DFn)nx_raw_gles_proc("glCompressedTexSubImage2D");
    if(!g_real_glCompressedTexSubImage2D)return;

    GLenum host_format=format;
    GLsizei host_size=imageSize;
    const void *host_data=data;
    uint64_t transcode_us=0;
    int bc=gfxdiag_etc2_bc_prepare(format,width,height,imageSize,data,
        &host_format,&host_size,&host_data,&transcode_us);

    gfxdiag_note_simple("glCompressedTexSubImage2D");
    uint64_t first_seq=g_gfxdiag_last.seq;
    uint64_t t0=trace_now_us();
    int retried=0;
    int retry_failed=0;
    __atomic_store_n(&g_gfxdebug_oom_seq,0u,__ATOMIC_RELAXED);
    g_real_glCompressedTexSubImage2D(target,level,xoffset,yoffset,width,height,host_format,host_size,host_data);

    if(__atomic_load_n(&g_gfxdebug_oom_seq,__ATOMIC_RELAXED)==first_seq){
        trace_log_printf("GLMEM","OOM compressedSubImage2D seq=%llu fmt=0x%x host=0x%x level=%d %dx%d bytes=%d bc=%d pending_delete=%u",
            (unsigned long long)first_seq,format,host_format,level,width,height,host_size,bc,g_gfx_reclaim_deleted_pending);
        (void)gfxdiag_drain_host_errors();
        gfxdiag_finish_reclaim("compressed-subimage-oom-retry",0);

        gfxdiag_note_simple("glCompressedTexSubImage2D");
        uint64_t retry_seq=g_gfxdiag_last.seq;
        __atomic_store_n(&g_gfxdebug_oom_seq,0u,__ATOMIC_RELAXED);
        retried=1;
        g_real_glCompressedTexSubImage2D(target,level,xoffset,yoffset,width,height,host_format,host_size,host_data);
        if(__atomic_load_n(&g_gfxdebug_oom_seq,__ATOMIC_RELAXED)==retry_seq){
            retry_failed=1;
            trace_log_printf("GLMEM","OOM retry failed compressedSubImage2D seq=%llu fmt=0x%x host=0x%x level=%d %dx%d bytes=%d bc=%d",
                (unsigned long long)retry_seq,format,host_format,level,width,height,host_size,bc);
        }
    }

    uint64_t gl_us=trace_now_us()-t0;
    if(bc)mutexUnlock(&g_etc2_bc_lock);
    if(bc||retried||retry_failed||gfxdiag_is_trace_texture(width,height)||gl_us>=4000ull)
        trace_log_printf("GLTEX","compressedSubImage2D seq=%llu fmt=0x%x host=0x%x level=%d %dx%d bytes=%d cpu=%lluus gl=%lluus bc=%d retried=%d retry_failed=%d pending_delete=%u",
            (unsigned long long)g_gfxdiag_last.seq,format,host_format,level,width,height,host_size,
            (unsigned long long)transcode_us,(unsigned long long)gl_us,bc,retried,retry_failed,g_gfx_reclaim_deleted_pending);
}
static void *nx_eglGetProcAddress(const char *name){
    if(!name)return NULL;
    void *raw=nx_raw_gles_proc(name);
    if(!raw){
        if(!strcmp(name,"glGetError")) raw=(void*)&glGetError;
        else if(!strcmp(name,"glGetString")) raw=(void*)&glGetString;
        else if(!strcmp(name,"glGetIntegerv")) raw=(void*)&glGetIntegerv;
    }
    if(!raw)return NULL;
    if(!strcmp(name,"glGetError"))return(void*)&nx_glGetError;
    if(!strcmp(name,"glGetString"))return(void*)&nx_glGetString;
    if(!strcmp(name,"glCompressedTexImage2D"))return(void*)&nx_glCompressedTexImage2D;
    if(!strcmp(name,"glCompressedTexImage3D"))return(void*)&nx_glCompressedTexImage3D;
    if(!strcmp(name,"glTexImage2D"))return(void*)&nx_glTexImage2D;
    if(!strcmp(name,"glTexImage3D"))return(void*)&nx_glTexImage3D;
    if(!strcmp(name,"glTexSubImage2D"))return(void*)&nx_glTexSubImage2D;
    if(!strcmp(name,"glTexStorage2D")||!strcmp(name,"glTexStorage2DEXT"))return(void*)&nx_glTexStorage2D;
    if(!strcmp(name,"glDeleteTextures"))return(void*)&nx_glDeleteTextures;
    if(!strcmp(name,"glTexStorage3D")||!strcmp(name,"glTexStorage3DEXT"))return(void*)&nx_glTexStorage3D;
    if(!strcmp(name,"glTexStorage2DMultisample"))return(void*)&nx_glTexStorage2DMultisample;
    if(!strcmp(name,"glTexStorage3DMultisample"))return(void*)&nx_glTexStorage3DMultisample;
    if(!strcmp(name,"glRenderbufferStorage"))return(void*)&nx_glRenderbufferStorage;
    if(!strcmp(name,"glRenderbufferStorageMultisample"))return(void*)&nx_glRenderbufferStorageMultisample;
    if(!strcmp(name,"glBufferData"))return(void*)&nx_glBufferData;
    if(!strcmp(name,"glGenerateMipmap"))return(void*)&nx_glGenerateMipmap;
    if(!strcmp(name,"glCreateShader"))return(void*)&nx_glCreateShader;
    if(!strcmp(name,"glCreateProgram"))return(void*)&nx_glCreateProgram;
    if(!strcmp(name,"glCompileShader"))return(void*)&nx_glCompileShader;
    if(!strcmp(name,"glLinkProgram"))return(void*)&nx_glLinkProgram;
    if(!strcmp(name,"glGetShaderiv"))return(void*)&nx_glGetShaderiv;
    if(!strcmp(name,"glGetProgramiv"))return(void*)&nx_glGetProgramiv;
    if(!strcmp(name,"glFinish"))return(void*)&nx_glFinish_profile;
    if(!strcmp(name,"glClientWaitSync"))return(void*)&nx_glClientWaitSync_profile;
    if(!strcmp(name,"glCompressedTexSubImage2D"))return(void*)&nx_glCompressedTexSubImage2D;
    return raw;
}
#else
static void *nx_eglGetProcAddress(const char *name){return name?(void*)eglGetProcAddress(name):NULL;}
#endif

static uintptr_t nx_resolve_gles_proc(const char *name) {
    if (!name || name[0] != 'g' || name[1] != 'l') return 0;
    void *p = nx_eglGetProcAddress(name);

    if (!p && !strcmp(name, "glGetString")) p = (void *)&glGetString;
    if (!p && !strcmp(name, "glGetIntegerv")) p = (void *)&glGetIntegerv;
    return (uintptr_t)p;
}

static void *nx_dlsym(void*h,const char*name){
    uintptr_t a=0;
    g_dlerr=NULL;
    if(!name){g_dlerr="null symbol name";return NULL;}

    if(h&&h!=HANDLE_IMPORTS){
        so_module*m=(so_module*)h;
        a=so_try_find_addr_rx(m,name);

        if(!a)a=(uintptr_t)so_resolve_external(name);
    }else{

        a=(uintptr_t)so_resolve_external(name);
    }
    if(!a)a=imports_lookup(name);
    if(!a && h==HANDLE_IMPORTS)a=nx_resolve_gles_proc(name);
    if(!a){g_dlerr="symbol not found";return NULL;}
    return(void*)a;
}
typedef struct{const char*dli_fname;void*dli_fbase;const char*dli_sname;void*dli_saddr;}DlInfoCompat;
static int nx_dladdr(const void*addr,DlInfoCompat*i){(void)addr;if(i){memset(i,0,sizeof(*i));i->dli_fname="angrybirdas_nx";}return 1;}

static int sl_create_engine(void**e,unsigned a,const void*b,unsigned c,const void*d,const void*f){(void)a;(void)b;(void)c;(void)d;(void)f;if(e)*e=NULL;return-1;}

static int JNI_GetCreatedJavaVMs(void**buf,int len,int*n){if(n)*n=1;if(buf&&len>0)buf[0]=fake_vm;return JNI_OK;}

#define MAP(x) {#x,(uintptr_t)&x}
#define MAPN(n,x) {n,(uintptr_t)&x}

typedef struct { EGLSurface surface; NWindow *window; } NxEglWindowSurface;
static NxEglWindowSurface g_egl_window_surfaces[8];

static NWindow *nx_egl_window_for_surface(EGLSurface surface){
    for(unsigned i=0;i<sizeof(g_egl_window_surfaces)/sizeof(g_egl_window_surfaces[0]);i++)
        if(g_egl_window_surfaces[i].surface==surface) return g_egl_window_surfaces[i].window;
    return NULL;
}

static void nx_egl_track_window_surface(EGLSurface surface,NWindow *window){
    if(surface==EGL_NO_SURFACE||!window||!nwindowIsValid(window)) return;
    for(unsigned i=0;i<sizeof(g_egl_window_surfaces)/sizeof(g_egl_window_surfaces[0]);i++){
        if(g_egl_window_surfaces[i].surface==surface||g_egl_window_surfaces[i].surface==EGL_NO_SURFACE){
            g_egl_window_surfaces[i].surface=surface;
            g_egl_window_surfaces[i].window=window;
            return;
        }
    }
}

static void nx_egl_untrack_surface(EGLSurface surface){
    for(unsigned i=0;i<sizeof(g_egl_window_surfaces)/sizeof(g_egl_window_surfaces[0]);i++)
        if(g_egl_window_surfaces[i].surface==surface){
            g_egl_window_surfaces[i].surface=EGL_NO_SURFACE;
            g_egl_window_surfaces[i].window=NULL;
        }
}

static EGLSurface nx_eglCreateWindowSurface(EGLDisplay display,EGLConfig config,EGLNativeWindowType native_window,const EGLint *attribs){
    EGLSurface surface=eglCreateWindowSurface(display,config,native_window,attribs);
    nx_egl_track_window_surface(surface,(NWindow*)native_window);
    return surface;
}

static EGLBoolean nx_eglDestroySurface(EGLDisplay display,EGLSurface surface){
    EGLBoolean ok=eglDestroySurface(display,surface);
    if(ok==EGL_TRUE) nx_egl_untrack_surface(surface);
    return ok;
}

static EGLBoolean nx_eglQuerySurface(EGLDisplay display,EGLSurface surface,EGLint attribute,EGLint *value){
    EGLBoolean ok=eglQuerySurface(display,surface,attribute,value);
    if(ok==EGL_TRUE&&value&&(attribute==EGL_WIDTH||attribute==EGL_HEIGHT)&&*value<=0){
        NWindow *window=nx_egl_window_for_surface(surface);
        if(window){
            u32 width=0,height=0;
            if(R_SUCCEEDED(nwindowGetDimensions(window,&width,&height))&&width&&height){
                *value=(attribute==EGL_WIDTH)?(EGLint)width:(EGLint)height;
            }
        }
    }
    return ok;
}

static EGLBoolean nx_eglSwapBuffers(EGLDisplay display, EGLSurface surface){
    input_draw_cursor_overlay();
#if ENABLE_GFX_DIAGNOSTICS
    if(g_gfx_reclaim_fence){
        (void)gfxdiag_async_poll("swap");
        if(!g_gfx_reclaim_fence && g_gfx_reclaim_deleted_pending>=GFX_RECLAIM_DELETE_THRESHOLD)
            gfxdiag_async_submit("swap-followup");
    }
#endif
    trace_main_block_begin("gl:eglSwapBuffers",surface,NULL);
    uint64_t t0=trace_now_us();
    EGLBoolean ok=eglSwapBuffers(display,surface);
    uint64_t us=trace_now_us()-t0;
    trace_main_block_end();
    if(us>=12000ull)trace_log_printf("SWAP","time=%lluus ok=%d pending_delete=%u",
        (unsigned long long)us,(int)ok,
#if ENABLE_GFX_DIAGNOSTICS
        g_gfx_reclaim_deleted_pending
#else
        0u
#endif
    );
    return ok;
}

static DynLibFunction g_core[] = {
 MAPN("__android_log_print",alog_print),MAPN("__android_log_vprint",alog_vprint),MAPN("__android_log_write",alog_write),MAPN("__android_log_buf_write",alog_buf_write),MAPN("android_set_abort_message",android_abort_message),MAPN("__assert2",assert2),MAPN("__stack_chk_fail",stack_chk_fail),MAPN("__cxa_atexit",cxa_atexit_stub),MAPN("__cxa_finalize",cxa_finalize_stub),MAPN("__register_atfork",register_atfork_stub),MAPN("__errno",bionic_errno),MAPN("__system_property_get",system_property_get),MAPN("__system_property_find",system_property_find),MAPN("__system_property_read",system_property_read),MAPN("android_get_device_api_level",android_api_level),MAPN("__FD_SET_chk",fd_set_chk),MAPN("__FD_ISSET_chk",fd_isset_chk),MAPN("__gnu_strerror_r",gnu_strerror_r),MAPN("__ctype_get_mb_cur_max",ctype_mb_cur_max),
 MAPN("malloc",nx_malloc),MAPN("calloc",nx_calloc),MAPN("realloc",nx_realloc),MAP(free),MAPN("memalign",nx_memalign),MAPN("posix_memalign",nx_posix_memalign),MAP(memcpy),MAP(memmove),MAP(memset),MAP(memcmp),MAP(memchr),MAPN("memrchr",nx_memrchr),MAP(strlen),MAP(strnlen),MAP(strcmp),MAP(strncmp),MAP(strcasecmp),MAP(strcpy),MAP(strncpy),MAP(strcat),MAP(strncat),MAP(strchr),MAP(strrchr),MAP(strstr),MAPN("strcasestr",nx_strcasestr),MAP(strspn),MAP(strcspn),MAP(strdup),MAP(strtok_r),MAP(strerror),MAPN("strerror_r",nx_strerror_r),MAPN("fnmatch",nx_fnmatch),MAPN("strlcpy",nx_strlcpy),MAPN("basename",nx_basename),MAP(atoi),MAP(atol),MAP(strtol),MAP(strtoul),MAP(strtoll),MAP(strtoull),MAP(strtof),MAP(strtod),MAP(strtold),MAP(isspace),MAP(wmemchr),MAP(wcslen),MAP(wmemcmp),MAP(wcstoul),MAP(wcstoll),MAP(wcstoull),MAP(wcstof),MAP(wcstod),MAP(wcstold),MAP(swprintf),MAP(wcstol),MAP(towlower),MAP(towupper),MAP(iswlower),MAP(iswspace),MAP(iswprint),MAP(iswcntrl),MAP(iswupper),MAP(iswalpha),MAP(iswdigit),MAP(iswpunct),MAP(iswxdigit),MAP(iswblank),MAP(btowc),MAP(wctob),MAP(wcrtomb),MAP(mbrtowc),MAP(mbtowc),MAP(mbrlen),MAP(mbsrtowcs),MAPN("mbsnrtowcs",nx_mbsnrtowcs),MAPN("wcsnrtombs",nx_wcsnrtombs),
 MAPN("fopen",nx_file_fopen_profile),MAPN("fclose",file_fclose),MAPN("fread",nx_fread_profile),MAP(fwrite),MAP(fflush),MAP(fseek),MAP(ftell),MAP(fseeko),MAP(ftello),MAPN("fseeko64",fseeko),MAPN("ftello64",ftello),MAP(fgetc),MAP(fgets),MAP(fputc),MAP(fputs),MAP(fprintf),MAP(printf),MAPN("puts",nx_puts),MAP(vfprintf),MAP(vprintf),MAP(sprintf),MAP(snprintf),MAP(vsnprintf),MAP(vasprintf),MAP(fscanf),MAP(sscanf),MAP(vsscanf),MAP(fileno),MAP(fdopen),MAP(feof),MAP(ferror),MAP(clearerr),MAP(setbuf),MAPN("remove",file_remove),MAPN("rename",file_rename),
 MAPN("open",nx_file_open_profile),MAPN("close",nx_close),MAPN("read",nx_read),MAPN("write",nx_write),MAPN("fcntl",nx_fcntl),MAPN("readv",nx_readv),MAPN("writev",nx_writev),MAPN("pread",nx_pread),MAPN("pwrite",nx_pwrite),MAPN("lseek",nx_lseek),MAPN("lseek64",nx_lseek),MAPN("stat",file_stat),MAPN("lstat",file_lstat),MAPN("fstat",file_fstat),MAPN("statfs",file_statfs),MAPN("mkdir",file_mkdir),MAP(rmdir),MAPN("unlink",file_unlink),MAPN("link",nx_link),MAPN("access",file_access),MAP(chmod),MAP(fchmod),MAPN("ftruncate",nx_ftruncate),MAPN("truncate",file_truncate),MAPN("fsync",nx_fsync),MAPN("dup",nx_dup),MAPN("dup2",nx_dup2),MAPN("pipe",nx_pipe),MAPN("opendir",file_opendir),MAPN("readdir",file_readdir),MAPN("closedir",file_closedir),MAP(readlink),MAPN("realpath",file_realpath),MAP(symlink),MAPN("flock",nx_flock),
 MAP(clock),MAPN("clock_gettime",bionic_clock_gettime),MAPN("clock_getres",bionic_clock_getres),MAP(gettimeofday),MAP(time),MAP(difftime),MAP(nanosleep),MAP(usleep),MAP(localtime),MAP(localtime_r),MAP(gmtime),MAP(gmtime_r),MAP(mktime),MAP(strftime),MAP(setlocale),MAP(localeconv),MAPN("newlocale",nx_newlocale),MAPN("uselocale",nx_uselocale),MAPN("freelocale",nx_freelocale),MAPN("strftime_l",nx_strftime_l),MAPN("strtoll_l",nx_strtoll_l),MAPN("strtoull_l",nx_strtoull_l),MAPN("strtold_l",nx_strtold_l),MAPN("strcoll_l",nx_strcoll_l),MAPN("strxfrm_l",nx_strxfrm_l),MAPN("wcscoll_l",nx_wcscoll_l),MAPN("wcsxfrm_l",nx_wcsxfrm_l),MAPN("iswlower_l",nx_iswlower_l),MAPN("iswspace_l",nx_iswspace_l),MAPN("iswprint_l",nx_iswprint_l),MAPN("iswcntrl_l",nx_iswcntrl_l),MAPN("iswupper_l",nx_iswupper_l),MAPN("iswalpha_l",nx_iswalpha_l),MAPN("iswdigit_l",nx_iswdigit_l),MAPN("iswpunct_l",nx_iswpunct_l),MAPN("iswxdigit_l",nx_iswxdigit_l),MAPN("iswblank_l",nx_iswblank_l),MAPN("towupper_l",nx_towupper_l),MAPN("towlower_l",nx_towlower_l),MAPN("openlog",nx_openlog),MAPN("syslog",nx_syslog),MAPN("closelog",nx_closelog),
 MAP(acos),MAP(acosf),MAP(logb),MAP(scalbn),MAP(asinf),MAP(atan),MAP(atanf),MAP(atan2),MAP(atan2f),MAP(cos),MAP(cosf),MAP(sin),MAP(sinf),MAPN("sincos",nx_sincos),MAPN("sincosf",nx_sincosf),MAP(tan),MAP(tanf),MAP(tanhf),MAP(exp),MAP(expf),MAP(exp2),MAP(exp2f),MAP(log),MAP(logf),MAP(log10),MAP(log10f),MAP(log2f),MAP(pow),MAP(powf),MAP(fmod),MAP(fmodf),MAP(sqrtf),MAP(hypot),MAP(hypotf),MAP(ldexp),MAP(ldexpf),MAP(modf),MAP(modff),MAP(remainderf),MAP(nearbyintf),MAP(nextafter),MAP(cbrtf),
 MAP(qsort),MAP(bsearch),MAP(div),MAPN("abort",nx_guest_abort),MAPN("exit",nx_guest_exit),MAPN("_exit",nx_guest__exit),MAP(getenv),MAP(setenv),MAP(unsetenv),MAP(getpid),MAPN("getuid",nx_getuid),MAPN("geteuid",nx_geteuid),MAPN("getegid",nx_getegid),MAP(getcwd),MAP(isatty),MAPN("getpagesize",nx_getpagesize),MAPN("gethostname",nx_gethostname),MAPN("uname",nx_uname),MAPN("getpriority",nx_getpriority),MAPN("setpriority",nx_setpriority),MAPN("ioctl",nx_ioctl),MAPN("getpwuid",nx_getpwuid),MAPN("getpwuid_r",nx_getpwuid_r),MAPN("utime",nx_utime),MAPN("utimes",nx_utimes),MAPN("futimens",nx_futimens),MAPN("sendfile",nx_sendfile),MAPN("srand48",nx_srand48),MAPN("lrand48",nx_lrand48),MAPN("sysconf",bionic_sysconf),MAP(sched_yield),MAPN("gettid",bionic_gettid),MAPN("getauxval",bionic_getauxval),
 MAP(socket),MAP(bind),MAP(listen),MAP(accept),MAP(connect),MAP(shutdown),MAP(setsockopt),MAP(getsockopt),MAP(getpeername),MAP(getsockname),MAP(send),MAP(sendto),MAP(sendmsg),MAP(recv),MAP(recvfrom),MAP(recvmsg),MAPN("select",nx_select),MAPN("poll",nx_poll),MAP(getaddrinfo),MAP(freeaddrinfo),MAP(gethostbyname),MAP(gethostbyaddr),MAP(getnameinfo),MAP(inet_addr),MAP(inet_ntop),MAP(inet_pton),MAPN("if_nametoindex",nx_if_nametoindex),
 MAPN("inflate",nx_inflate_profile),MAP(inflateEnd),MAP(inflateInit2_),MAP(eglChooseConfig),MAP(eglCreateContext),MAP(eglCreatePbufferSurface),MAPN("eglCreateWindowSurface",nx_eglCreateWindowSurface),MAP(eglDestroyContext),MAPN("eglDestroySurface",nx_eglDestroySurface),MAP(eglGetConfigAttrib),MAP(eglGetCurrentContext),MAP(eglGetCurrentSurface),MAP(eglGetDisplay),MAP(eglGetError),MAPN("eglGetProcAddress",nx_eglGetProcAddress),MAP(eglInitialize),MAP(eglMakeCurrent),MAP(eglQueryString),MAPN("eglQuerySurface",nx_eglQuerySurface),MAP(eglSurfaceAttrib),MAPN("eglSwapBuffers",nx_eglSwapBuffers),MAP(eglSwapInterval),MAP(eglTerminate),
 MAPN("mmap",bionic_mmap),MAPN("munmap",bionic_munmap),MAPN("mprotect",bionic_mprotect),MAPN("madvise",bionic_madvise),MAPN("syscall",bionic_syscall),
 MAPN("pthread_mutex_init",bionic_pthread_mutex_init),MAPN("pthread_mutex_destroy",bionic_pthread_mutex_destroy),MAPN("pthread_mutex_lock",bionic_pthread_mutex_lock),MAPN("pthread_mutex_trylock",bionic_pthread_mutex_trylock),MAPN("pthread_mutex_unlock",bionic_pthread_mutex_unlock),MAPN("pthread_cond_init",bionic_pthread_cond_init),MAPN("pthread_cond_destroy",bionic_pthread_cond_destroy),MAPN("pthread_cond_wait",bionic_pthread_cond_wait),MAPN("pthread_cond_timedwait",bionic_pthread_cond_timedwait),MAPN("pthread_cond_signal",bionic_pthread_cond_signal),MAPN("pthread_cond_broadcast",bionic_pthread_cond_broadcast),MAPN("pthread_rwlock_init",bionic_pthread_rwlock_init),MAPN("pthread_rwlock_rdlock",bionic_pthread_rwlock_rdlock),MAPN("pthread_rwlock_wrlock",bionic_pthread_rwlock_wrlock),MAPN("pthread_rwlock_unlock",bionic_pthread_rwlock_unlock),MAPN("pthread_once",bionic_pthread_once),MAPN("pthread_create",bionic_pthread_create),MAPN("pthread_join",bionic_pthread_join),MAPN("pthread_detach",bionic_pthread_detach),MAPN("pthread_exit",bionic_pthread_exit),MAPN("pthread_self",bionic_pthread_self),MAPN("pthread_equal",bionic_pthread_equal),MAPN("pthread_setname_np",bionic_pthread_setname_np),MAPN("pthread_attr_init",bionic_pthread_attr_init),MAPN("pthread_attr_destroy",bionic_pthread_attr_destroy),MAPN("pthread_attr_setdetachstate",bionic_pthread_attr_setdetachstate),MAPN("pthread_attr_setstacksize",bionic_pthread_attr_setstacksize),MAPN("pthread_attr_getstack",bionic_pthread_attr_getstack),MAPN("pthread_getattr_np",bionic_pthread_getattr_np),MAPN("pthread_key_create",bionic_pthread_key_create),MAPN("pthread_key_delete",bionic_pthread_key_delete),MAPN("pthread_setspecific",bionic_pthread_setspecific),MAPN("pthread_getspecific",bionic_pthread_getspecific),MAPN("pthread_kill",bionic_pthread_kill),MAPN("pthread_sigmask",bionic_pthread_sigmask),MAPN("pthread_mutexattr_init",nx_pthread_attr8_init),MAPN("pthread_mutexattr_destroy",nx_attr_noop),MAPN("pthread_mutexattr_settype",nx_attr_settype),MAPN("pthread_condattr_init",nx_pthread_attr8_init),MAPN("pthread_condattr_destroy",nx_attr_noop),MAPN("pthread_condattr_setclock",nx_condattr_setclock),
 MAPN("sem_init",bionic_sem_init),MAPN("sem_destroy",bionic_sem_destroy),MAPN("sem_post",bionic_sem_post),MAPN("sem_wait",bionic_sem_wait),MAPN("sem_trywait",bionic_sem_trywait),MAPN("sem_timedwait",bionic_sem_timedwait),MAPN("sem_getvalue",bionic_sem_getvalue),
 MAPN("dlopen",nx_dlopen),MAPN("dlsym",nx_dlsym),MAPN("dlclose",nx_dlclose),MAPN("dlerror",nx_dlerror),MAPN("dladdr",nx_dladdr),MAPN("dl_iterate_phdr",so_dl_iterate_phdr),MAPN("JNI_GetCreatedJavaVMs",JNI_GetCreatedJavaVMs),
 MAPN("setjmp",bionic_setjmp),MAPN("_setjmp",bionic_setjmp),MAPN("sigsetjmp",bionic_setjmp),MAPN("longjmp",bionic_longjmp),MAPN("_longjmp",bionic_longjmp),MAPN("siglongjmp",bionic_longjmp),MAPN("sigemptyset",nx_sigemptyset),MAPN("sigfillset",nx_sigfillset),MAPN("sigaddset",nx_sigaddset),MAPN("sigdelset",nx_sigdelset),MAPN("sigsuspend",nx_sigsuspend),MAPN("sigaction",nx_sigaction),MAPN("signal",nx_signal),MAPN("raise",nx_raise),MAPN("sigaltstack",nx_sigaltstack),MAPN("kill",nx_kill),MAPN("fork",nx_fork),MAPN("execl",nx_execl),MAPN("waitpid",nx_waitpid),MAPN("prctl",nx_prctl),MAPN("process_vm_readv",nx_process_vm_readv),MAPN("ptrace",nx_ptrace),
 MAPN("AAudio_createStreamBuilder",aaudio_bridge_create_stream_builder),MAPN("AAudioStreamBuilder_delete",aaudio_bridge_builder_delete),MAPN("AAudioStreamBuilder_openStream",aaudio_bridge_builder_open_stream),MAPN("AAudioStreamBuilder_setBufferCapacityInFrames",aaudio_bridge_builder_set_buffer_capacity),MAPN("AAudioStreamBuilder_setChannelCount",aaudio_bridge_builder_set_channel_count),MAPN("AAudioStreamBuilder_setDataCallback",aaudio_bridge_builder_set_data_callback),MAPN("AAudioStreamBuilder_setDeviceId",aaudio_bridge_builder_set_device_id),MAPN("AAudioStreamBuilder_setDirection",aaudio_bridge_builder_set_direction),MAPN("AAudioStreamBuilder_setErrorCallback",aaudio_bridge_builder_set_error_callback),MAPN("AAudioStreamBuilder_setFormat",aaudio_bridge_builder_set_format),MAPN("AAudioStreamBuilder_setFramesPerDataCallback",aaudio_bridge_builder_set_frames_per_callback),MAPN("AAudioStreamBuilder_setPerformanceMode",aaudio_bridge_builder_set_performance_mode),MAPN("AAudioStreamBuilder_setSampleRate",aaudio_bridge_builder_set_sample_rate),MAPN("AAudioStreamBuilder_setSessionId",aaudio_bridge_builder_set_session_id),MAPN("AAudioStreamBuilder_setSharingMode",aaudio_bridge_builder_set_sharing_mode),MAPN("AAudioStream_close",aaudio_bridge_stream_close),MAPN("AAudioStream_requestStart",aaudio_bridge_stream_request_start),MAPN("AAudioStream_requestStop",aaudio_bridge_stream_request_stop),MAPN("AAudioStream_waitForStateChange",aaudio_bridge_stream_wait_for_state_change),MAPN("AAudioStream_getState",aaudio_bridge_stream_get_state),MAPN("AAudioStream_getBufferCapacityInFrames",aaudio_bridge_stream_get_buffer_capacity),MAPN("AAudioStream_getBufferSizeInFrames",aaudio_bridge_stream_get_buffer_size),MAPN("AAudioStream_setBufferSizeInFrames",aaudio_bridge_stream_set_buffer_size),MAPN("AAudioStream_getDeviceId",aaudio_bridge_stream_get_device_id),MAPN("AAudioStream_getFramesPerBurst",aaudio_bridge_stream_get_frames_per_burst),MAPN("AAudioStream_getSessionId",aaudio_bridge_stream_get_session_id),MAPN("AAudioStream_getXRunCount",aaudio_bridge_stream_get_xrun_count),MAPN("AAudioStream_isMMapUsed",aaudio_bridge_stream_is_mmap_used),MAPN("AAudioStream_getSampleRate",aaudio_bridge_stream_get_sample_rate),MAPN("AAudioStream_getChannelCount",aaudio_bridge_stream_get_channel_count),MAPN("AAudioStream_getFormat",aaudio_bridge_stream_get_format),MAPN("AAudioStream_getDirection",aaudio_bridge_stream_get_direction),MAPN("AAudioStream_getPerformanceMode",aaudio_bridge_stream_get_performance_mode),MAPN("AAudioStream_getSharingMode",aaudio_bridge_stream_get_sharing_mode),MAPN("AAudioStream_getFramesWritten",aaudio_bridge_stream_get_frames_written),MAPN("slCreateEngine",sl_create_engine),
 {"__sF",(uintptr_t)&guest_sF[0]},{"stdout",(uintptr_t)&guest_stdout},{"stderr",(uintptr_t)&guest_stderr}
};
#undef MAP
#undef MAPN

void imports_init(void){if(g_imports_ready)return;g_imports_ready=1;guest_stdout=stdout;guest_stderr=stderr;memset(guest_sF,0,sizeof guest_sF);}
uintptr_t imports_lookup(const char *name){if(!name)return 0;imports_init();uintptr_t a=android_ndk_lookup(name);if(a)return a;a=media_lookup(name);if(a)return a;for(size_t i=0;i<sizeof(g_core)/sizeof(g_core[0]);i++)if(!strcmp(name,g_core[i].symbol))return g_core[i].func;return 0;}
int imports_resolve_module(so_module*m){imports_init();return so_resolve(m,g_core,(int)(sizeof(g_core)/sizeof(g_core[0])),imports_fallback(),imports_lookup);}
