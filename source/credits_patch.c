#include "credits_patch.h"
#include "credits_patch_data.h"
#include "config.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(g_credits_blockinfo)==CREDITS_NEW_CBI_SIZE, "credits blockinfo size mismatch");
_Static_assert(sizeof(g_credits_block325)==CREDITS_NEW_BLOCK_SIZE, "credits block size mismatch");

static uint32_t be32_load(const unsigned char *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static uint64_t be64_load(const unsigned char *p) {
    return ((uint64_t)p[0]<<56)|((uint64_t)p[1]<<48)|((uint64_t)p[2]<<40)|((uint64_t)p[3]<<32)|
           ((uint64_t)p[4]<<24)|((uint64_t)p[5]<<16)|((uint64_t)p[6]<<8)|(uint64_t)p[7];
}

static void be32_store(unsigned char *p, uint32_t v) {
    p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16);
    p[2]=(unsigned char)(v>>8);  p[3]=(unsigned char)v;
}
static void be64_store(unsigned char *p, uint64_t v) {
    p[0]=(unsigned char)(v>>56); p[1]=(unsigned char)(v>>48);
    p[2]=(unsigned char)(v>>40); p[3]=(unsigned char)(v>>32);
    p[4]=(unsigned char)(v>>24); p[5]=(unsigned char)(v>>16);
    p[6]=(unsigned char)(v>>8);  p[7]=(unsigned char)v;
}

static int copy_exact(FILE *src, FILE *dst, uint64_t bytes) {
    unsigned char *buf=(unsigned char*)malloc(256u*1024u);
    if(!buf)return -1;
    while(bytes){
        size_t n=bytes>256u*1024u?256u*1024u:(size_t)bytes;
        if(fread(buf,1,n,src)!=n || fwrite(buf,1,n,dst)!=n){free(buf);return -1;}
        bytes-=n;
    }
    free(buf);
    return 0;
}

int credits_patch_apply(void) {
    const char *path=DATA_ROOT "/assets/bin/Data/data.unity3d";
    const char *tmp =DATA_ROOT "/assets/bin/Data/data.unity3d.porttmp";
    const char *bak =DATA_ROOT "/assets/bin/Data/data.unity3d.portbak";
    struct stat st;
    if(stat(path,&st)!=0){

        return 0;
    }
    if((uint64_t)st.st_size==CREDITS_NEW_TOTAL){

        return 0;
    }
    if((uint64_t)st.st_size!=CREDITS_OLD_TOTAL){

        return 0;
    }

    FILE *src=fopen(path,"rb");
    if(!src){return 0;}
    unsigned char header[64];
    if(fread(header,1,sizeof(header),src)!=sizeof(header) || memcmp(header,"UnityFS\0",8)!=0 ||
       be64_load(header+CREDITS_HEADER_TOTAL_OFF)!=CREDITS_OLD_TOTAL ||
       be32_load(header+CREDITS_HEADER_CBI_OFF)!=19462u){
        fclose(src); return 0;
    }

    be64_store(header+CREDITS_HEADER_TOTAL_OFF,CREDITS_NEW_TOTAL);
    be32_store(header+CREDITS_HEADER_CBI_OFF,CREDITS_NEW_CBI_SIZE);

    remove(tmp); remove(bak);
    FILE *dst=fopen(tmp,"wb");
    if(!dst){fclose(src);return 0;}
    int ok=1;
    if(fwrite(header,1,sizeof(header),dst)!=sizeof(header))ok=0;
    if(ok && fwrite(g_credits_blockinfo,1,sizeof(g_credits_blockinfo),dst)!=sizeof(g_credits_blockinfo))ok=0;
    if(ok){
        long here=ftell(dst);
        if(here<0)ok=0;
        else{
            static const unsigned char zeros[16]={0};
            while(ok && (uint64_t)here<CREDITS_NEW_DATA_POS){
                size_t n=(size_t)(CREDITS_NEW_DATA_POS-(uint64_t)here);
                if(n>sizeof(zeros))n=sizeof(zeros);
                if(fwrite(zeros,1,n,dst)!=n)ok=0;
                here+=n;
            }
        }
    }
    if(ok && fseeko(src,(off_t)CREDITS_OLD_DATA_POS,SEEK_SET)!=0)ok=0;
    if(ok && copy_exact(src,dst,CREDITS_OLD_BLOCK_POS-CREDITS_OLD_DATA_POS)<0)ok=0;
    if(ok && fwrite(g_credits_block325,1,sizeof(g_credits_block325),dst)!=sizeof(g_credits_block325))ok=0;
    if(ok && fseeko(src,(off_t)CREDITS_OLD_AFTER_BLOCK,SEEK_SET)!=0)ok=0;
    if(ok && copy_exact(src,dst,CREDITS_OLD_TOTAL-CREDITS_OLD_AFTER_BLOCK)<0)ok=0;
    if(ok && fflush(dst)!=0)ok=0;
    if(ok && fsync(fileno(dst))!=0)ok=0;
    if(fclose(dst)!=0)ok=0;
    fclose(src);

    if(!ok){

        remove(tmp);
        return 0;
    }
    if(stat(tmp,&st)!=0 || (uint64_t)st.st_size!=CREDITS_NEW_TOTAL){

        remove(tmp); return 0;
    }

    if(rename(path,bak)!=0){

        remove(tmp); return 0;
    }
    if(rename(tmp,path)!=0){
        (void)rename(bak,path);

        remove(tmp); return 0;
    }
    remove(bak);

    return 0;
}
