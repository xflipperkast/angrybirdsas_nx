#include "file_bridge.h"
#include "config.h"
#include "trace_log.h"
#include <switch.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t pad2;
    int64_t st_blocks;
    struct { int64_t tv_sec, tv_nsec; } ast_atim;
    struct { int64_t tv_sec, tv_nsec; } ast_mtim;
    struct { int64_t tv_sec, tv_nsec; } ast_ctim;
    uint32_t unused4;
    uint32_t unused5;
} AndroidStat;

typedef struct {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_fsid;
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
} AndroidStatfs;

_Static_assert(sizeof(AndroidStat) == 128, "Android aarch64 struct stat size changed");
_Static_assert(sizeof(AndroidStatfs) == 120, "Android aarch64 struct statfs size changed");

#define POSITIONED_FD_SLOTS 1024

typedef struct {
    int valid;
    int host_flags;
    FsFile file;
    char *path;
} PositionedFdSlot;

static Mutex g_positioned_fd_lock;
static PositionedFdSlot g_positioned_fd[POSITIONED_FD_SLOTS];
static Mutex g_positioned_fallback_lock;
static uint64_t g_positioned_calls;
static uint64_t g_positioned_bytes;
static uint64_t g_positioned_errors;
static uint64_t g_positioned_fallbacks;
static uint64_t g_positioned_slow;
static uint64_t g_positioned_max_us;
static uint64_t g_positioned_last_summary_us;

static void atomic_max_u64(uint64_t *dst,uint64_t value){
    uint64_t cur=__atomic_load_n(dst,__ATOMIC_RELAXED);
    while(cur<value&&!__atomic_compare_exchange_n(dst,&cur,value,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}

static void positioned_trace_note(const char *kind,int fd,off_t offset,size_t requested,
        ssize_t result,uint64_t elapsed,int fallback,const char *path){
    __atomic_add_fetch(&g_positioned_calls,1u,__ATOMIC_RELAXED);
    if(result>0)__atomic_add_fetch(&g_positioned_bytes,(uint64_t)result,__ATOMIC_RELAXED);
    if(result<0)__atomic_add_fetch(&g_positioned_errors,1u,__ATOMIC_RELAXED);
    if(fallback)__atomic_add_fetch(&g_positioned_fallbacks,1u,__ATOMIC_RELAXED);
    if(elapsed>=TRACE_SLOW_IO_US)__atomic_add_fetch(&g_positioned_slow,1u,__ATOMIC_RELAXED);
    atomic_max_u64(&g_positioned_max_us,elapsed);

    if(elapsed>=TRACE_SLOW_IO_US || result<0){
        trace_log_printf("IO", "%s fd=%d off=%lld req=%zu got=%lld time=%lluus path=%s%s",
            kind,fd,(long long)offset,requested,(long long)result,
            (unsigned long long)elapsed,path&&*path?path:"?",fallback?" fallback-seek":"");
    }

    uint64_t now=trace_now_us();
    uint64_t last=__atomic_load_n(&g_positioned_last_summary_us,__ATOMIC_RELAXED);
    if(now-last>=1000000ull && __atomic_compare_exchange_n(&g_positioned_last_summary_us,&last,now,0,
            __ATOMIC_RELAXED,__ATOMIC_RELAXED)){
        uint64_t calls=__atomic_exchange_n(&g_positioned_calls,0u,__ATOMIC_RELAXED);
        uint64_t bytes=__atomic_exchange_n(&g_positioned_bytes,0u,__ATOMIC_RELAXED);
        uint64_t errors=__atomic_exchange_n(&g_positioned_errors,0u,__ATOMIC_RELAXED);
        uint64_t fallbacks=__atomic_exchange_n(&g_positioned_fallbacks,0u,__ATOMIC_RELAXED);
        uint64_t slow=__atomic_exchange_n(&g_positioned_slow,0u,__ATOMIC_RELAXED);
        uint64_t max_us=__atomic_exchange_n(&g_positioned_max_us,0u,__ATOMIC_RELAXED);
        if(calls)trace_log_printf("IOSTAT", "positioned calls=%llu bytes=%llu slow=%llu max=%lluus errors=%llu fallbacks=%llu",
            (unsigned long long)calls,(unsigned long long)bytes,(unsigned long long)slow,
            (unsigned long long)max_us,(unsigned long long)errors,(unsigned long long)fallbacks);
    }
}

static void positioned_slot_clear_nolock(int fd){
    if(fd<0||fd>=POSITIONED_FD_SLOTS)return;
    PositionedFdSlot *slot=&g_positioned_fd[fd];
    if(slot->valid)fsFileClose(&slot->file);
    free(slot->path);
    memset(slot,0,sizeof(*slot));
}

static u32 positioned_fs_mode(int host_flags){
    u32 mode=0;
    switch(host_flags&O_ACCMODE){
        case O_WRONLY: mode=FsOpenMode_Write|FsOpenMode_Append; break;
        case O_RDWR: mode=FsOpenMode_Read|FsOpenMode_Write|FsOpenMode_Append; break;
        case O_RDONLY:
        default: mode=FsOpenMode_Read; break;
    }
    return mode;
}

static int positioned_open_native(const char *resolved,int host_flags,FsFile *out){
    if(!resolved||!out)return -1;
    FsFileSystem *fs=NULL;
    char fs_path[FS_MAX_PATH];
    if(fsdevTranslatePath(resolved,&fs,fs_path)<0||!fs)return -1;
    Result rc=fsFsOpenFile(fs,fs_path,positioned_fs_mode(host_flags),out);
    return R_SUCCEEDED(rc)?0:-1;
}

static void positioned_register(int fd,const char *resolved,int host_flags){
    if(fd<0||fd>=POSITIONED_FD_SLOTS||!resolved)return;
    char *copy=strdup(resolved);
    mutexLock(&g_positioned_fd_lock);
    positioned_slot_clear_nolock(fd);
    PositionedFdSlot *slot=&g_positioned_fd[fd];
    slot->host_flags=host_flags;
    slot->path=copy;
    mutexUnlock(&g_positioned_fd_lock);
}

static int interesting_asset_path(const char *path){
    if(!path)return 0;
    const char *base=strrchr(path,'/');
    base=base?base+1:path;
    if(strstr(path,"/UnityCache/")||strstr(path,"/assets/bin/Data/"))return 1;
    if(strstr(base,"sharedassets")||strstr(base,"resources.assets")||strstr(base,"globalgamemanagers"))return 1;
    const char *dot=strrchr(base,'.');
    if(!dot)return 0;
    return !strcasecmp(dot,".assets")||!strcasecmp(dot,".bundle")||!strcasecmp(dot,".unity3d")||
           !strcasecmp(dot,".resS")||!strcasecmp(dot,".resource");
}

static int fopen_host_flags(const char *mode){
    if(!mode||!*mode)return O_RDONLY;
    int flags=0;
    if(mode[0]=='r')flags=strchr(mode,'+')?O_RDWR:O_RDONLY;
    else if(mode[0]=='w')flags=(strchr(mode,'+')?O_RDWR:O_WRONLY)|O_CREAT|O_TRUNC;
    else if(mode[0]=='a')flags=(strchr(mode,'+')?O_RDWR:O_WRONLY)|O_CREAT|O_APPEND;
    else flags=O_RDONLY;
    return flags;
}

static int positioned_copy_metadata(int fd,int *host_flags,char *path,size_t path_cap){
    if(fd<0||fd>=POSITIONED_FD_SLOTS)return 0;
    int present=0;
    mutexLock(&g_positioned_fd_lock);
    PositionedFdSlot *slot=&g_positioned_fd[fd];
    if(host_flags)*host_flags=slot->host_flags;
    if(path&&path_cap){
        if(slot->path)snprintf(path,path_cap,"%s",slot->path);
        else path[0]=0;
    }
    present=slot->path!=NULL;
    mutexUnlock(&g_positioned_fd_lock);
    return present;
}

static int positioned_snapshot(int fd,FsFile *out,int *host_flags,char *path,size_t path_cap){
    if(fd<0||fd>=POSITIONED_FD_SLOTS)return 0;
    int ok=0;
    mutexLock(&g_positioned_fd_lock);
    PositionedFdSlot *slot=&g_positioned_fd[fd];
    if(host_flags)*host_flags=slot->host_flags;
    if(path&&path_cap){
        if(slot->path)snprintf(path,path_cap,"%s",slot->path);
        else path[0]=0;
    }
    if(!slot->valid&&slot->path){
        FsFile native;
        if(positioned_open_native(slot->path,slot->host_flags,&native)==0){
            slot->file=native;
            slot->valid=1;
        }
    }
    if(slot->valid){
        if(out)*out=slot->file;
        ok=1;
    }
    mutexUnlock(&g_positioned_fd_lock);
    return ok;
}

static ssize_t positioned_native_read(FsFile *file,void *buf,size_t count,off_t offset){
    if(!count)return 0;
    u64 got=0;
    Result rc=fsFileRead(file,(s64)offset,buf,(u64)count,FsReadOption_None,&got);
    if(R_SUCCEEDED(rc))return (ssize_t)(got>(u64)SSIZE_MAX?(u64)SSIZE_MAX:got);
    if(R_VALUE(rc)!=0xD401u){errno=EIO;return -1;}

    unsigned char bounce[0x1000];
    size_t done=0;
    while(done<count){
        size_t want=count-done;
        if(want>sizeof(bounce))want=sizeof(bounce);
        got=0;
        rc=fsFileRead(file,(s64)offset+(s64)done,bounce,(u64)want,FsReadOption_None,&got);
        if(R_FAILED(rc)){
            if(done)return (ssize_t)done;
            errno=EIO;
            return -1;
        }
        if(got>(u64)want)got=(u64)want;
        memcpy((unsigned char*)buf+done,bounce,(size_t)got);
        done+=(size_t)got;
        if(got<(u64)want)break;
    }
    return (ssize_t)done;
}

static ssize_t positioned_native_write(FsFile *file,const void *buf,size_t count,off_t offset){
    if(!count)return 0;
    Result rc=fsFileWrite(file,(s64)offset,buf,(u64)count,FsWriteOption_None);
    if(R_SUCCEEDED(rc))return (ssize_t)count;
    if(R_VALUE(rc)!=0xD401u){errno=EIO;return -1;}

    unsigned char bounce[0x1000];
    size_t done=0;
    while(done<count){
        size_t want=count-done;
        if(want>sizeof(bounce))want=sizeof(bounce);
        memcpy(bounce,(const unsigned char*)buf+done,want);
        rc=fsFileWrite(file,(s64)offset+(s64)done,bounce,(u64)want,FsWriteOption_None);
        if(R_FAILED(rc)){
            if(done)return (ssize_t)done;
            errno=EIO;
            return -1;
        }
        done+=want;
    }
    return (ssize_t)done;
}

ssize_t file_pread(int fd,void *buf,size_t count,off_t offset){
    if(offset<0){errno=EINVAL;return -1;}
    if(!buf&&count){errno=EFAULT;return -1;}
    uint64_t start=trace_now_us();
    char path[320]={0};
    FsFile native;
    int host_flags=0;
    int tracked=positioned_snapshot(fd,&native,&host_flags,path,sizeof(path));
    ssize_t result=-1;
    int fallback=0;
    if(tracked&&(host_flags&O_ACCMODE)!=O_WRONLY){
        result=positioned_native_read(&native,buf,count,offset);
    }else{
        fallback=1;
        mutexLock(&g_positioned_fallback_lock);
        off_t old=lseek(fd,0,SEEK_CUR);
        if(old!=(off_t)-1&&lseek(fd,offset,SEEK_SET)!=(off_t)-1){
            result=read(fd,buf,count);
            int e=result<0?errno:0;
            if(lseek(fd,old,SEEK_SET)==(off_t)-1&&result>=0){result=-1;e=errno;}
            errno=e;
        }
        mutexUnlock(&g_positioned_fallback_lock);
    }
    uint64_t elapsed=trace_now_us()-start;
    positioned_trace_note("pread",fd,offset,count,result,elapsed,fallback,path);
    return result;
}

ssize_t file_pwrite(int fd,const void *buf,size_t count,off_t offset){
    if(offset<0){errno=EINVAL;return -1;}
    if(!buf&&count){errno=EFAULT;return -1;}
    uint64_t start=trace_now_us();
    char path[320]={0};
    FsFile native;
    int host_flags=0;
    int tracked=positioned_snapshot(fd,&native,&host_flags,path,sizeof(path));
    ssize_t result=-1;
    int fallback=0;
    if(tracked&&(host_flags&O_ACCMODE)!=O_RDONLY){
        off_t write_offset=offset;
        if(host_flags&O_APPEND){
            s64 end=0;
            if(R_SUCCEEDED(fsFileGetSize(&native,&end))&&end>=0)write_offset=(off_t)end;
        }
        result=positioned_native_write(&native,buf,count,write_offset);
    }else{
        fallback=1;
        mutexLock(&g_positioned_fallback_lock);
        off_t old=lseek(fd,0,SEEK_CUR);
        if(old!=(off_t)-1&&lseek(fd,offset,SEEK_SET)!=(off_t)-1){
            result=write(fd,buf,count);
            int e=result<0?errno:0;
            if(lseek(fd,old,SEEK_SET)==(off_t)-1&&result>=0){result=-1;e=errno;}
            errno=e;
        }
        mutexUnlock(&g_positioned_fallback_lock);
    }
    uint64_t elapsed=trace_now_us()-start;
    positioned_trace_note("pwrite",fd,offset,count,result,elapsed,fallback,path);
    return result;
}


typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} AndroidDirent64;

_Static_assert(offsetof(AndroidDirent64, d_type) == 0x12, "Android dirent d_type offset changed");
_Static_assert(offsetof(AndroidDirent64, d_name) == 0x13, "Android dirent d_name offset changed");
_Static_assert(sizeof(AndroidDirent64) == 280, "Android aarch64 struct dirent size changed");

#define GUEST_DIR_MAGIC 0x44524942u

typedef struct {
    uint32_t magic;
    DIR *host;
    uint64_t seq;
    AndroidDirent64 guest;
} GuestDir;

static uint8_t android_dirent_type(const struct dirent *h) {
    if (!h) return 0;
#if defined(DT_UNKNOWN)
    switch (h->d_type) {
        case DT_UNKNOWN: return 0;
#ifdef DT_FIFO
        case DT_FIFO: return 1;
#endif
#ifdef DT_CHR
        case DT_CHR: return 2;
#endif
#ifdef DT_DIR
        case DT_DIR: return 4;
#endif
#ifdef DT_BLK
        case DT_BLK: return 6;
#endif
#ifdef DT_REG
        case DT_REG: return 8;
#endif
#ifdef DT_LNK
        case DT_LNK: return 10;
#endif
#ifdef DT_SOCK
        case DT_SOCK: return 12;
#endif
#ifdef DT_WHT
        case DT_WHT: return 14;
#endif
        default: return 0;
    }
#else
    (void)h;
    return 0;
#endif
}

static int write_virtual_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int ensure_directory(const char *path) {
    if (mkdir(path, 0777) == 0 || errno == EEXIST) return 0;
    return -1;
}

static int format_cpu_list(char *out, size_t cap, u64 mask) {
    size_t w = 0;
    int first = 1;
    for (unsigned i = 0; i < 32;) {
        if (!(mask & (1ull << i))) { ++i; continue; }
        unsigned start = i, end = i;
        while (end + 1 < 32 && (mask & (1ull << (end + 1)))) ++end;
        int n;
        if (start == end)
            n = snprintf(out + w, cap - w, "%s%u", first ? "" : ",", start);
        else
            n = snprintf(out + w, cap - w, "%s%u-%u", first ? "" : ",", start, end);
        if (n < 0 || (size_t)n >= cap - w) return -1;
        w += (size_t)n;
        first = 0;
        i = end + 1;
    }
    if (first || w + 2 > cap) return -1;
    out[w++] = '\n';
    out[w] = '\0';
    return 0;
}

static int prepare_virtual_cpu_sysfs(u64 core_mask) {
    if (!core_mask) return -1;
    if (ensure_directory(DATA_ROOT "/sys") < 0 ||
        ensure_directory(DATA_ROOT "/sys/devices") < 0 ||
        ensure_directory(DATA_ROOT "/sys/devices/system") < 0 ||
        ensure_directory(DATA_ROOT "/sys/devices/system/cpu") < 0)
        return -1;

    char topo[128];
    if (format_cpu_list(topo, sizeof(topo), core_mask) < 0) return -1;
    if (write_virtual_file(DATA_ROOT "/sys/devices/system/cpu/present", topo) < 0 ||
        write_virtual_file(DATA_ROOT "/sys/devices/system/cpu/possible", topo) < 0)
        return -1;

    for (unsigned i = 0; i < 32; ++i) {
        if (!(core_mask & (1ull << i))) continue;
        char dir[PATH_MAX];
        char path[PATH_MAX];
        int n = snprintf(dir, sizeof(dir),
                         DATA_ROOT "/sys/devices/system/cpu/cpu%u", i);
        if (n < 0 || (size_t)n >= sizeof(dir)) return -1;
        if (ensure_directory(dir) < 0) return -1;

        n = snprintf(path, sizeof(path),
                     DATA_ROOT "/sys/devices/system/cpu/cpu%u/cpu_capacity", i);
        if (n < 0 || (size_t)n >= sizeof(path)) return -1;
        if (write_virtual_file(path, "1024\n") < 0) return -1;
    }

    return 0;
}

int file_prepare_virtual_system_files(void) {
    if (mkdir(DATA_ROOT "/proc", 0777) < 0 && errno != EEXIST) return -1;

    u64 core_mask = 0;
    u64 total = 0;
    u64 used = 0;
    Result rc_core = svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    Result rc_total = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    Result rc_used = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

    if (R_SUCCEEDED(rc_core) && core_mask) {
        char cpu[1024];
        size_t w = 0;
        for (unsigned i = 0; i < 64; ++i) {
            if (!(core_mask & (1ull << i))) continue;
            int n = snprintf(cpu + w, sizeof(cpu) - w, "processor\t: %u\n\n", i);
            if (n < 0 || (size_t)n >= sizeof(cpu) - w) return -1;
            w += (size_t)n;
        }
        if (write_virtual_file(DATA_ROOT "/proc/cpuinfo", cpu) < 0) return -1;
        if (prepare_virtual_cpu_sysfs(core_mask) < 0) return -1;
    } else {
        remove(DATA_ROOT "/proc/cpuinfo");
    }

    if (R_SUCCEEDED(rc_total) && R_SUCCEEDED(rc_used) && total > 0 && used <= total) {
        u64 free_bytes = total - used;
        char mem[512];
        int n = snprintf(mem, sizeof(mem),
                         "MemTotal:       %llu kB\n"
                         "MemFree:        %llu kB\n"
                         "MemAvailable:   %llu kB\n",
                         (unsigned long long)(total >> 10),
                         (unsigned long long)(free_bytes >> 10),
                         (unsigned long long)(free_bytes >> 10));
        if (n < 0 || (size_t)n >= sizeof(mem)) return -1;
        if (write_virtual_file(DATA_ROOT "/proc/meminfo", mem) < 0) return -1;
    } else {
        remove(DATA_ROOT "/proc/meminfo");
    }
    return 0;
}

static const char *strip_runtime_prefixed_android_path(const char *p) {
    if (!p) return p;

    const size_t root_n = strlen(DATA_ROOT);
    if (strncmp(p, DATA_ROOT, root_n) != 0 ||
        (p[root_n] != '/' && p[root_n] != '\0'))
        return p;

    const char *nested = p + root_n;
    const size_t data_n = strlen(ANDROID_DATA_DIR);
    const size_t ext_n = strlen(ANDROID_EXTERNAL_FILES_DIR);

    if ((!strncmp(nested, ANDROID_DATA_DIR, data_n) &&
         (nested[data_n] == '/' || nested[data_n] == '\0')) ||
        (!strncmp(nested, ANDROID_EXTERNAL_FILES_DIR, ext_n) &&
         (nested[ext_n] == '/' || nested[ext_n] == '\0')))
        return nested;

    return p;
}

int file_translate(const char *in, char *out, size_t cap) {
    if (!in || !out || !cap) return 0;

    if (!strcmp(in, "/proc/cpuinfo")) {
        snprintf(out, cap, DATA_ROOT "/proc/cpuinfo");
        return 1;
    }
    if (!strcmp(in, "/proc/meminfo")) {
        snprintf(out, cap, DATA_ROOT "/proc/meminfo");
        return 1;
    }
    if (!strcmp(in, "/sys/devices/system/cpu/present")) {
        snprintf(out, cap, DATA_ROOT "/sys/devices/system/cpu/present");
        return 1;
    }
    if (!strcmp(in, "/sys/devices/system/cpu/possible")) {
        snprintf(out, cap, DATA_ROOT "/sys/devices/system/cpu/possible");
        return 1;
    }
    if (!strncmp(in, "/sys/devices/system/cpu/cpu", 27) && strstr(in, "/cpu_capacity")) {
        snprintf(out, cap, DATA_ROOT "%s", in);
        return 1;
    }

    if (!strcmp(in, INSTALL_APK)) {
        snprintf(out, cap, "%s", DATA_ROOT);
        return 1;
    }

    const char *apk_assets = strstr(in, ".apk/assets/");
    size_t apk_marker_len = sizeof(".apk/assets/") - 1;
    if (!apk_assets) {
        apk_assets = strstr(in, ".apk!/assets/");
        apk_marker_len = sizeof(".apk!/assets/") - 1;
    }
    if (apk_assets) {
        snprintf(out, cap, DATA_ROOT "/assets/%s", apk_assets + apk_marker_len);
        return 1;
    }

    if (!strncmp(in, "/assets/", 8)) {
        snprintf(out, cap, DATA_ROOT "/assets/%s", in + 8);
        return 1;
    }

    if (!strncmp(in, "assets/", 7)) {
        snprintf(out, cap, DATA_ROOT "/%s", in);
        return 1;
    }

    const char *android_in = strip_runtime_prefixed_android_path(in);
    if (android_in != in) {
    }

    const char *pfx = ANDROID_DATA_DIR;
    size_t n = strlen(pfx);
    if (!strncmp(android_in, pfx, n) &&
        (android_in[n] == '/' || android_in[n] == '\0')) {
        snprintf(out, cap, DATA_ROOT "%s", android_in + n);
        return 1;
    }
    pfx = ANDROID_EXTERNAL_FILES_DIR;
    n = strlen(pfx);
    if (!strncmp(android_in, pfx, n) &&
        (android_in[n] == '/' || android_in[n] == '\0')) {
        snprintf(out, cap, DATA_ROOT "/files%s", android_in + n);
        return 1;
    }

    const size_t root_n = strlen(DATA_ROOT);
    const char *host = in;
    while (!strncmp(host, DATA_ROOT "/", root_n + 1)) {
        const char *nested = host + root_n + 1;
        if (strncmp(nested, DATA_ROOT, root_n) ||
            (nested[root_n] != '/' && nested[root_n] != '\0')) break;
        host = nested;
    }
    if (host != in) {
        snprintf(out, cap, "%s", host);
        return 1;
    }

    snprintf(out, cap, "%s", in);
    return 0;
}

static char *translate_path_alloc(const char *p) {
    if (!p) { errno = EFAULT; return NULL; }
    char *b = (char *)malloc(PATH_MAX);
    if (!b) { errno = ENOMEM; return NULL; }
    file_translate(p, b, PATH_MAX);
    return b;
}

static int find_packaged_local_fallback(const char *guest, char *out, size_t cap) {
    static const char guest_pfx[] = ANDROID_FILES_DIR "/downloaded/";
    guest = strip_runtime_prefixed_android_path(guest);
    if (!guest || strncmp(guest, guest_pfx, sizeof(guest_pfx) - 1) != 0) return 0;

    const char *rel = guest + sizeof(guest_pfx) - 1;
    if (!*rel || *rel == '/' || strchr(rel, '\\')) return 0;

    const char *seg = rel;
    while (*seg) {
        const char *slash = strchr(seg, '/');
        size_t n = slash ? (size_t)(slash - seg) : strlen(seg);
        if ((n == 1 && seg[0] == '.') || (n == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
        if (!slash) break;
        seg = slash + 1;
        if (!*seg) return 0;
    }

    int n = snprintf(out, cap, DATA_ROOT "/assets/local/%s", rel);
    return n > 0 && (size_t)n < cap;
}

static char *translate_read_path_alloc(const char *p) {
    char *primary = translate_path_alloc(p);
    if (!primary) return NULL;

    int saved_errno = errno;
    struct stat st;
    int primary_rc = stat(primary, &st);
    if (primary_rc == 0) {
        errno = saved_errno;
        return primary;
    }

    char *local = (char *)malloc(PATH_MAX);
    if (!local) {
        errno = saved_errno;
        return primary;
    }
    if (!find_packaged_local_fallback(p, local, PATH_MAX) || stat(local, &st) != 0 || !S_ISREG(st.st_mode)) {
        free(local);
        errno = saved_errno;
        return primary;
    }

    free(primary);
    errno = saved_errno;
    return local;
}

static void free_preserving_errno(char *p, int saved_errno) {
    free(p);
    errno = saved_errno;
}

static void stat_to_android(const struct stat *h, AndroidStat *a) {
    memset(a, 0, sizeof(*a));
    a->st_dev = (uint64_t)h->st_dev;
    a->st_ino = (uint64_t)h->st_ino;
    a->st_mode = (uint32_t)h->st_mode;
    a->st_nlink = (uint32_t)h->st_nlink;
    a->st_uid = (uint32_t)h->st_uid;
    a->st_gid = (uint32_t)h->st_gid;
    a->st_rdev = (uint64_t)h->st_rdev;
    a->st_size = (int64_t)h->st_size;
    a->st_blksize = 4096;
    a->st_blocks = ((int64_t)h->st_size + 511) / 512;
    a->ast_atim.tv_sec = (int64_t)h->st_atime;
    a->ast_mtim.tv_sec = (int64_t)h->st_mtime;
    a->ast_ctim.tv_sec = (int64_t)h->st_ctime;
}

#define UNITYCACHE_LOCK_SLOTS 16
#define UNITYCACHE_DATA_SLOTS 16
typedef struct {
    int fd;
    char *path;
} UnityCacheFileSlot;

static Mutex g_unitycache_lock_mutex;
static UnityCacheFileSlot g_unitycache_lock_slots[UNITYCACHE_LOCK_SLOTS];
static UnityCacheFileSlot g_unitycache_data_slots[UNITYCACHE_DATA_SLOTS];

static int is_unitycache_named_file(const char *p, const char *name) {
    static const char pfx[] = DATA_ROOT "/files/UnityCache/";
    if (!p || !name || strncmp(p, pfx, sizeof(pfx)-1) != 0) return 0;
    const char *base = strrchr(p, '/');
    return base && strcmp(base + 1, name) == 0;
}

static int is_unitycache_lock_file(const char *p) {
    return is_unitycache_named_file(p, "__lock");
}

static int is_unitycache_data_file(const char *p) {
    return is_unitycache_named_file(p, "__data");
}

static void unitycache_lock_forget_fd_nolock(int fd) {
    for (int i = 0; i < UNITYCACHE_LOCK_SLOTS; ++i) {
        if (g_unitycache_lock_slots[i].fd == fd) {
            free(g_unitycache_lock_slots[i].path);
            g_unitycache_lock_slots[i].path = NULL;
            g_unitycache_lock_slots[i].fd = -1;
        }
    }
}

static void unitycache_lock_register_nolock(int fd, const char *path) {
    if (fd < 0 || !path) return;
    unitycache_lock_forget_fd_nolock(fd);
    for (int i = 0; i < UNITYCACHE_LOCK_SLOTS; ++i) {
        if (!g_unitycache_lock_slots[i].path) {
            char *copy = strdup(path);
            if (!copy) return;
            g_unitycache_lock_slots[i].fd = fd;
            g_unitycache_lock_slots[i].path = copy;
            return;
        }
    }
}

static int unitycache_lock_find_nolock(const char *path) {
    for (int i = 0; i < UNITYCACHE_LOCK_SLOTS; ++i) {
        UnityCacheFileSlot *slot = &g_unitycache_lock_slots[i];
        if (!slot->path || strcmp(slot->path, path) != 0) continue;
        struct stat st;
        if (fstat(slot->fd, &st) == 0) return slot->fd;
        free(slot->path);
        slot->path = NULL;
        slot->fd = -1;
    }
    return -1;
}

static void unitycache_data_forget_fd_nolock(int fd) {
    for (int i = 0; i < UNITYCACHE_DATA_SLOTS; ++i) {
        if (g_unitycache_data_slots[i].fd == fd) {
            free(g_unitycache_data_slots[i].path);
            g_unitycache_data_slots[i].path = NULL;
            g_unitycache_data_slots[i].fd = -1;
        }
    }
}

static void unitycache_data_register_nolock(int fd, const char *path) {
    if (fd < 0 || !path) return;
    unitycache_data_forget_fd_nolock(fd);
    for (int i = 0; i < UNITYCACHE_DATA_SLOTS; ++i) {
        if (!g_unitycache_data_slots[i].path) {
            char *copy = strdup(path);
            if (!copy) return;
            g_unitycache_data_slots[i].fd = fd;
            g_unitycache_data_slots[i].path = copy;
            return;
        }
    }
}

static int unitycache_data_find_nolock(const char *path) {
    for (int i = 0; i < UNITYCACHE_DATA_SLOTS; ++i) {
        UnityCacheFileSlot *slot = &g_unitycache_data_slots[i];
        if (!slot->path || strcmp(slot->path, path) != 0) continue;
        struct stat st;
        if (fstat(slot->fd, &st) == 0) return slot->fd;
        free(slot->path);
        slot->path = NULL;
        slot->fd = -1;
    }
    return -1;
}

static int unitycache_data_lookup_fd_nolock(const char *path) {
    for (int i = 0; i < UNITYCACHE_DATA_SLOTS; ++i) {
        UnityCacheFileSlot *slot = &g_unitycache_data_slots[i];
        if (slot->path && strcmp(slot->path, path) == 0) return slot->fd;
    }
    return -1;
}

void file_note_fd_closed(int fd) {
    if(fd>=0&&fd<POSITIONED_FD_SLOTS){
        mutexLock(&g_positioned_fd_lock);
        positioned_slot_clear_nolock(fd);
        mutexUnlock(&g_positioned_fd_lock);
    }
    mutexLock(&g_unitycache_lock_mutex);
    unitycache_lock_forget_fd_nolock(fd);
    unitycache_data_forget_fd_nolock(fd);
    mutexUnlock(&g_unitycache_lock_mutex);
}

void file_note_fd_duplicated(int oldfd, int newfd) {
    if (oldfd < 0 || newfd < 0 || oldfd == newfd) return;
    char positioned_path[PATH_MAX]={0};
    int positioned_flags=0;
    (void)positioned_copy_metadata(oldfd,&positioned_flags,positioned_path,sizeof(positioned_path));
    if(positioned_path[0])positioned_register(newfd,positioned_path,positioned_flags);
    else if(newfd<POSITIONED_FD_SLOTS){
        mutexLock(&g_positioned_fd_lock);
        positioned_slot_clear_nolock(newfd);
        mutexUnlock(&g_positioned_fd_lock);
    }
    mutexLock(&g_unitycache_lock_mutex);
    unitycache_lock_forget_fd_nolock(newfd);
    unitycache_data_forget_fd_nolock(newfd);
    for (int i = 0; i < UNITYCACHE_LOCK_SLOTS; ++i) {
        UnityCacheFileSlot *slot = &g_unitycache_lock_slots[i];
        if (slot->fd == oldfd && slot->path) {
            unitycache_lock_register_nolock(newfd, slot->path);
            break;
        }
    }
    for (int i = 0; i < UNITYCACHE_DATA_SLOTS; ++i) {
        UnityCacheFileSlot *slot = &g_unitycache_data_slots[i];
        if (slot->fd == oldfd && slot->path) {
            unitycache_data_register_nolock(newfd, slot->path);
            break;
        }
    }
    mutexUnlock(&g_unitycache_lock_mutex);
}

static int ensure_unity_cache_parents(const char *translated) {
    static const char pfx[] = DATA_ROOT "/files/UnityCache/";
    const size_t pfx_n = sizeof(pfx) - 1;
    if (!translated || strncmp(translated, pfx, pfx_n) != 0) return 0;

    char *tmp = strdup(translated);
    if (!tmp) { errno = ENOMEM; return -1; }

    char *scan = tmp + strlen(DATA_ROOT) + 1;
    for (char *slash = strchr(scan, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (mkdir(tmp, 0777) < 0 && errno != EEXIST) {
            int e = errno;
            free(tmp);
            errno = e;
            return -1;
        }
        *slash = '/';
    }

    free(tmp);
    return 0;
}

int file_open(const char *p, int flags, ...) {
    int out = flags & 3;

    if (flags & 0100) out |= O_CREAT;
    if (flags & 0200) out |= O_EXCL;
    if (flags & 01000) out |= O_TRUNC;
    if (flags & 02000) out |= O_APPEND;
#ifdef O_NONBLOCK
    if (flags & 04000) out |= O_NONBLOCK;
#endif
    mode_t m = 0666;
    if (flags & 0100) {
        va_list a;
        va_start(a, flags);
        m = (mode_t)va_arg(a, int);
        va_end(a);
    }
    int guest_accmode = flags & 3;
    char *resolved = (guest_accmode == 0 && !(flags & 0100)) ? translate_read_path_alloc(p) : translate_path_alloc(p);
    if (!resolved) return -1;
    const int uc_lock = is_unitycache_lock_file(resolved);
    const int uc_data = is_unitycache_data_file(resolved);
    if (uc_lock) {

        out = (out & ~3) | O_RDWR;
    }
    if ((flags & 0100) && ensure_unity_cache_parents(resolved) < 0) {
        int e = errno;
        free_preserving_errno(resolved, e);
        return -1;
    }
    int fd = open(resolved, out, m);
    int e = fd < 0 ? errno : 0;
    unsigned nxrc = fd < 0 ? (unsigned)fsdevGetLastResult() : 0;

    if (uc_lock) {
        mutexLock(&g_unitycache_lock_mutex);
        if (fd >= 0) {
            unitycache_lock_register_nolock(fd, resolved);
        } else if (nxrc == 0xE02u) {
            int existing = unitycache_lock_find_nolock(resolved);
            if (existing >= 0) {
                int shared = dup(existing);
                if (shared >= 0) {
                    int compat_e = 0;
                    if ((flags & 01000) && ftruncate(shared, 0) < 0)
                        compat_e = errno;
                    if (!compat_e) {
                        fd = shared;
                        e = 0;
                        nxrc = 0;
                        unitycache_lock_register_nolock(fd, resolved);
                    } else {
                        close(shared);
                        e = compat_e;
                    }
                } else {
                    e = errno;
                }
            }
        }
        mutexUnlock(&g_unitycache_lock_mutex);
    }

    if (uc_data && fd >= 0) {
        mutexLock(&g_unitycache_lock_mutex);
        unitycache_data_register_nolock(fd, resolved);
        mutexUnlock(&g_unitycache_lock_mutex);
    }

    if(fd>=0){
        positioned_register(fd,resolved,out);
        if(interesting_asset_path(resolved))trace_log_printf("OPEN","fd=%d flags=0x%x path=%s",fd,out,resolved);
    }
    free_preserving_errno(resolved, e);
    return fd;
}

FILE *file_fopen(const char *p, const char *m) {
    int read_only = m && m[0] == 'r' && strchr(m, '+') == NULL;
    char *resolved = read_only ? translate_read_path_alloc(p) : translate_path_alloc(p);
    if (!resolved) return NULL;
    if (!read_only && ensure_unity_cache_parents(resolved) < 0) {
        int e = errno;
        free_preserving_errno(resolved, e);
        return NULL;
    }
    FILE *fp = fopen(resolved, m);
    int e = fp ? 0 : errno;
    if(fp){
        int fd=fileno(fp);
        if(fd>=0)positioned_register(fd,resolved,fopen_host_flags(m));
        if(interesting_asset_path(resolved))trace_log_printf("OPEN","FILE fd=%d mode=%s path=%s",fd,m?m:"?",resolved);
    }
    free_preserving_errno(resolved, e);
    return fp;
}

int file_fclose(FILE *fp){
    if(!fp){errno=EINVAL;return EOF;}
    int fd=fileno(fp);
    int rc=fclose(fp);
    if(fd>=0)file_note_fd_closed(fd);
    return rc;
}

int file_stat(const char *p, void *guest) {
    struct stat h;
    char *resolved = translate_read_path_alloc(p);
    if (!resolved) return -1;
    int rc = stat(resolved, &h);
    int e = rc ? errno : 0;

    if (rc < 0 && e == EIO && is_unitycache_data_file(resolved)) {
        int source_fd = -1;
        int probe_fd = -1;

        mutexLock(&g_unitycache_lock_mutex);
        source_fd = unitycache_data_lookup_fd_nolock(resolved);
        if (source_fd >= 0) {
            probe_fd = dup(source_fd);
        }
        mutexUnlock(&g_unitycache_lock_mutex);

        if (probe_fd >= 0) {
            int frc = fstat(probe_fd, &h);
            int fe = frc < 0 ? errno : 0;
            (void)close(probe_fd);
            if (frc == 0) {
                rc = 0;
                e = 0;
            } else {
                rc = frc;
                e = fe;
            }
        }
    }

    if (!rc && guest) stat_to_android(&h, (AndroidStat *)guest);
    free_preserving_errno(resolved, e);
    return rc;
}

int file_lstat(const char *p, void *guest) {
    struct stat h;
    char *resolved = translate_read_path_alloc(p);
    if (!resolved) return -1;
    int rc = lstat(resolved, &h);
    int e = rc ? errno : 0;
    if (!rc && guest) stat_to_android(&h, (AndroidStat *)guest);
    free_preserving_errno(resolved, e);
    return rc;
}

int file_truncate(const char *p, off_t length) {
    if (!p || length < 0) { errno = EINVAL; return -1; }
    char *resolved = translate_path_alloc(p);
    if (!resolved) return -1;

    int rc = -1;
    int e = 0;

    if (is_unitycache_data_file(resolved)) {
        mutexLock(&g_unitycache_lock_mutex);
        int existing = unitycache_data_find_nolock(resolved);
        if (existing >= 0) {
            rc = ftruncate(existing, length);
            e = rc < 0 ? errno : 0;
        }
        mutexUnlock(&g_unitycache_lock_mutex);
        if (existing >= 0) {
            free_preserving_errno(resolved, e);
            return rc;
        }
    }

    rc = truncate(resolved, length);
    e = rc < 0 ? errno : 0;
    free_preserving_errno(resolved, e);
    return rc;
}

int file_fstat(int fd, void *guest) {
    struct stat h;
    int rc = fstat(fd, &h);
    if (!rc && guest) stat_to_android(&h, (AndroidStat *)guest);
    return rc;
}

int file_statfs(const char *p, void *guest) {
    if (!guest) { errno = EFAULT; return -1; }
    char *r=translate_path_alloc(p);
    if(!r)return -1;
    struct statvfs h;
    int rc=statvfs(r,&h);
    int e=rc?errno:0;
    if(!rc){
        AndroidStatfs *s=(AndroidStatfs*)guest;
        memset(s,0,sizeof(*s));
        s->f_bsize=(uint64_t)h.f_bsize;
        s->f_frsize=(uint64_t)(h.f_frsize?h.f_frsize:h.f_bsize);
        s->f_blocks=(uint64_t)h.f_blocks;
        s->f_bfree=(uint64_t)h.f_bfree;
        s->f_bavail=(uint64_t)h.f_bavail;
        s->f_files=(uint64_t)h.f_files;
        s->f_ffree=(uint64_t)h.f_ffree;
        s->f_namelen=(uint64_t)h.f_namemax;
    }
    free_preserving_errno(r,e);
    return rc;
}

int file_access(const char *p, int m) {
    char *r = (m & W_OK) ? translate_path_alloc(p) : translate_read_path_alloc(p);
    if (!r) return -1;
    int rc = access(r, m);
    int e = rc ? errno : 0;
    free_preserving_errno(r, e);
    return rc;
}

DIR *file_opendir(const char *p) {
    char *r = translate_path_alloc(p);
    if (!r) return NULL;
    DIR *host = opendir(r);
    int e = host ? 0 : errno;
    GuestDir *g = NULL;

    if (host) {
        g = (GuestDir *)calloc(1, sizeof(*g));
        if (!g) {
            e = ENOMEM;
            closedir(host);
            host = NULL;
        } else {
            g->magic = GUEST_DIR_MAGIC;
            g->host = host;
        }
    }

    free_preserving_errno(r, e);
    return (DIR *)g;
}

void *file_readdir(DIR *opaque) {
    GuestDir *g = (GuestDir *)opaque;
    if (!g || g->magic != GUEST_DIR_MAGIC || !g->host) {
        errno = EBADF;
        return NULL;
    }

    errno = 0;
    struct dirent *h = readdir(g->host);
    int e = errno;
    if (!h) {
        errno = e;
        return NULL;
    }

    AndroidDirent64 *a = &g->guest;
    memset(a, 0, sizeof(*a));
    a->d_off = (int64_t)++g->seq;
    a->d_type = android_dirent_type(h);
    size_t n = strnlen(h->d_name, sizeof(a->d_name) - 1);
    memcpy(a->d_name, h->d_name, n);
    a->d_name[n] = '\0';
    size_t reclen = offsetof(AndroidDirent64, d_name) + n + 1;
    reclen = (reclen + 7u) & ~7u;
    a->d_reclen = (uint16_t)reclen;

    errno = e;
    return a;
}

int file_closedir(DIR *opaque) {
    GuestDir *g = (GuestDir *)opaque;
    if (!g || g->magic != GUEST_DIR_MAGIC || !g->host) {
        errno = EBADF;
        return -1;
    }

    DIR *host = g->host;
    g->magic = 0;
    g->host = NULL;

    int rc = closedir(host);
    int e = rc ? errno : 0;
    free(g);
    errno = e;
    return rc;
}

int file_mkdir(const char *p, mode_t m) {
    char *r = translate_path_alloc(p);
    if (!r) return -1;
    if (ensure_unity_cache_parents(r) < 0) {
        int e = errno;
        free_preserving_errno(r, e);
        return -1;
    }
    int rc = mkdir(r, m);
    int e = rc ? errno : 0;
    free_preserving_errno(r, e);
    return rc;
}

int file_unlink(const char *p) {
    char *r = translate_path_alloc(p);
    if (!r) return -1;
    int rc = unlink(r);
    int e = rc ? errno : 0;
    free_preserving_errno(r, e);
    return rc;
}

int file_remove(const char *p) {
    char *r = translate_path_alloc(p);
    if (!r) return -1;
    int rc = remove(r);
    int e = rc ? errno : 0;
    free_preserving_errno(r, e);
    return rc;
}

int file_rename(const char *a, const char *b0) {
    char *a1 = translate_path_alloc(a);
    if (!a1) return -1;
    char *b1 = translate_path_alloc(b0);
    if (!b1) {
        int e = errno;
        free(a1);
        errno = e;
        return -1;
    }

    int rc = rename(a1, b1);
    int e = rc ? errno : 0;
    free(a1);
    free_preserving_errno(b1, e);
    return rc;
}

char *file_realpath(const char *p, char *r) {
    char *t = translate_read_path_alloc(p);
    if (!t) return NULL;
    char *out = realpath(t, r);
    int e = out ? 0 : errno;
    free_preserving_errno(t, e);
    return out;
}
