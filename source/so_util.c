#include "so_util.h"
#include "fatal.h"
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#ifndef DT_RELR
#define DT_RELR 36
#define DT_RELRSZ 35
#endif
#define DT_ANDROID_RELR 0x6fffe000
#define DT_ANDROID_RELRSZ 0x6fffe001

static so_module *g_modules;

static Elf64_Xword dynamic_tag(so_module *m, Elf64_Sxword tag) {
    for(int i=0;i<m->phnum;i++) if(m->phdr[i].p_type==PT_DYNAMIC) {
        const Elf64_Dyn *d=(const Elf64_Dyn*)((uintptr_t)m->so_base+m->phdr[i].p_offset);
        for(;d->d_tag!=DT_NULL;d++) if(d->d_tag==tag) return d->d_un.d_val;
    }
    return 0;
}

void so_flush_caches(so_module *m){armDCacheFlush(m->load_virtbase,m->load_size);armICacheInvalidate(m->load_virtbase,m->load_size);}
void so_free_temp(so_module *m){free(m->so_base);m->so_base=NULL;}

uint32_t so_read_word(so_module *m,size_t offset){
    if(!m||!m->load_virtbase||offset+sizeof(uint32_t)>m->load_size)return 0;
    return *(const volatile uint32_t*)((const uint8_t*)m->load_virtbase+offset);
}

int so_patch_code(so_module *m,size_t offset,const void *data,size_t size){
    if(!m||!m->load_virtbase||!data||!size||offset+size>m->load_size)return -1;
    uintptr_t dst=(uintptr_t)m->load_virtbase+offset;
    uintptr_t page=dst&~(uintptr_t)0xfff;
    size_t maplen=ALIGN_MEM((dst-page)+size,0x1000);
    virtmemLock();
    void *alias=virtmemFindAslr(maplen,0);
    VirtmemReservation *rv=alias?virtmemAddReservation(alias,maplen):NULL;
    virtmemUnlock();
    if(!alias||!rv)return -2;
    Result rc=svcMapProcessMemory(alias,envGetOwnProcessHandle(),(u64)page,maplen);
    if(R_FAILED(rc)){
        virtmemLock();if(rv)virtmemRemoveReservation(rv);virtmemUnlock();
        return -3;
    }
    memcpy((uint8_t*)alias+(dst-page),data,size);
    armDCacheFlush((uint8_t*)alias+(dst-page),size);
    svcUnmapProcessMemory(alias,envGetOwnProcessHandle(),(u64)page,maplen);
    virtmemLock();if(rv)virtmemRemoveReservation(rv);virtmemUnlock();
    armICacheInvalidate((void*)dst,size);
    return 0;
}

int so_check_build_id(so_module *m,const char *expected){
    if(!m||!m->so_base||!expected||!*expected)return 0;
    for(int i=0;i<m->phnum;i++){
        const Elf64_Phdr *ph=&m->phdr[i];
        if(ph->p_type!=PT_NOTE||ph->p_offset+ph->p_filesz>m->so_size)continue;
        const uint8_t *p=(const uint8_t*)m->so_base+ph->p_offset,*end=p+ph->p_filesz;
        while((size_t)(end-p)>=sizeof(Elf64_Nhdr)){
            const Elf64_Nhdr *nh=(const Elf64_Nhdr*)p;p+=sizeof(*nh);
            size_t namesz=ALIGN_MEM((size_t)nh->n_namesz,4),descsz=ALIGN_MEM((size_t)nh->n_descsz,4);
            if(namesz>(size_t)(end-p)||descsz>(size_t)(end-p-namesz))break;
            const char *name=(const char*)p;const uint8_t *desc=p+namesz;
            if(nh->n_type==3&&nh->n_namesz>=3&&!memcmp(name,"GNU",3)){
                char actual[129];size_t n=nh->n_descsz;if(n>64)n=64;
                static const char hex[]="0123456789abcdef";
                for(size_t k=0;k<n;k++){actual[k*2]=hex[desc[k]>>4];actual[k*2+1]=hex[desc[k]&15];}
                actual[n*2]=0;
                if(!strcmp(actual,expected)){return 1;}
                return 0;
            }
            p+=namesz+descsz;
        }
    }
    return 0;
}

int so_load(so_module *m,const char *filename,void *base,size_t max_size){
    memset(m,0,sizeof(*m)); snprintf(m->name,sizeof(m->name),"%s",filename);
    FILE *fp=fopen(filename,"rb"); if(!fp)return -1;
    fseeko(fp,0,SEEK_END); off_t sz=ftello(fp); fseeko(fp,0,SEEK_SET); if(sz<=0){fclose(fp);return -2;}
    m->so_size=(size_t)sz; m->so_base=malloc(m->so_size); if(!m->so_base){fclose(fp);return -3;}
    if(fread(m->so_base,1,m->so_size,fp)!=m->so_size){fclose(fp);so_free_temp(m);return -4;} fclose(fp);
    if(memcmp(m->so_base,ELFMAG,SELFMAG)){so_free_temp(m);return -5;}
    m->elf_hdr=(Elf64_Ehdr*)m->so_base;
    if(m->elf_hdr->e_machine!=EM_AARCH64 || m->elf_hdr->e_phnum>SO_MAX_SEGMENTS){so_free_temp(m);return -6;}
    m->prog_hdr=(Elf64_Phdr*)((uintptr_t)m->so_base+m->elf_hdr->e_phoff);
    m->sec_hdr=(Elf64_Shdr*)((uintptr_t)m->so_base+m->elf_hdr->e_shoff);
    m->shstrtab=(char*)((uintptr_t)m->so_base+m->sec_hdr[m->elf_hdr->e_shstrndx].sh_offset);
    m->phnum=m->elf_hdr->e_phnum; memcpy(m->phdr,m->prog_hdr,m->phnum*sizeof(Elf64_Phdr));
    size_t hi=0;
    for(int i=0;i<m->phnum;i++) if(m->prog_hdr[i].p_type==PT_LOAD){size_t e=m->prog_hdr[i].p_vaddr+m->prog_hdr[i].p_memsz;if(e>hi)hi=e;}
    m->load_size=ALIGN_MEM(hi,0x1000); if(!base||m->load_size>max_size){so_free_temp(m);return -7;}
    m->load_base=base; memset(base,0,m->load_size);
    virtmemLock();
    m->load_virtbase=virtmemFindCodeMemory(m->load_size,0x1000);
    m->load_memrv=m->load_virtbase?virtmemAddReservation(m->load_virtbase,m->load_size):NULL;
    virtmemUnlock();
    if(!m->load_virtbase||!m->load_memrv){so_free_temp(m);return -8;}
    for(int i=0;i<m->phnum;i++){
        Elf64_Phdr *p=&m->prog_hdr[i];
        if(p->p_type==PT_LOAD && p->p_filesz){
            if(p->p_offset+p->p_filesz>m->so_size){so_free_temp(m);return -9;}
            memcpy((void*)((uintptr_t)m->load_base+p->p_vaddr),(void*)((uintptr_t)m->so_base+p->p_offset),p->p_filesz);
        }
    }
    for(int i=0;i<m->elf_hdr->e_shnum;i++){
        const char *n=m->shstrtab+m->sec_hdr[i].sh_name;
        if(!strcmp(n,".dynsym")){m->syms=(Elf64_Sym*)((uintptr_t)m->load_base+m->sec_hdr[i].sh_addr);m->num_syms=m->sec_hdr[i].sh_size/sizeof(Elf64_Sym);}
        else if(!strcmp(n,".dynstr"))m->dynstrtab=(char*)((uintptr_t)m->load_base+m->sec_hdr[i].sh_addr);
    }
    if(!m->syms||!m->dynstrtab){so_free_temp(m);return -10;}
    m->next=NULL; if(!g_modules)g_modules=m;else{so_module *q=g_modules;while(q->next)q=q->next;q->next=m;}
    return 0;
}

static void process_relr(so_module *m,const Elf64_Xword *r,size_t sz){
    uintptr_t where=0; size_t count=sz/sizeof(*r);
    for(size_t i=0;i<count;i++){
        Elf64_Xword e=r[i];
        if(!(e&1)){where=(uintptr_t)e;*(uint64_t*)((uintptr_t)m->load_base+where)+=(uint64_t)m->load_virtbase;where+=8;}
        else{for(int bit=1;bit<64;bit++)if(e&(1ull<<bit))*((uint64_t*)((uintptr_t)m->load_base+where+(bit-1)*8))+=(uint64_t)m->load_virtbase;where+=63*8;}
    }
}

int so_relocate(so_module *m){
    for(int i=0;i<m->elf_hdr->e_shnum;i++){
        const char *n=m->shstrtab+m->sec_hdr[i].sh_name;
        if(strcmp(n,".rela.dyn")&&strcmp(n,".rela.plt"))continue;
        Elf64_Rela *rels=(Elf64_Rela*)((uintptr_t)m->load_base+m->sec_hdr[i].sh_addr); int nr=m->sec_hdr[i].sh_size/sizeof(*rels);
        for(int j=0;j<nr;j++){
            uintptr_t *ptr=(uintptr_t*)((uintptr_t)m->load_base+rels[j].r_offset);
            Elf64_Sym *sym=&m->syms[ELF64_R_SYM(rels[j].r_info)]; int type=ELF64_R_TYPE(rels[j].r_info);
            if(type==R_AARCH64_RELATIVE)*ptr=(uintptr_t)m->load_virtbase+rels[j].r_addend;
            else if(type==R_AARCH64_ABS64){if(sym->st_shndx!=SHN_UNDEF)*ptr=(uintptr_t)m->load_virtbase+sym->st_value+rels[j].r_addend;else *ptr=rels[j].r_addend;}
            else if(type==R_AARCH64_GLOB_DAT||type==R_AARCH64_JUMP_SLOT){if(sym->st_shndx!=SHN_UNDEF)*ptr=(uintptr_t)m->load_virtbase+sym->st_value+rels[j].r_addend;}
            else if(type!=R_AARCH64_NONE){return -1;}
        }
    }
    Elf64_Xword off=dynamic_tag(m,DT_RELR),sz=dynamic_tag(m,DT_RELRSZ); if(!off){off=dynamic_tag(m,DT_ANDROID_RELR);sz=dynamic_tag(m,DT_ANDROID_RELRSZ);} if(off&&sz)process_relr(m,(const Elf64_Xword*)((uintptr_t)m->load_base+off),sz);
    return 0;
}

static uintptr_t lookup_export(so_module *m,const char *name){
    for(int i=0;i<m->num_syms;i++){
        Elf64_Sym *s=&m->syms[i]; if(s->st_shndx==SHN_UNDEF||ELF64_ST_BIND(s->st_info)==STB_LOCAL)continue;
        const char *n=m->dynstrtab+s->st_name; if(n[0]==name[0]&&!strcmp(n,name))return (uintptr_t)m->load_virtbase+s->st_value;
    } return 0;
}
void *so_resolve_external(const char *name){for(so_module *m=g_modules;m;m=m->next){uintptr_t a=lookup_export(m,name);if(a)return(void*)a;}return NULL;}
so_module *so_find_named(const char *needle){for(so_module*m=g_modules;m;m=m->next)if(strstr(m->name,needle))return m;return NULL;}
static uintptr_t resolve_one(so_module *self,DynLibFunction *f,int n,const char *name,uintptr_t (*extra_lookup)(const char*)){
    for(int i=0;i<n;i++)if(!strcmp(name,f[i].symbol))return f[i].func;
    for(so_module*m=g_modules;m;m=m->next)if(m!=self){uintptr_t a=lookup_export(m,name);if(a)return a;}
    if(extra_lookup){uintptr_t a=extra_lookup(name);if(a)return a;}
    return 0;
}
int so_resolve(so_module *m,DynLibFunction *funcs,int n,uintptr_t fallback,uintptr_t (*extra_lookup)(const char*)){
    int missing=0;
    for(int i=0;i<m->elf_hdr->e_shnum;i++){
        const char *sn=m->shstrtab+m->sec_hdr[i].sh_name; if(strcmp(sn,".rela.dyn")&&strcmp(sn,".rela.plt"))continue;
        Elf64_Rela *rels=(Elf64_Rela*)((uintptr_t)m->load_base+m->sec_hdr[i].sh_addr);int nr=m->sec_hdr[i].sh_size/sizeof(*rels);
        for(int j=0;j<nr;j++){
            int type=ELF64_R_TYPE(rels[j].r_info); if(type!=R_AARCH64_ABS64&&type!=R_AARCH64_GLOB_DAT&&type!=R_AARCH64_JUMP_SLOT)continue;
            Elf64_Sym *s=&m->syms[ELF64_R_SYM(rels[j].r_info)];if(s->st_shndx!=SHN_UNDEF)continue;
            uintptr_t *ptr=(uintptr_t*)((uintptr_t)m->load_base+rels[j].r_offset); const char *name=m->dynstrtab+s->st_name;
            uintptr_t a=resolve_one(m,funcs,n,name,extra_lookup);
            if(!a){

                if(ELF64_ST_BIND(s->st_info)==STB_WEAK){
                    *ptr=rels[j].r_addend;
                    continue;
                }
                a=fallback;missing++;
            }
            *ptr=a+rels[j].r_addend;
        }
    }
    return missing;
}

void so_finalize(so_module *m){
    Result rc=svcMapProcessCodeMemory(envGetOwnProcessHandle(),(u64)m->load_virtbase,(u64)m->load_base,m->load_size); if(R_FAILED(rc))fatal_error("svcMapProcessCodeMemory failed for %s: %08x",m->name,rc);
    size_t pages=m->load_size/0x1000; uint8_t *x=calloc(pages,1);if(!x)fatal_error("Out of memory finalizing %s",m->name);
    for(int i=0;i<m->phnum;i++)if(m->phdr[i].p_type==PT_LOAD&&(m->phdr[i].p_flags&PF_X)){
        size_t a=m->phdr[i].p_vaddr/0x1000,b=ALIGN_MEM(m->phdr[i].p_vaddr+m->phdr[i].p_memsz,0x1000)/0x1000;for(size_t p=a;p<b&&p<pages;p++)x[p]=1;
    }
    for(int want=1;want>=0;want--){for(size_t p=0;p<pages;){if(x[p]!=want){p++;continue;}size_t e=p+1;while(e<pages&&x[e]==want)e++;u64 a=(u64)m->load_virtbase+p*0x1000,sz=(e-p)*0x1000;rc=svcSetProcessMemoryPermission(envGetOwnProcessHandle(),a,sz,want?Perm_Rx:Perm_Rw);if(R_FAILED(rc))fatal_error("svcSetProcessMemoryPermission %s failed: %08x",m->name,rc);p=e;}}
    free(x);
    uintptr_t d=(uintptr_t)m->load_virtbase-(uintptr_t)m->load_base; m->syms=(Elf64_Sym*)((uintptr_t)m->syms+d);m->dynstrtab=(char*)((uintptr_t)m->dynstrtab+d);
    armICacheInvalidate(m->load_virtbase,m->load_size);
}
void so_execute_init_array(so_module *m){
    for(int i=0;i<m->elf_hdr->e_shnum;i++){
        const char*n=m->shstrtab+m->sec_hdr[i].sh_name;
        if(strcmp(n,".init_array"))continue;
        void(**a)(void)=(void*)((uintptr_t)m->load_virtbase+m->sec_hdr[i].sh_addr);
        int nr=(int)(m->sec_hdr[i].sh_size/sizeof(*a));
        for(int j=0;j<nr;j++){
            if(!a[j])continue;
            a[j]();
        }
    }
}
uintptr_t so_try_find_addr_rx(so_module*m,const char*symbol){return lookup_export(m,symbol);}
struct phdr_info_compat{Elf64_Addr dlpi_addr;const char*dlpi_name;const Elf64_Phdr*dlpi_phdr;Elf64_Half dlpi_phnum;};
int so_dl_iterate_phdr(int(*cb)(void*,size_t,void*),void*data){for(so_module*m=g_modules;m;m=m->next){struct phdr_info_compat i={(Elf64_Addr)m->load_virtbase,m->name,m->phdr,(Elf64_Half)m->phnum};int r=cb(&i,sizeof(i),data);if(r)return r;}return 0;}
