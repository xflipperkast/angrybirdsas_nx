#include "installer.h"
#include "config.h"
#include "credits_patch.h"
#include <switch.h>
#include <zlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdarg.h>

#define ZIP_NAME_MAX 768

#define INSTALL_REQ_MANIFEST (1u << 0)
#define INSTALL_REQ_MAIN     (1u << 1)
#define INSTALL_REQ_UNITY    (1u << 2)
#define INSTALL_REQ_IL2CPP   (1u << 3)
#define INSTALL_REQ_METADATA (1u << 4)
#define INSTALL_REQ_DATA     (1u << 5)
#define INSTALL_REQ_RESOURCES (1u << 6)
#define INSTALL_REQ_GUID      (1u << 7)
#define INSTALL_REQ_MSCORLIB  (1u << 8)
#define INSTALL_REQ_SYSTEM_DATA (1u << 9)
#define INSTALL_REQ_SYSTEM_DRAWING (1u << 10)
#define INSTALL_REQ_ENGLISH   (1u << 11)

typedef struct { FILE *fp; uint16_t entries; uint32_t cd_offset; } ZipArchive;
typedef struct {
    char name[ZIP_NAME_MAX];
    uint16_t flags, method;
    uint32_t crc32, csize, usize, local_offset;
} ZipEntry;
typedef int (*ZipVisitor)(ZipArchive *, const ZipEntry *, void *);

static char g_error[1024];
static int g_console;
static char g_manifest_package[128];
static char g_manifest_version_name[128];
static int g_manifest_version_code=-1;
static int g_manifest_min_sdk=-1;
static int g_manifest_target_sdk=-1;

const char *installer_package_name(void){return g_manifest_package[0]?g_manifest_package:NULL;}
const char *installer_version_name(void){return g_manifest_version_name[0]?g_manifest_version_name:NULL;}
int installer_version_code(void){return g_manifest_version_code;}
int installer_min_sdk(void){return g_manifest_min_sdk;}
int installer_target_sdk(void){return g_manifest_target_sdk;}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void installer_set_error(const char *s) { snprintf(g_error, sizeof(g_error), "%s", s ? s : "Installer error"); }
static void installer_set_errorf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
}
const char *installer_last_error(void) { return g_error[0] ? g_error : "Unknown installer error"; }

static int file_exists_nonempty(const char *p) { struct stat st; return stat(p, &st) == 0 && st.st_size > 0; }
static int is_safe_relative_path(const char *p) { return p && *p && p[0] != '/' && !strstr(p, "..") && !strchr(p, '\\'); }
static int has_trailing_slash(const char *p) { size_t n = strlen(p); return n && p[n-1] == '/'; }

static unsigned long long bytes_to_mib_ceil(uint64_t bytes) {
    return (unsigned long long)((bytes + ((1ull << 20) - 1ull)) >> 20);
}

static int ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        installer_set_errorf("Install path exists but is not a directory:\n%s", path);
        return -1;
    }
    if (mkdir(path, 0777) == 0) return 0;
    installer_set_errorf("Could not create install directory:\n%s\nerrno=%d (%s)", path, errno, strerror(errno));
    return -1;
}

static int verify_install_location_writable(void) {
    if (ensure_directory(GAME_HOME) < 0 || ensure_directory(DATA_ROOT) < 0) return -1;

    const char *probe = DATA_ROOT "/.installer_write_test.tmp";
    FILE *f = fopen(probe, "wb");
    if (!f) {
        installer_set_errorf("The SD card/runtime directory is not writable:\n%s\nerrno=%d (%s)",
            DATA_ROOT, errno, strerror(errno));
        return -1;
    }
    int saved_errno = 0;
    int ok = fputc(0x5a, f) != EOF && fflush(f) == 0;
    if (!ok) saved_errno = errno;
    int close_rc = fclose(f);
    if (close_rc != 0) { ok = 0; if (!saved_errno) saved_errno = errno; }
    if (remove(probe) != 0 && errno != ENOENT) { ok = 0; if (!saved_errno) saved_errno = errno; }
    if (!ok) {
        if (!saved_errno) saved_errno = EIO;
        installer_set_errorf("Write test failed in the runtime directory. The SD card may be full, read-only, or damaged.\nerrno=%d (%s)",
            saved_errno, strerror(saved_errno));
        return -1;
    }
    return 0;
}

static int query_sd_space(uint64_t *free_bytes, uint64_t *total_bytes) {
    if (free_bytes) *free_bytes = 0;
    if (total_bytes) *total_bytes = 0;
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) {
        installer_set_error("The SD card filesystem is not mounted (sdmc unavailable).");
        return -1;
    }
    s64 free_space = 0;
    s64 total_space = 0;
    Result rc = fsFsGetFreeSpace(fs, "/", &free_space);
    if (R_FAILED(rc) || free_space < 0) {
        installer_set_errorf("Could not query free SD-card space (Result 0x%08x).", (unsigned)rc);
        return -1;
    }
    rc = fsFsGetTotalSpace(fs, "/", &total_space);
    if (R_FAILED(rc) || total_space < 0) {
        installer_set_errorf("Could not query total SD-card space (Result 0x%08x).", (unsigned)rc);
        return -1;
    }
    if (free_bytes) *free_bytes = (uint64_t)free_space;
    if (total_bytes) *total_bytes = (uint64_t)total_space;
    return 0;
}

static int create_parent_directories(const char *filename) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s", filename);
    size_t root = strlen(DATA_ROOT);
    if (strncmp(p, DATA_ROOT, root) != 0 || (p[root] != '/' && p[root] != 0)) {
        installer_set_error("Refusing to extract outside the runtime directory.");
        return -1;
    }

    for (char *q = p + root + (p[root] == '/' ? 1 : 0); *q; ++q) {
        if (*q != '/') continue;
        *q = 0;
        if (mkdir(p, 0777) < 0 && errno != EEXIST) {
            installer_set_errorf("Could not create runtime directory:\n%s\nerrno=%d (%s)", p, errno, strerror(errno));
            return -1;
        }
        *q = '/';
    }
    return 0;
}

static void installer_progress_update(const char *name, uint64_t done, uint64_t total) {
    if (!g_console) { consoleInit(NULL); g_console = 1; }
    unsigned pct = total ? (unsigned)((done * 100u) / total) : 0; if (pct > 100) pct = 100;
    unsigned fill = pct * 32 / 100;
    consoleClear();
    printf("Angry Bird Epic All Stars - first run\n\n");
    printf("Extracting game.apk\n%s\n\n[", name ? name : "preparing");
    for (unsigned i=0;i<32;i++) putchar(i < fill ? '#' : '-');
    printf("] %3u%%\n\nDo not close the app during extraction.\n", pct);
    consoleUpdate(NULL);
}
static void installer_progress_finish(int ok) {
    if (!g_console) return;
    consoleClear();
    if (ok) {
        printf("Extraction complete. Starting Unity...\n");
    } else {
        printf("Installation failed.\n\n%s\n", installer_last_error());
    }
    consoleUpdate(NULL);
    svcSleepThread(ok ? 500000000ULL : 1500000000ULL);
    consoleExit(NULL); g_console = 0;
}

static int zip_open(ZipArchive *z, const char *path) {
    memset(z,0,sizeof(*z)); z->fp = fopen(path,"rb"); if (!z->fp) return -1;
    if (fseeko(z->fp,0,SEEK_END)) goto bad;
    off_t end = ftello(z->fp); if (end < 22) goto bad;
    size_t tail_n = (size_t)(end < 0x10016 ? end : 0x10016);
    uint8_t *tail = malloc(tail_n); if (!tail) goto bad;
    if (fseeko(z->fp,end-(off_t)tail_n,SEEK_SET) || fread(tail,1,tail_n,z->fp)!=tail_n) { free(tail); goto bad; }
    for (size_t pos=tail_n-22;;--pos) {
        if (rd32(tail+pos)==0x06054b50u) {
            z->entries=rd16(tail+pos+10); z->cd_offset=rd32(tail+pos+16); free(tail);
            if (!z->entries || z->cd_offset==UINT32_MAX) goto bad;
            return 0;
        }
        if (!pos) break;
    }
    free(tail);
bad:
    fclose(z->fp); z->fp=NULL; return -1;
}
static void zip_close(ZipArchive *z) { if (z->fp) fclose(z->fp); z->fp=NULL; }

static int zip_visit(ZipArchive *z, ZipVisitor visitor, void *user) {
    if (fseeko(z->fp,z->cd_offset,SEEK_SET)) return -1;
    for (uint16_t i=0;i<z->entries;i++) {
        uint8_t h[46]; if (fread(h,1,sizeof(h),z->fp)!=sizeof(h) || rd32(h)!=0x02014b50u) return -1;
        uint16_t nl=rd16(h+28), xl=rd16(h+30), cl=rd16(h+32);
        if (!nl || nl>=ZIP_NAME_MAX) return -1;
        ZipEntry e={0};
        e.flags=rd16(h+8); e.method=rd16(h+10); e.crc32=rd32(h+16);
        e.csize=rd32(h+20); e.usize=rd32(h+24); e.local_offset=rd32(h+42);
        if(e.csize==UINT32_MAX || e.usize==UINT32_MAX || e.local_offset==UINT32_MAX){
            installer_set_error("ZIP64 game.apk files are not supported by this installer.");
            return -1;
        }
        if (fread(e.name,1,nl,z->fp)!=nl) return -1;
        e.name[nl]=0;
        if (fseeko(z->fp,(off_t)xl+cl,SEEK_CUR)) return -1;
        off_t next=ftello(z->fp);
        if (visitor(z,&e,user)<0) return -1;
        if (fseeko(z->fp,next,SEEK_SET)) return -1;
    }
    return 0;
}

static int install_entry_target(const ZipEntry *e, char target[PATH_MAX], unsigned *required_bit) {
    if (required_bit) *required_bit = 0;
    if (!strcmp(e->name, "AndroidManifest.xml")) {
        int n = snprintf(target, PATH_MAX, DATA_ROOT "/AndroidManifest.xml");
        if (n < 0 || n >= PATH_MAX) {
            installer_set_error("AndroidManifest.xml target path is too long.");
            return -1;
        }
        if (required_bit) *required_bit = INSTALL_REQ_MANIFEST;
        return 1;
    }

    if (!strncmp(e->name, "assets/", 7)) {
        if (has_trailing_slash(e->name)) return 0;
        if (!is_safe_relative_path(e->name)) {
            installer_set_errorf("Unsafe path found in game.apk:\n%s", e->name);
            return -1;
        }
        int n = snprintf(target, PATH_MAX, DATA_ROOT "/%s", e->name);
        if (n < 0 || n >= PATH_MAX) {
            installer_set_errorf("Extracted asset path is too long:\n%s", e->name);
            return -1;
        }
        if (required_bit) {
            if (!strcmp(e->name, "assets/bin/Data/Managed/Metadata/global-metadata.dat"))
                *required_bit = INSTALL_REQ_METADATA;
            else if (!strcmp(e->name, "assets/bin/Data/data.unity3d"))
                *required_bit = INSTALL_REQ_DATA;
            else if (!strcmp(e->name, "assets/bin/Data/resources.resource"))
                *required_bit = INSTALL_REQ_RESOURCES;
            else if (!strcmp(e->name, "assets/bin/Data/unity_app_guid"))
                *required_bit = INSTALL_REQ_GUID;
            else if (!strcmp(e->name, "assets/bin/Data/Managed/Resources/mscorlib.dll-resources.dat"))
                *required_bit = INSTALL_REQ_MSCORLIB;
            else if (!strcmp(e->name, "assets/bin/Data/Managed/Resources/System.Data.dll-resources.dat"))
                *required_bit = INSTALL_REQ_SYSTEM_DATA;
            else if (!strcmp(e->name, "assets/bin/Data/Managed/Resources/System.Drawing.dll-resources.dat"))
                *required_bit = INSTALL_REQ_SYSTEM_DRAWING;
            else if (!strcmp(e->name, "assets/local/live_English.bytes"))
                *required_bit = INSTALL_REQ_ENGLISH;
        }
        return 1;
    }

    if (!strncmp(e->name, "lib/arm64-v8a/", 14)) {
        const char *name = e->name + 14;
        if (strcmp(name, LIB_MAIN) && strcmp(name, LIB_UNITY) && strcmp(name, LIB_IL2CPP)) return 0;
        int n = snprintf(target, PATH_MAX, DATA_ROOT "/%s", name);
        if (n < 0 || n >= PATH_MAX) {
            installer_set_errorf("Extracted library path is too long:\n%s", e->name);
            return -1;
        }
        if (required_bit) {
            if (!strcmp(name, LIB_MAIN)) *required_bit = INSTALL_REQ_MAIN;
            else if (!strcmp(name, LIB_UNITY)) *required_bit = INSTALL_REQ_UNITY;
            else if (!strcmp(name, LIB_IL2CPP)) *required_bit = INSTALL_REQ_IL2CPP;
        }
        return 1;
    }
    return 0;
}

static const char *cache_target_for_apk_entry(const char *name) {
    if (!strcmp(name, "assets/bin/Data/Managed/Metadata/global-metadata.dat"))
        return DATA_ROOT "/files/il2cpp/Metadata/global-metadata.dat";
    if (!strcmp(name, "assets/bin/Data/Managed/Resources/mscorlib.dll-resources.dat"))
        return DATA_ROOT "/files/il2cpp/Resources/mscorlib.dll-resources.dat";
    if (!strcmp(name, "assets/bin/Data/Managed/Resources/System.Data.dll-resources.dat"))
        return DATA_ROOT "/files/il2cpp/Resources/System.Data.dll-resources.dat";
    if (!strcmp(name, "assets/bin/Data/Managed/Resources/System.Drawing.dll-resources.dat"))
        return DATA_ROOT "/files/il2cpp/Resources/System.Drawing.dll-resources.dat";
    if (!strcmp(name, "assets/local/live_English.bytes"))
        return DATA_ROOT "/files/downloaded/live_English.bytes";
    return NULL;
}

typedef struct {
    uint64_t final_payload_bytes;
    uint64_t existing_payload_credit;
    uint64_t cache_bytes;
    uint64_t existing_cache_credit;
    uint64_t replace_peak_bytes;
    unsigned files;
    unsigned required_mask;
} InstallSpacePlan;

static uint64_t existing_credit_for_file(const char *path, uint64_t expected) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return 0;
    uint64_t have = (uint64_t)st.st_size;
    return have < expected ? have : expected;
}

static int install_space_visitor(ZipArchive *z, const ZipEntry *e, void *user) {
    (void)z;
    InstallSpacePlan *p = user;
    char target[PATH_MAX];
    unsigned required_bit = 0;
    int selected = install_entry_target(e, target, &required_bit);
    if (selected < 0) return -1;
    if (!selected) return 0;

    uint64_t size = e->usize;
    p->final_payload_bytes += size;
    p->existing_payload_credit += existing_credit_for_file(target, size);
    if (file_exists_nonempty(target) && size > p->replace_peak_bytes) p->replace_peak_bytes = size;
    p->files++;
    p->required_mask |= required_bit;

    const char *cache_target = cache_target_for_apk_entry(e->name);
    if (cache_target) {
        p->cache_bytes += size;
        p->existing_cache_credit += existing_credit_for_file(cache_target, size);
        if (file_exists_nonempty(cache_target) && size > p->replace_peak_bytes) p->replace_peak_bytes = size;
    }
    return 0;
}

static int build_install_space_plan(ZipArchive *z, InstallSpacePlan *p, uint64_t *required_extra) {
    memset(p, 0, sizeof(*p));
    if (zip_visit(z, install_space_visitor, p) < 0) {
        if (!g_error[0]) installer_set_error("Could not inspect game.apk contents for the install size check.");
        return -1;
    }
    const unsigned required = INSTALL_REQ_MANIFEST | INSTALL_REQ_MAIN | INSTALL_REQ_UNITY |
        INSTALL_REQ_IL2CPP | INSTALL_REQ_METADATA | INSTALL_REQ_DATA | INSTALL_REQ_RESOURCES |
        INSTALL_REQ_GUID | INSTALL_REQ_MSCORLIB | INSTALL_REQ_SYSTEM_DATA |
        INSTALL_REQ_SYSTEM_DRAWING | INSTALL_REQ_ENGLISH;
    if ((p->required_mask & required) != required) {
        installer_set_error("game.apk is missing one or more required ARM64 Unity/IL2CPP/cache files. Verify that the correct APK was copied and is complete.");
        return -1;
    }

    uint64_t payload_growth = p->final_payload_bytes > p->existing_payload_credit
        ? p->final_payload_bytes - p->existing_payload_credit : 0;
    uint64_t cache_growth = p->cache_bytes > p->existing_cache_credit
        ? p->cache_bytes - p->existing_cache_credit : 0;
    uint64_t need = payload_growth + cache_growth + p->replace_peak_bytes + INSTALL_SPACE_RESERVE_BYTES;
    if (required_extra) *required_extra = need;
    return 0;
}

static int extract_entry(ZipArchive *z, const ZipEntry *e, const char *target) {
    if (e->flags & 1u) { installer_set_error("Encrypted ZIP entries are unsupported."); return -1; }
    if (e->method != 0 && e->method != 8) { installer_set_error("game.apk uses an unsupported ZIP compression method."); return -1; }
    uint8_t lh[30];
    if (fseeko(z->fp,e->local_offset,SEEK_SET) || fread(lh,1,sizeof(lh),z->fp)!=sizeof(lh) || rd32(lh)!=0x04034b50u) { installer_set_error("Bad APK local ZIP header."); return -1; }
    uint16_t nl=rd16(lh+26), xl=rd16(lh+28);
    if (fseeko(z->fp,(off_t)nl+xl,SEEK_CUR) || create_parent_directories(target)<0) return -1;
    char tmp[PATH_MAX + 5];
    int tmp_len=snprintf(tmp,sizeof(tmp),"%s.tmp",target);
    if(tmp_len<0||(size_t)tmp_len>=sizeof(tmp)){installer_set_error("Extracted file path is too long.");return -1;}
    FILE *out=fopen(tmp,"wb");
    if (!out) {
        installer_set_errorf("Could not create extracted file:\n%s\nerrno=%d (%s)", tmp, errno, strerror(errno));
        return -1;
    }
    uint8_t *in=malloc(ZIP_IO_BYTES), *obuf=malloc(ZIP_IO_BYTES);
    if(!in || !obuf){
        free(in); free(obuf); fclose(out); remove(tmp);
        installer_set_error("Not enough memory for the APK extraction buffers.");
        return -1;
    }
    int ok=1;
    uint64_t written=0;
    uLong crc=crc32(0L,Z_NULL,0);
    if (e->method==0) {
        uint32_t left=e->csize;
        while (ok && left) {
            size_t n=left<ZIP_IO_BYTES?left:ZIP_IO_BYTES;
            if(fread(in,1,n,z->fp)!=n){ok=0;break;}
            crc=crc32(crc,in,(uInt)n);
            if(fwrite(in,1,n,out)!=n){ok=0;break;}
            left-=(uint32_t)n; written+=n; installer_progress_update(e->name,written,e->usize);
        }
    } else {
        z_stream s={0}; int st=Z_OK; ok=ok && inflateInit2(&s,-MAX_WBITS)==Z_OK; uint32_t left=e->csize;
        while (ok && st!=Z_STREAM_END) {
            if (!s.avail_in && left) { size_t n=left<ZIP_IO_BYTES?left:ZIP_IO_BYTES; if (fread(in,1,n,z->fp)!=n){ok=0;break;} left-=(uint32_t)n; s.next_in=in; s.avail_in=(uInt)n; }
            s.next_out=obuf; s.avail_out=ZIP_IO_BYTES; st=inflate(&s,left?Z_NO_FLUSH:Z_FINISH);
            size_t made=ZIP_IO_BYTES-s.avail_out;
            if(made){
                crc=crc32(crc,obuf,(uInt)made);
                if(fwrite(obuf,1,made,out)!=made){ok=0;break;}
            }
            written+=made; installer_progress_update(e->name,written,e->usize);
            if (st!=Z_OK && st!=Z_STREAM_END && st!=Z_BUF_ERROR){ok=0;break;} if(st==Z_BUF_ERROR&&!left&&!s.avail_in){ok=0;break;}
        }
        inflateEnd(&s); ok=ok && st==Z_STREAM_END;
    }
    free(in); free(obuf);
    if(fclose(out)!=0)ok=0;
    if (!ok || written!=e->usize) {
        int saved_errno = errno;
        remove(tmp);
        installer_set_errorf("APK extraction failed or was truncated:\n%s\nexpected=%llu bytes, wrote=%llu bytes\nerrno=%d (%s)",
            e->name, (unsigned long long)e->usize, (unsigned long long)written,
            saved_errno, strerror(saved_errno));
        return -1;
    }
    if ((uint32_t)crc != e->crc32) {
        remove(tmp);
        installer_set_errorf("APK CRC check failed for:\n%s\nexpected=%08x got=%08x\n\ngame.apk may be incomplete or corrupted.",
            e->name, e->crc32, (uint32_t)crc);
        return -1;
    }
    remove(target);
    if (rename(tmp,target)!=0) {
        int saved_errno = errno;
        remove(tmp);
        installer_set_errorf("Could not finalize extracted file:\n%s\nerrno=%d (%s)", target, saved_errno, strerror(saved_errno));
        return -1;
    }
    return 0;
}

typedef struct { unsigned files; int have_main,have_unity,have_il2cpp,have_metadata,have_data,have_resources,have_manifest; } InstallState;
static int install_visitor(ZipArchive *z,const ZipEntry *e,void *u) {
    InstallState *s=u;
    char target[PATH_MAX];
    unsigned required_bit=0;
    int selected=install_entry_target(e,target,&required_bit);
    if(selected<0)return -1;
    if(!selected)return 0;
    if(required_bit==INSTALL_REQ_MANIFEST)s->have_manifest=1;
    else if(required_bit==INSTALL_REQ_MAIN)s->have_main=1;
    else if(required_bit==INSTALL_REQ_UNITY)s->have_unity=1;
    else if(required_bit==INSTALL_REQ_IL2CPP)s->have_il2cpp=1;
    else if(required_bit==INSTALL_REQ_METADATA)s->have_metadata=1;
    else if(required_bit==INSTALL_REQ_DATA)s->have_data=1;
    else if(required_bit==INSTALL_REQ_RESOURCES)s->have_resources=1;
    if (extract_entry(z,e,target)<0) return -1;
    s->files++;
    return 0;
}

typedef struct {
    const uint8_t *base;
    size_t size;
    uint32_t count;
    uint32_t flags;
    uint32_t strings_start;
    uint16_t header_size;
} AxmlStrings;

static int axml_u8len(const uint8_t *p, const uint8_t *end, size_t *value, size_t *used) {
    if (p >= end) return -1;
    uint8_t a = *p++;
    if (a & 0x80u) {
        if (p >= end) return -1;
        *value = ((size_t)(a & 0x7fu) << 8) | *p;
        *used = 2;
    } else {
        *value = a;
        *used = 1;
    }
    return 0;
}

static int axml_u16len(const uint8_t *p, const uint8_t *end, size_t *value, size_t *used) {
    if ((size_t)(end - p) < 2) return -1;
    uint16_t a = rd16(p);
    if (a & 0x8000u) {
        if ((size_t)(end - p) < 4) return -1;
        *value = ((size_t)(a & 0x7fffu) << 16) | rd16(p + 2);
        *used = 4;
    } else {
        *value = a;
        *used = 2;
    }
    return 0;
}

static int axml_string(const AxmlStrings *sp, uint32_t idx, char *out, size_t cap) {
    if (!sp || !out || !cap || idx >= sp->count) return -1;
    const uint8_t *end = sp->base + sp->size;
    const uint8_t *offs = sp->base + sp->header_size;
    if ((size_t)(end - offs) < (size_t)sp->count * 4u) return -1;
    uint32_t off = rd32(offs + (size_t)idx * 4u);
    if (sp->strings_start > sp->size || off > sp->size - sp->strings_start) return -1;
    const uint8_t *q = sp->base + sp->strings_start + off;

    if (sp->flags & 0x100u) {
        size_t utf16_units = 0, n8 = 0, used1 = 0, used2 = 0;
        if (axml_u8len(q, end, &utf16_units, &used1) < 0) return -1;
        q += used1;
        if (axml_u8len(q, end, &n8, &used2) < 0) return -1;
        q += used2;
        (void)utf16_units;
        if (n8 > (size_t)(end - q)) return -1;
        size_t n = n8 < cap - 1 ? n8 : cap - 1;
        memcpy(out, q, n);
        out[n] = 0;
        return 0;
    }

    size_t units = 0, used = 0;
    if (axml_u16len(q, end, &units, &used) < 0) return -1;
    q += used;
    if (units > (size_t)(end - q) / 2u) return -1;
    size_t w = 0;
    for (size_t i = 0; i < units && w + 1 < cap; ++i) {
        uint16_t c = rd16(q + i * 2u);
        if (c < 0x80u) out[w++] = (char)c;
        else if (c < 0x800u && w + 2 < cap) {
            out[w++] = (char)(0xc0u | (c >> 6));
            out[w++] = (char)(0x80u | (c & 0x3fu));
        } else if (c >= 0xd800u && c <= 0xdbffu && i + 1 < units) {
            uint16_t lo = rd16(q + (i + 1) * 2u);
            if (lo >= 0xdc00u && lo <= 0xdfffu && w + 4 < cap) {
                uint32_t cp = 0x10000u + (((uint32_t)c - 0xd800u) << 10) + ((uint32_t)lo - 0xdc00u);
                out[w++] = (char)(0xf0u | (cp >> 18));
                out[w++] = (char)(0x80u | ((cp >> 12) & 0x3fu));
                out[w++] = (char)(0x80u | ((cp >> 6) & 0x3fu));
                out[w++] = (char)(0x80u | (cp & 0x3fu));
                ++i;
            } else {
                return -1;
            }
        } else if (c >= 0xdc00u && c <= 0xdfffu) {
            return -1;
        } else if (w + 3 < cap) {
            out[w++] = (char)(0xe0u | (c >> 12));
            out[w++] = (char)(0x80u | ((c >> 6) & 0x3fu));
            out[w++] = (char)(0x80u | (c & 0x3fu));
        } else {
            break;
        }
    }
    out[w] = 0;
    return 0;
}

static int axml_attr_string(const AxmlStrings *sp, const uint8_t *a, const uint8_t *end,
                            char *out, size_t cap) {
    if ((size_t)(end - a) < 20) return -1;
    uint32_t raw = rd32(a + 8);
    uint8_t type = a[15];
    uint32_t data = rd32(a + 16);
    if (raw != 0xffffffffu) return axml_string(sp, raw, out, cap);
    if (type == 0x03u) return axml_string(sp, data, out, cap);
    return -1;
}

static int axml_attr_int(const AxmlStrings *sp, const uint8_t *a, const uint8_t *end, int *out) {
    if ((size_t)(end - a) < 20 || !out) return -1;
    uint8_t type = a[15];
    uint32_t data = rd32(a + 16);
    if (type == 0x10u || type == 0x11u) {
        *out = (int)data;
        return 0;
    }
    char tmp[48];
    if (axml_attr_string(sp, a, end, tmp, sizeof(tmp)) == 0) {
        char *ep = NULL;
        long v = strtol(tmp, &ep, 0);
        if (ep && *ep == 0 && v >= INT_MIN && v <= INT_MAX) {
            *out = (int)v;
            return 0;
        }
    }
    return -1;
}

static int parse_android_manifest(const char *path) {
    g_manifest_package[0] = 0;
    g_manifest_version_name[0] = 0;
    g_manifest_version_code = -1;
    g_manifest_min_sdk = -1;
    g_manifest_target_sdk = -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseeko(f, 0, SEEK_END)) { fclose(f); return -1; }
    off_t z = ftello(f);
    if (z < 8 || z > (off_t)(8 * 1024 * 1024)) { fclose(f); return -1; }
    if (fseeko(f, 0, SEEK_SET)) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)z);
    if (!buf) { fclose(f); return -1; }
    int ok = fread(buf, 1, (size_t)z, f) == (size_t)z;
    fclose(f);
    if (!ok) { free(buf); return -1; }

    size_t size = (size_t)z;
    if (rd16(buf) != 0x0003u || rd32(buf + 4) > size) {
        free(buf);
        return -1;
    }

    AxmlStrings sp = {0};
    for (size_t pos = rd16(buf + 2); pos + 8 <= size; ) {
        uint16_t type = rd16(buf + pos);
        uint16_t hs = rd16(buf + pos + 2);
        uint32_t cs = rd32(buf + pos + 4);
        if (cs < 8 || hs < 8 || hs > cs || cs > size - pos) break;

        if (type == 0x0001u && hs >= 28) {
            sp.base = buf + pos;
            sp.size = cs;
            sp.header_size = hs;
            sp.count = rd32(buf + pos + 8);
            sp.flags = rd32(buf + pos + 16);
            sp.strings_start = rd32(buf + pos + 20);
        } else if (type == 0x0102u && sp.base && hs >= 16 && cs >= 36) {
            const uint8_t *chunk = buf + pos;
            const uint8_t *end = chunk + cs;
            const uint8_t *ext = chunk + 16;
            uint32_t name_idx = rd32(ext + 4);
            char elem[64];
            if (axml_string(&sp, name_idx, elem, sizeof(elem)) == 0) {
                uint16_t attr_start = rd16(ext + 8);
                uint16_t attr_size = rd16(ext + 10);
                uint16_t attr_count = rd16(ext + 12);
                const uint8_t *attrs = ext + attr_start;
                if (attr_size >= 20 && attrs <= end &&
                    (size_t)(end - attrs) >= (size_t)attr_size * attr_count) {
                    for (uint16_t i = 0; i < attr_count; ++i) {
                        const uint8_t *a = attrs + (size_t)i * attr_size;
                        uint32_t attr_name_idx = rd32(a + 4);
                        char key[64];
                        if (axml_string(&sp, attr_name_idx, key, sizeof(key)) < 0) continue;
                        if (!strcmp(elem, "manifest")) {
                            if (!strcmp(key, "package"))
                                (void)axml_attr_string(&sp, a, end, g_manifest_package, sizeof(g_manifest_package));
                            else if (!strcmp(key, "versionName"))
                                (void)axml_attr_string(&sp, a, end, g_manifest_version_name, sizeof(g_manifest_version_name));
                            else if (!strcmp(key, "versionCode"))
                                (void)axml_attr_int(&sp, a, end, &g_manifest_version_code);
                        } else if (!strcmp(elem, "uses-sdk")) {
                            if (!strcmp(key, "minSdkVersion"))
                                (void)axml_attr_int(&sp, a, end, &g_manifest_min_sdk);
                            else if (!strcmp(key, "targetSdkVersion"))
                                (void)axml_attr_int(&sp, a, end, &g_manifest_target_sdk);
                        }
                    }
                }
            }
        }
        pos += cs;
    }
    free(buf);

    if (g_manifest_package[0] && strcmp(g_manifest_package, GAME_PACKAGE)) {
        return -1;
    }
    return g_manifest_package[0] ? 0 : -1;
}

typedef struct { int found; } ManifestExtractState;
static int manifest_visitor(ZipArchive *z, const ZipEntry *e, void *user) {
    ManifestExtractState *state = user;
    if (strcmp(e->name, "AndroidManifest.xml")) return 0;
    state->found = 1;
    return extract_entry(z, e, DATA_ROOT "/AndroidManifest.xml");
}

static int load_manifest_metadata(void) {
    const char *cached = DATA_ROOT "/AndroidManifest.xml";
    if (!file_exists_nonempty(cached) && file_exists_nonempty(INSTALL_APK)) {
        ZipArchive z;
        if (zip_open(&z, INSTALL_APK) == 0) {
            ManifestExtractState st = {0};
            (void)zip_visit(&z, manifest_visitor, &st);
            zip_close(&z);
        }
    }
    return parse_android_manifest(cached);
}

static int validate_manifest_metadata(void) {
    const char *pkg = installer_package_name();
    if (!pkg || !*pkg) {
        installer_set_error("Could not parse the extracted AndroidManifest.xml. game.apk may be incomplete or corrupted.");
        return -1;
    }
    if (strcmp(pkg, GAME_PACKAGE) != 0) {
        installer_set_errorf("Wrong game.apk package.\n\nExpected: %s\nFound:    %s", GAME_PACKAGE, pkg);
        return -1;
    }
    return 0;
}

static int runtime_files_present(void) {
    const char *req[]={DATA_ROOT "/" LIB_MAIN,DATA_ROOT "/" LIB_UNITY,DATA_ROOT "/" LIB_IL2CPP,
      DATA_ROOT "/assets/bin/Data/Managed/Metadata/global-metadata.dat",DATA_ROOT "/assets/bin/Data/data.unity3d",
      DATA_ROOT "/assets/bin/Data/resources.resource",DATA_ROOT "/assets/bin/Data/unity_app_guid"};
    for (unsigned i=0;i<sizeof(req)/sizeof(req[0]);i++) if(!file_exists_nonempty(req[i])) return 0;
    return 1;
}

static int write_runtime_install_marker(unsigned files) {
    char tmp[PATH_MAX + 5];
    int n=snprintf(tmp,sizeof(tmp),"%s.tmp",INSTALL_MARKER);
    if(n<0 || (size_t)n>=sizeof(tmp)){
        installer_set_error("Install marker path is too long.");
        return -1;
    }
    if(create_parent_directories(INSTALL_MARKER)<0) return -1;

    FILE *m=fopen(tmp,"wb");
    if(!m){
        installer_set_error("Could not create the install marker.");
        return -1;
    }
    int ok=fprintf(m,"Unity %s\nfiles=%u\n",UNITY_VERSION,files)>0;
    if(fflush(m)!=0) ok=0;
    if(fclose(m)!=0) ok=0;
    if(!ok){
        remove(tmp);
        installer_set_error("Could not write the install marker.");
        return -1;
    }
    remove(INSTALL_MARKER);
    if(rename(tmp,INSTALL_MARKER)!=0){
        remove(tmp);
        installer_set_error("Could not finalize the install marker.");
        return -1;
    }
    return 0;
}

static void remove_install_media_if_safe(void) {
    if(!file_exists_nonempty(INSTALL_APK)) return;
    if(!file_exists_nonempty(INSTALL_MARKER) || !runtime_files_present()) return;
    if(!file_exists_nonempty(DATA_ROOT "/AndroidManifest.xml")) return;
    (void)remove(INSTALL_APK);
}

static const struct { const char *src; const char *dst; } il2cpp_cache_files[] = {
    { DATA_ROOT "/assets/bin/Data/Managed/Metadata/global-metadata.dat",
      DATA_ROOT "/files/il2cpp/Metadata/global-metadata.dat" },
    { DATA_ROOT "/assets/bin/Data/Managed/Resources/mscorlib.dll-resources.dat",
      DATA_ROOT "/files/il2cpp/Resources/mscorlib.dll-resources.dat" },
    { DATA_ROOT "/assets/bin/Data/Managed/Resources/System.Data.dll-resources.dat",
      DATA_ROOT "/files/il2cpp/Resources/System.Data.dll-resources.dat" },
    { DATA_ROOT "/assets/bin/Data/Managed/Resources/System.Drawing.dll-resources.dat",
      DATA_ROOT "/files/il2cpp/Resources/System.Drawing.dll-resources.dat" },
};

static int files_have_same_size(const char *a, const char *b) {
    struct stat sa, sb;
    return stat(a,&sa)==0 && stat(b,&sb)==0 && sa.st_size>0 && sa.st_size==sb.st_size;
}

static int copy_file_atomic(const char *src, const char *dst) {
    if (create_parent_directories(dst)<0) return -1;

    FILE *in=fopen(src,"rb");
    if(!in){
        installer_set_errorf("Could not open extracted runtime resource:\n%s\nerrno=%d (%s)", src, errno, strerror(errno));
        return -1;
    }

    char tmp[PATH_MAX+5];
    int n=snprintf(tmp,sizeof(tmp),"%s.tmp",dst);
    if(n<0 || (size_t)n>=sizeof(tmp)){ fclose(in); installer_set_error("IL2CPP cache path is too long."); return -1; }
    FILE *out=fopen(tmp,"wb");
    if(!out){
        int saved_errno=errno;
        fclose(in);
        installer_set_errorf("Could not create runtime cache file:\n%s\nerrno=%d (%s)", tmp, saved_errno, strerror(saved_errno));
        return -1;
    }

    uint8_t *buf=malloc(ZIP_IO_BYTES);
    int ok=buf!=NULL;
    while(ok){
        size_t got=fread(buf,1,ZIP_IO_BYTES,in);
        if(got && fwrite(buf,1,got,out)!=got){ ok=0; break; }
        if(got<ZIP_IO_BYTES){ if(ferror(in)) ok=0; break; }
    }
    free(buf);
    if(fclose(out)!=0) ok=0;
    fclose(in);
    if(!ok){
        int saved_errno=errno;
        remove(tmp);
        installer_set_errorf("Could not copy runtime resource:\n%s\n-> %s\nerrno=%d (%s)", src, dst, saved_errno, strerror(saved_errno));
        return -1;
    }
    remove(dst);
    if(rename(tmp,dst)!=0){
        int saved_errno=errno;
        remove(tmp);
        installer_set_errorf("Could not finalize runtime cache file:\n%s\nerrno=%d (%s)", dst, saved_errno, strerror(saved_errno));
        return -1;
    }
    return 0;
}

static int il2cpp_marker_matches(const char guid[37]) {
    char got[37]={0};
    FILE *f=fopen(DATA_ROOT "/files/il2cpp/unity.ver","rb");
    if(!f) return 0;
    size_t n=fread(got,1,36,f);
    int extra=fgetc(f);
    fclose(f);
    return n==36 && extra==EOF && memcmp(got,guid,36)==0;
}

static int seed_english_localization_cache(void) {
    const char *src = DATA_ROOT "/assets/local/live_English.bytes";
    const char *dst = DATA_ROOT "/files/downloaded/live_English.bytes";
    struct stat src_st, dst_st;

    if (stat(src, &src_st) != 0 || !S_ISREG(src_st.st_mode) || src_st.st_size <= 0) {
        installer_set_error("Missing packaged assets/local/live_English.bytes.");
        return -1;
    }
    if (stat(dst, &dst_st) == 0 && S_ISREG(dst_st.st_mode) && dst_st.st_size == src_st.st_size && dst_st.st_size > 0)
        return 0;
    if (copy_file_atomic(src, dst) < 0) return -1;
    if (stat(dst, &dst_st) != 0 || dst_st.st_size != src_st.st_size || dst_st.st_size <= 0) {
        installer_set_error("Seeded live_English.bytes cache file is invalid.");
        return -1;
    }
    return 0;
}

static int prepare_il2cpp_cache(void) {
    char guid[37]={0};
    FILE *g=fopen(DATA_ROOT "/assets/bin/Data/unity_app_guid","rb");
    if(!g){ installer_set_error("Missing extracted unity_app_guid."); return -1; }
    size_t gn=fread(guid,1,36,g);
    int extra=fgetc(g);
    fclose(g);
    if(gn!=36 || extra!=EOF){ installer_set_error("Extracted unity_app_guid is invalid."); return -1; }

    int ready=il2cpp_marker_matches(guid);
    for(unsigned i=0;i<sizeof(il2cpp_cache_files)/sizeof(il2cpp_cache_files[0]);i++)
        if(!files_have_same_size(il2cpp_cache_files[i].src,il2cpp_cache_files[i].dst)) ready=0;
    if(ready){
        return 0;
    }

    for(unsigned i=0;i<sizeof(il2cpp_cache_files)/sizeof(il2cpp_cache_files[0]);i++){
        if(!file_exists_nonempty(il2cpp_cache_files[i].src)){
            installer_set_error("An extracted Managed IL2CPP resource is missing.");
            return -1;
        }
        if(copy_file_atomic(il2cpp_cache_files[i].src,il2cpp_cache_files[i].dst)<0) return -1;
    }

    const char *marker=DATA_ROOT "/files/il2cpp/unity.ver";
    if(create_parent_directories(marker)<0) return -1;
    char tmp[PATH_MAX+5];
    int tn=snprintf(tmp,sizeof(tmp),"%s.tmp",marker);
    if(tn<0 || (size_t)tn>=sizeof(tmp)){ installer_set_error("IL2CPP marker path is too long."); return -1; }
    FILE *m=fopen(tmp,"wb");
    if(!m){ remove(tmp); installer_set_error("Could not write IL2CPP unity.ver."); return -1; }
    int marker_ok=(fwrite(guid,1,36,m)==36);
    if(fclose(m)!=0) marker_ok=0;
    if(!marker_ok){
        remove(tmp);
        installer_set_error("Could not write IL2CPP unity.ver.");
        return -1;
    }
    remove(marker);
    if(rename(tmp,marker)!=0){ remove(tmp); installer_set_error("Could not finalize IL2CPP unity.ver."); return -1; }

    return 0;
}

static int verify_game_apk_basic(void) {
    struct stat st;
    if (stat(INSTALL_APK, &st) != 0) {
        installer_set_errorf("Missing game.apk for first installation:\n%s\nerrno=%d (%s)",
            INSTALL_APK, errno, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        installer_set_errorf("game.apk is not a regular file:\n%s", INSTALL_APK);
        return -1;
    }
    if (st.st_size < 22) {
        installer_set_errorf("game.apk is empty or too small to be a valid APK (%lld bytes).",
            (long long)st.st_size);
        return -1;
    }
    return 0;
}

static int installer_preflight(ZipArchive *z) {
    if (verify_install_location_writable() < 0) return -1;

    InstallSpacePlan plan;
    uint64_t required_extra = 0;
    if (build_install_space_plan(z, &plan, &required_extra) < 0) return -1;

    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
    if (query_sd_space(&free_bytes, &total_bytes) < 0) return -1;
    if (free_bytes < required_extra) {
        installer_set_errorf(
            "Not enough free space on the SD card.\n\nAvailable: %llu MiB\nRequired:  %llu MiB\n\nFree at least %llu MiB more and try again.",
            bytes_to_mib_ceil(free_bytes), bytes_to_mib_ceil(required_extra),
            bytes_to_mib_ceil(required_extra - free_bytes));
        return -1;
    }

    if (!g_console) { consoleInit(NULL); g_console = 1; }
    consoleClear();
    printf("Angry Bird Epic All Stars - first run\n\n");
    printf("game.apk verified\n");
    printf("Files to extract: %u\n", plan.files);
    printf("Install data:     %llu MiB\n", bytes_to_mib_ceil(plan.final_payload_bytes + plan.cache_bytes));
    printf("Extra required:   %llu MiB\n", bytes_to_mib_ceil(required_extra));
    printf("SD free:          %llu MiB / %llu MiB\n\n", bytes_to_mib_ceil(free_bytes), bytes_to_mib_ceil(total_bytes));
    printf("Starting extraction...\n");
    consoleUpdate(NULL);
    svcSleepThread(300000000ULL);
    return 0;
}

int installer_prepare_game_files(void) {
    g_error[0]=0;
    if (file_exists_nonempty(INSTALL_MARKER) && runtime_files_present()){
        (void)load_manifest_metadata();
        if (validate_manifest_metadata() < 0) return -1;
        if (prepare_il2cpp_cache() < 0) return -1;
        if (seed_english_localization_cache() < 0) return -1;
        (void)credits_patch_apply();
        remove_install_media_if_safe();
        return 0;
    }
    if (verify_game_apk_basic() < 0) return -1;
    if (ensure_directory(GAME_HOME) < 0 || ensure_directory(DATA_ROOT) < 0) return -1;
    installer_progress_update("Opening game.apk",0,1);
    ZipArchive z;
    if(zip_open(&z,INSTALL_APK)<0){
        installer_set_error("game.apk is not a supported ZIP/APK or its central directory is damaged.");
        installer_progress_finish(0);
        return -1;
    }
    if(installer_preflight(&z)<0){
        zip_close(&z);
        installer_progress_finish(0);
        return -1;
    }
    InstallState st={0}; int rc=zip_visit(&z,install_visitor,&st); zip_close(&z);
    if(rc<0 || !runtime_files_present() || !st.have_manifest || !st.have_main || !st.have_unity || !st.have_il2cpp || !st.have_metadata || !st.have_data || !st.have_resources){
        if (!g_error[0])
            installer_set_error("game.apk does not contain the required ARM64 Unity 6000 files.");
        installer_progress_finish(0);
        return -1;
    }
    (void)load_manifest_metadata();
    if(validate_manifest_metadata()<0){ installer_progress_finish(0); return -1; }
    if(!file_exists_nonempty(DATA_ROOT "/AndroidManifest.xml")){
        installer_set_error("Extracted AndroidManifest.xml is missing.");
        installer_progress_finish(0);
        return -1;
    }
    if(prepare_il2cpp_cache()<0){ installer_progress_finish(0); return -1; }
    if(seed_english_localization_cache()<0){ installer_progress_finish(0); return -1; }
    (void)credits_patch_apply();
    if(write_runtime_install_marker(st.files)<0){ installer_progress_finish(0); return -1; }
    remove_install_media_if_safe();
    installer_progress_finish(1); return 0;
}
