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
    if(p>=5 && f){
        crash_log_printf("alog[%d][%s]: ",p,t?t:"");
        crash_log_vprintf(f,a);
        crash_log_printf("\n");
    }
    return 0;
}
static int alog_print(int p,const char*t,const char*f,...){
    va_list a; va_start(a,f); int r=alog_vprint(p,t,f,a); va_end(a); return r;
}
static int alog_write(int p,const char*t,const char*s){
    if(p>=5)crash_log_printf("alog[%d][%s]: %s\n",p,t?t:"",s?s:"");
    return 0;
}
static int alog_buf_write(int b,int p,const char*t,const char*s){(void)b;return alog_write(p,t,s);}

static void *nx_malloc(size_t n){
    void *p=malloc(n);
    if(!p)crash_log_printf("alloc: malloc(%lu) FAILED\n",(unsigned long)n);
    return p;
}
static void *nx_calloc(size_t n,size_t z){
    void *p=calloc(n,z);
    if(!p && n && z)crash_log_printf("alloc: calloc(%lu,%lu) FAILED\n",(unsigned long)n,(unsigned long)z);
    return p;
}
static void *nx_realloc(void *old,size_t n){
    void *p=realloc(old,n);
    if(!p && n)crash_log_printf("alloc: realloc(%p,%lu) FAILED\n",old,(unsigned long)n);
    return p;
}
static void *nx_memalign(size_t a,size_t n){
    void *p=memalign(a,n);
    if(!p)crash_log_printf("alloc: memalign(%lu,%lu) FAILED\n",(unsigned long)a,(unsigned long)n);
    return p;
}

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
    return read(fd,buf,count);
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
    if(!p){
        crash_log_printf("alloc: posix_memalign(%lu,%lu) FAILED\n",
                         (unsigned long)alignment,(unsigned long)size);
        return ENOMEM;
    }
    *memptr=p;
    return 0;
}
static Mutex g_pio_mutex;
static ssize_t nx_pread(int fd,void *buf,size_t count,off_t offset){
    if(fakefd_is_range(fd)){errno=ESPIPE;return-1;}
    if(offset<0){errno=EINVAL;return-1;}
    mutexLock(&g_pio_mutex);
    off_t old=lseek(fd,0,SEEK_CUR);
    if(old==(off_t)-1){mutexUnlock(&g_pio_mutex);return-1;}
    if(lseek(fd,offset,SEEK_SET)==(off_t)-1){int e=errno;mutexUnlock(&g_pio_mutex);errno=e;return-1;}
    ssize_t ret=nx_read(fd,buf,count);
    int e=errno;
    if(lseek(fd,old,SEEK_SET)==(off_t)-1&&ret>=0){ret=-1;e=errno;}
    mutexUnlock(&g_pio_mutex);
    errno=e;
    return ret;
}
static ssize_t nx_pwrite(int fd,const void *buf,size_t count,off_t offset){
    if(fakefd_is_range(fd)){errno=ESPIPE;return-1;}
    if(offset<0){errno=EINVAL;return-1;}
    mutexLock(&g_pio_mutex);
    off_t old=lseek(fd,0,SEEK_CUR);
    if(old==(off_t)-1){int e=errno;mutexUnlock(&g_pio_mutex);errno=e;return-1;}
    if(lseek(fd,offset,SEEK_SET)==(off_t)-1){int e=errno;mutexUnlock(&g_pio_mutex);errno=e;return-1;}
    ssize_t ret=nx_write(fd,buf,count);
    int e=ret<0?errno:0;
    if(lseek(fd,old,SEEK_SET)==(off_t)-1&&ret>=0){ret=-1;e=errno;}
    mutexUnlock(&g_pio_mutex);
    errno=e;
    return ret;
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

static uintptr_t nx_resolve_gles_proc(const char *name) {
    if (!name || name[0] != 'g' || name[1] != 'l') return 0;
    void *p = (void *)eglGetProcAddress(name);

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
    return eglSwapBuffers(display,surface);
}

static DynLibFunction g_core[] = {
 MAPN("__android_log_print",alog_print),MAPN("__android_log_vprint",alog_vprint),MAPN("__android_log_write",alog_write),MAPN("__android_log_buf_write",alog_buf_write),MAPN("android_set_abort_message",android_abort_message),MAPN("__assert2",assert2),MAPN("__stack_chk_fail",stack_chk_fail),MAPN("__cxa_atexit",cxa_atexit_stub),MAPN("__cxa_finalize",cxa_finalize_stub),MAPN("__register_atfork",register_atfork_stub),MAPN("__errno",bionic_errno),MAPN("__system_property_get",system_property_get),MAPN("__system_property_find",system_property_find),MAPN("__system_property_read",system_property_read),MAPN("android_get_device_api_level",android_api_level),MAPN("__FD_SET_chk",fd_set_chk),MAPN("__FD_ISSET_chk",fd_isset_chk),MAPN("__gnu_strerror_r",gnu_strerror_r),MAPN("__ctype_get_mb_cur_max",ctype_mb_cur_max),
 MAPN("malloc",nx_malloc),MAPN("calloc",nx_calloc),MAPN("realloc",nx_realloc),MAP(free),MAPN("memalign",nx_memalign),MAPN("posix_memalign",nx_posix_memalign),MAP(memcpy),MAP(memmove),MAP(memset),MAP(memcmp),MAP(memchr),MAPN("memrchr",nx_memrchr),MAP(strlen),MAP(strnlen),MAP(strcmp),MAP(strncmp),MAP(strcasecmp),MAP(strcpy),MAP(strncpy),MAP(strcat),MAP(strncat),MAP(strchr),MAP(strrchr),MAP(strstr),MAPN("strcasestr",nx_strcasestr),MAP(strspn),MAP(strcspn),MAP(strdup),MAP(strtok_r),MAP(strerror),MAPN("strerror_r",nx_strerror_r),MAPN("fnmatch",nx_fnmatch),MAPN("strlcpy",nx_strlcpy),MAPN("basename",nx_basename),MAP(atoi),MAP(atol),MAP(strtol),MAP(strtoul),MAP(strtoll),MAP(strtoull),MAP(strtof),MAP(strtod),MAP(strtold),MAP(isspace),MAP(wmemchr),MAP(wcslen),MAP(wmemcmp),MAP(wcstoul),MAP(wcstoll),MAP(wcstoull),MAP(wcstof),MAP(wcstod),MAP(wcstold),MAP(swprintf),MAP(wcstol),MAP(towlower),MAP(towupper),MAP(iswlower),MAP(iswspace),MAP(iswprint),MAP(iswcntrl),MAP(iswupper),MAP(iswalpha),MAP(iswdigit),MAP(iswpunct),MAP(iswxdigit),MAP(iswblank),MAP(btowc),MAP(wctob),MAP(wcrtomb),MAP(mbrtowc),MAP(mbtowc),MAP(mbrlen),MAP(mbsrtowcs),MAPN("mbsnrtowcs",nx_mbsnrtowcs),MAPN("wcsnrtombs",nx_wcsnrtombs),
 MAPN("fopen",file_fopen),MAP(fclose),MAP(fread),MAP(fwrite),MAP(fflush),MAP(fseek),MAP(ftell),MAP(fseeko),MAP(ftello),MAPN("fseeko64",fseeko),MAPN("ftello64",ftello),MAP(fgetc),MAP(fgets),MAP(fputc),MAP(fputs),MAP(fprintf),MAP(printf),MAPN("puts",nx_puts),MAP(vfprintf),MAP(vprintf),MAP(sprintf),MAP(snprintf),MAP(vsnprintf),MAP(vasprintf),MAP(fscanf),MAP(sscanf),MAP(vsscanf),MAP(fileno),MAP(fdopen),MAP(feof),MAP(ferror),MAP(clearerr),MAP(setbuf),MAPN("remove",file_remove),MAPN("rename",file_rename),
 MAPN("open",file_open),MAPN("close",nx_close),MAPN("read",nx_read),MAPN("write",nx_write),MAPN("fcntl",nx_fcntl),MAPN("readv",nx_readv),MAPN("writev",nx_writev),MAPN("pread",nx_pread),MAPN("pwrite",nx_pwrite),MAPN("lseek",nx_lseek),MAPN("lseek64",nx_lseek),MAPN("stat",file_stat),MAPN("lstat",file_lstat),MAPN("fstat",file_fstat),MAPN("statfs",file_statfs),MAPN("mkdir",file_mkdir),MAP(rmdir),MAPN("unlink",file_unlink),MAPN("link",nx_link),MAPN("access",file_access),MAP(chmod),MAP(fchmod),MAPN("ftruncate",nx_ftruncate),MAPN("truncate",file_truncate),MAPN("fsync",nx_fsync),MAPN("dup",nx_dup),MAPN("dup2",nx_dup2),MAPN("pipe",nx_pipe),MAPN("opendir",file_opendir),MAPN("readdir",file_readdir),MAPN("closedir",file_closedir),MAP(readlink),MAPN("realpath",file_realpath),MAP(symlink),MAPN("flock",nx_flock),
 MAP(clock),MAPN("clock_gettime",bionic_clock_gettime),MAPN("clock_getres",bionic_clock_getres),MAP(gettimeofday),MAP(time),MAP(difftime),MAP(nanosleep),MAP(usleep),MAP(localtime),MAP(localtime_r),MAP(gmtime),MAP(gmtime_r),MAP(mktime),MAP(strftime),MAP(setlocale),MAP(localeconv),MAPN("newlocale",nx_newlocale),MAPN("uselocale",nx_uselocale),MAPN("freelocale",nx_freelocale),MAPN("strftime_l",nx_strftime_l),MAPN("strtoll_l",nx_strtoll_l),MAPN("strtoull_l",nx_strtoull_l),MAPN("strtold_l",nx_strtold_l),MAPN("strcoll_l",nx_strcoll_l),MAPN("strxfrm_l",nx_strxfrm_l),MAPN("wcscoll_l",nx_wcscoll_l),MAPN("wcsxfrm_l",nx_wcsxfrm_l),MAPN("iswlower_l",nx_iswlower_l),MAPN("iswspace_l",nx_iswspace_l),MAPN("iswprint_l",nx_iswprint_l),MAPN("iswcntrl_l",nx_iswcntrl_l),MAPN("iswupper_l",nx_iswupper_l),MAPN("iswalpha_l",nx_iswalpha_l),MAPN("iswdigit_l",nx_iswdigit_l),MAPN("iswpunct_l",nx_iswpunct_l),MAPN("iswxdigit_l",nx_iswxdigit_l),MAPN("iswblank_l",nx_iswblank_l),MAPN("towupper_l",nx_towupper_l),MAPN("towlower_l",nx_towlower_l),MAPN("openlog",nx_openlog),MAPN("syslog",nx_syslog),MAPN("closelog",nx_closelog),
 MAP(acos),MAP(acosf),MAP(logb),MAP(scalbn),MAP(asinf),MAP(atan),MAP(atanf),MAP(atan2),MAP(atan2f),MAP(cos),MAP(cosf),MAP(sin),MAP(sinf),MAPN("sincos",nx_sincos),MAPN("sincosf",nx_sincosf),MAP(tan),MAP(tanf),MAP(tanhf),MAP(exp),MAP(expf),MAP(exp2),MAP(exp2f),MAP(log),MAP(logf),MAP(log10),MAP(log10f),MAP(log2f),MAP(pow),MAP(powf),MAP(fmod),MAP(fmodf),MAP(sqrtf),MAP(hypot),MAP(hypotf),MAP(ldexp),MAP(ldexpf),MAP(modf),MAP(modff),MAP(remainderf),MAP(nearbyintf),MAP(nextafter),MAP(cbrtf),
 MAP(qsort),MAP(bsearch),MAP(div),MAPN("abort",nx_guest_abort),MAPN("exit",nx_guest_exit),MAPN("_exit",nx_guest__exit),MAP(getenv),MAP(setenv),MAP(unsetenv),MAP(getpid),MAPN("getuid",nx_getuid),MAPN("geteuid",nx_geteuid),MAPN("getegid",nx_getegid),MAP(getcwd),MAP(isatty),MAPN("getpagesize",nx_getpagesize),MAPN("gethostname",nx_gethostname),MAPN("uname",nx_uname),MAPN("getpriority",nx_getpriority),MAPN("setpriority",nx_setpriority),MAPN("ioctl",nx_ioctl),MAPN("getpwuid",nx_getpwuid),MAPN("getpwuid_r",nx_getpwuid_r),MAPN("utime",nx_utime),MAPN("utimes",nx_utimes),MAPN("futimens",nx_futimens),MAPN("sendfile",nx_sendfile),MAPN("srand48",nx_srand48),MAPN("lrand48",nx_lrand48),MAPN("sysconf",bionic_sysconf),MAP(sched_yield),MAPN("gettid",bionic_gettid),MAPN("getauxval",bionic_getauxval),
 MAP(socket),MAP(bind),MAP(listen),MAP(accept),MAP(connect),MAP(shutdown),MAP(setsockopt),MAP(getsockopt),MAP(getpeername),MAP(getsockname),MAP(send),MAP(sendto),MAP(sendmsg),MAP(recv),MAP(recvfrom),MAP(recvmsg),MAPN("select",nx_select),MAPN("poll",nx_poll),MAP(getaddrinfo),MAP(freeaddrinfo),MAP(gethostbyname),MAP(gethostbyaddr),MAP(getnameinfo),MAP(inet_addr),MAP(inet_ntop),MAP(inet_pton),MAPN("if_nametoindex",nx_if_nametoindex),
 MAP(inflate),MAP(inflateEnd),MAP(inflateInit2_),MAP(eglChooseConfig),MAP(eglCreateContext),MAP(eglCreatePbufferSurface),MAPN("eglCreateWindowSurface",nx_eglCreateWindowSurface),MAP(eglDestroyContext),MAPN("eglDestroySurface",nx_eglDestroySurface),MAP(eglGetConfigAttrib),MAP(eglGetCurrentContext),MAP(eglGetCurrentSurface),MAP(eglGetDisplay),MAP(eglGetError),MAP(eglGetProcAddress),MAP(eglInitialize),MAP(eglMakeCurrent),MAP(eglQueryString),MAPN("eglQuerySurface",nx_eglQuerySurface),MAP(eglSurfaceAttrib),MAPN("eglSwapBuffers",nx_eglSwapBuffers),MAP(eglSwapInterval),MAP(eglTerminate),
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
