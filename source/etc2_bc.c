#include "etc2_bc.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * ETC2 block decode adapted from the MIT-licensed ETC decoder used by
 * K0lb3/texture2ddecoder (itself derived from Ishotihadus/mikunyan).
 * BC1/BC3 encoding and the streaming transcoder are local to this port.
 * See THIRD_PARTY_NOTICES.md.
 */

static const uint8_t kWriteOrder[16] = {
    0,4,8,12,1,5,9,13,2,6,10,14,3,7,11,15
};
static const uint8_t kWriteOrderRev[16] = {
    15,11,7,3,14,10,6,2,13,9,5,1,12,8,4,0
};
static const uint8_t kEtc1Modifier[8][2] = {
    {2,8},{5,17},{9,29},{13,42},{18,60},{24,80},{33,106},{47,183}
};
static const uint8_t kEtc1Subblock[2][16] = {
    {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1}
};
static const uint8_t kEtc2Distance[8] = {3,6,11,16,23,32,41,64};
static const int8_t kEtc2AlphaMod[16][8] = {
    {-3,-6,-9,-15,2,5,8,14}, {-3,-7,-10,-13,2,6,9,12},
    {-2,-5,-8,-13,1,4,7,12}, {-2,-4,-6,-13,1,3,5,12},
    {-3,-6,-8,-12,2,5,7,11}, {-3,-7,-9,-11,2,6,8,10},
    {-4,-7,-8,-11,3,6,7,10}, {-3,-5,-8,-11,2,4,7,10},
    {-2,-6,-8,-10,1,5,7,9},  {-2,-5,-8,-10,1,4,7,9},
    {-2,-4,-8,-10,1,3,7,9},  {-2,-5,-7,-10,1,4,6,9},
    {-3,-4,-7,-10,2,3,6,9},  {-1,-2,-3,-10,0,1,2,9},
    {-4,-6,-8,-9,3,5,7,8},   {-3,-5,-7,-9,2,4,6,8}
};

static inline uint8_t clamp_u8(int n){
    return (uint8_t)(n < 0 ? 0 : n > 255 ? 255 : n);
}

/* Packed as BGRA bytes on little-endian AArch64, matching the source decoder. */
static inline uint32_t pack_bgra(uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    return (uint32_t)b | ((uint32_t)g<<8) | ((uint32_t)r<<16) | ((uint32_t)a<<24);
}
static inline uint32_t apply_color(const uint8_t c[3],int m){
    return pack_bgra(clamp_u8((int)c[0]+m),clamp_u8((int)c[1]+m),clamp_u8((int)c[2]+m),255);
}
static inline uint32_t raw_color(const uint8_t c[3]){
    return pack_bgra(c[0],c[1],c[2],255);
}

static void decode_etc2_rgb_block(const uint8_t *data,uint32_t out[16]){
    uint16_t j=(uint16_t)((uint16_t)data[6]<<8)|data[7];
    uint32_t k=(uint32_t)((uint16_t)data[4]<<8)|data[5];
    uint8_t c[3][3]={{0}};

    if(data[3]&2){
        uint8_t r=data[0]&0xf8;
        int dr=((data[0]<<3)&0x18)-((data[0]<<3)&0x20);
        uint8_t g=data[1]&0xf8;
        int dg=((data[1]<<3)&0x18)-((data[1]<<3)&0x20);
        uint8_t b=data[2]&0xf8;
        int db=((data[2]<<3)&0x18)-((data[2]<<3)&0x20);

        if((int)r+dr<0 || (int)r+dr>255){
            c[0][0]=(uint8_t)(((data[0]<<3)&0xc0)|((data[0]<<4)&0x30)|((data[0]>>1)&0x0c)|(data[0]&3));
            c[0][1]=(uint8_t)((data[1]&0xf0)|(data[1]>>4));
            c[0][2]=(uint8_t)((data[1]&0x0f)|(data[1]<<4));
            c[1][0]=(uint8_t)((data[2]&0xf0)|(data[2]>>4));
            c[1][1]=(uint8_t)((data[2]&0x0f)|(data[2]<<4));
            c[1][2]=(uint8_t)((data[3]&0xf0)|(data[3]>>4));
            uint8_t d=kEtc2Distance[((data[3]>>1)&6)|(data[3]&1)];
            uint32_t set[4]={raw_color(c[0]),apply_color(c[1],d),raw_color(c[1]),apply_color(c[1],-(int)d)};
            k<<=1;
            for(int i=0;i<16;i++,j>>=1,k>>=1)out[kWriteOrder[i]]=set[(k&2)|(j&1)];
        }else if((int)g+dg<0 || (int)g+dg>255){
            c[0][0]=(uint8_t)(((data[0]<<1)&0xf0)|((data[0]>>3)&0x0f));
            c[0][1]=(uint8_t)(((data[0]<<5)&0xe0)|(data[1]&0x10)); c[0][1]|=c[0][1]>>4;
            c[0][2]=(uint8_t)((data[1]&8)|((data[1]<<1)&6)|(data[2]>>7)); c[0][2]|=c[0][2]<<4;
            c[1][0]=(uint8_t)(((data[2]<<1)&0xf0)|((data[2]>>3)&0x0f));
            c[1][1]=(uint8_t)(((data[2]<<5)&0xe0)|((data[3]>>3)&0x10)); c[1][1]|=c[1][1]>>4;
            c[1][2]=(uint8_t)(((data[3]<<1)&0xf0)|((data[3]>>3)&0x0f));
            uint8_t d=(uint8_t)((data[3]&4)|((data[3]<<1)&2));
            if(c[0][0]>c[1][0] || (c[0][0]==c[1][0] && (c[0][1]>c[1][1] || (c[0][1]==c[1][1] && c[0][2]>=c[1][2]))))++d;
            d=kEtc2Distance[d];
            uint32_t set[4]={apply_color(c[0],d),apply_color(c[0],-(int)d),apply_color(c[1],d),apply_color(c[1],-(int)d)};
            k<<=1;
            for(int i=0;i<16;i++,j>>=1,k>>=1)out[kWriteOrder[i]]=set[(k&2)|(j&1)];
        }else if((int)b+db<0 || (int)b+db>255){
            c[0][0]=(uint8_t)(((data[0]<<1)&0xfc)|((data[0]>>5)&3));
            c[0][1]=(uint8_t)(((data[0]<<7)&0x80)|(data[1]&0x7e)|(data[0]&1));
            c[0][2]=(uint8_t)(((data[1]<<7)&0x80)|((data[2]<<2)&0x60)|((data[2]<<3)&0x18)|((data[3]>>5)&4)); c[0][2]|=c[0][2]>>6;
            c[1][0]=(uint8_t)(((data[3]<<1)&0xf8)|((data[3]<<2)&4)|((data[3]>>5)&3));
            c[1][1]=(uint8_t)((data[4]&0xfe)|(data[4]>>7));
            c[1][2]=(uint8_t)(((data[4]<<7)&0x80)|((data[5]>>1)&0x7c)); c[1][2]|=c[1][2]>>6;
            c[2][0]=(uint8_t)(((data[5]<<5)&0xe0)|((data[6]>>3)&0x1c)|((data[5]>>1)&3));
            c[2][1]=(uint8_t)(((data[6]<<3)&0xf8)|((data[7]>>5)&6)|((data[6]>>4)&1));
            c[2][2]=(uint8_t)((data[7]<<2)|((data[7]>>4)&3));
            for(int y=0,i=0;y<4;y++)for(int x=0;x<4;x++,i++){
                uint8_t rr=clamp_u8((x*((int)c[1][0]-c[0][0])+y*((int)c[2][0]-c[0][0])+4*c[0][0]+2)>>2);
                uint8_t gg=clamp_u8((x*((int)c[1][1]-c[0][1])+y*((int)c[2][1]-c[0][1])+4*c[0][1]+2)>>2);
                uint8_t bb=clamp_u8((x*((int)c[1][2]-c[0][2])+y*((int)c[2][2]-c[0][2])+4*c[0][2]+2)>>2);
                out[i]=pack_bgra(rr,gg,bb,255);
            }
        }else{
            uint8_t code[2]={(uint8_t)(data[3]>>5),(uint8_t)((data[3]>>2)&7)};
            const uint8_t *table=kEtc1Subblock[data[3]&1];
            c[0][0]=(uint8_t)(r|(r>>5)); c[0][1]=(uint8_t)(g|(g>>5)); c[0][2]=(uint8_t)(b|(b>>5));
            c[1][0]=(uint8_t)((int)r+dr); c[1][1]=(uint8_t)((int)g+dg); c[1][2]=(uint8_t)((int)b+db);
            c[1][0]|=c[1][0]>>5; c[1][1]|=c[1][1]>>5; c[1][2]|=c[1][2]>>5;
            for(int i=0;i<16;i++,j>>=1,k>>=1){
                uint8_t s=table[i],m=kEtc1Modifier[code[s]][j&1];
                out[kWriteOrder[i]]=apply_color(c[s],(k&1)?-(int)m:(int)m);
            }
        }
    }else{
        uint8_t code[2]={(uint8_t)(data[3]>>5),(uint8_t)((data[3]>>2)&7)};
        const uint8_t *table=kEtc1Subblock[data[3]&1];
        c[0][0]=(uint8_t)((data[0]&0xf0)|(data[0]>>4)); c[1][0]=(uint8_t)((data[0]&0x0f)|(data[0]<<4));
        c[0][1]=(uint8_t)((data[1]&0xf0)|(data[1]>>4)); c[1][1]=(uint8_t)((data[1]&0x0f)|(data[1]<<4));
        c[0][2]=(uint8_t)((data[2]&0xf0)|(data[2]>>4)); c[1][2]=(uint8_t)((data[2]&0x0f)|(data[2]<<4));
        for(int i=0;i<16;i++,j>>=1,k>>=1){
            uint8_t s=table[i],m=kEtc1Modifier[code[s]][j&1];
            out[kWriteOrder[i]]=apply_color(c[s],(k&1)?-(int)m:(int)m);
        }
    }
}

static uint64_t read_be64(const uint8_t *p){
    uint64_t v=0;
    for(int i=0;i<8;i++)v=(v<<8)|p[i];
    return v;
}

static void decode_etc2_alpha_block(const uint8_t *data,uint32_t out[16]){
    if(data[1]&0xf0){
        uint8_t mul=(uint8_t)(data[1]>>4);
        const int8_t *table=kEtc2AlphaMod[data[1]&0x0f];
        uint64_t l=read_be64(data);
        for(int i=0;i<16;i++,l>>=3){
            uint8_t a=clamp_u8((int)data[0]+(int)mul*table[l&7]);
            uint32_t *p=&out[kWriteOrderRev[i]];
            *p=(*p&0x00ffffffu)|((uint32_t)a<<24);
        }
    }else{
        for(int i=0;i<16;i++)out[i]=(out[i]&0x00ffffffu)|((uint32_t)data[0]<<24);
    }
}

static inline uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){
    return (uint16_t)(((uint16_t)(r>>3)<<11)|((uint16_t)(g>>2)<<5)|(b>>3));
}
static inline void rgb_from_565(uint16_t c,uint8_t out[3]){
    out[0]=(uint8_t)(((c>>11)&31)*255/31);
    out[1]=(uint8_t)(((c>>5)&63)*255/63);
    out[2]=(uint8_t)((c&31)*255/31);
}
static inline void put16le(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static inline void put32le(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}

static void encode_bc1_color(const uint32_t px[16],uint8_t out[8]){
    int minr=255,ming=255,minb=255,maxr=0,maxg=0,maxb=0;
    for(int i=0;i<16;i++){
        int b=(int)(px[i]&255),g=(int)((px[i]>>8)&255),r=(int)((px[i]>>16)&255);
        if(r<minr)minr=r;
        if(r>maxr)maxr=r;
        if(g<ming)ming=g;
        if(g>maxg)maxg=g;
        if(b<minb)minb=b;
        if(b>maxb)maxb=b;
    }
    int ir=(maxr-minr)>>4,ig=(maxg-ming)>>4,ib=(maxb-minb)>>4;
    minr+=ir; ming+=ig; minb+=ib; maxr-=ir; maxg-=ig; maxb-=ib;
    uint16_t c0=rgb565((uint8_t)maxr,(uint8_t)maxg,(uint8_t)maxb);
    uint16_t c1=rgb565((uint8_t)minr,(uint8_t)ming,(uint8_t)minb);
    if(c0<c1){uint16_t t=c0;c0=c1;c1=t;}
    if(c0==c1){if(c0<0xffff)c0++;else if(c1)c1--;}
    uint8_t p[4][3];
    rgb_from_565(c0,p[0]); rgb_from_565(c1,p[1]);
    for(int k=0;k<3;k++){
        p[2][k]=(uint8_t)((2*(unsigned)p[0][k]+p[1][k]+1)/3);
        p[3][k]=(uint8_t)((p[0][k]+2*(unsigned)p[1][k]+1)/3);
    }
    uint32_t bits=0;
    for(int i=0;i<16;i++){
        int b=(int)(px[i]&255),g=(int)((px[i]>>8)&255),r=(int)((px[i]>>16)&255);
        int best=0,bestd=0x7fffffff;
        for(int q=0;q<4;q++){
            int dr=r-p[q][0],dg=g-p[q][1],db=b-p[q][2];
            int d=dr*dr+dg*dg+db*db;
            if(d<bestd){bestd=d;best=q;}
        }
        bits|=(uint32_t)best<<(2*i);
    }
    put16le(out,c0); put16le(out+2,c1); put32le(out+4,bits);
}

static void encode_bc3_alpha(const uint32_t px[16],uint8_t out[8]){
    uint8_t amin=255,amax=0;
    for(int i=0;i<16;i++){
        uint8_t a=(uint8_t)(px[i]>>24);
        if(a<amin)amin=a;
        if(a>amax)amax=a;
    }
    uint8_t pal[8]; pal[0]=amax; pal[1]=amin;
    if(amax>amin){
        for(int i=1;i<=6;i++)pal[i+1]=(uint8_t)(((7-i)*(unsigned)amax+i*(unsigned)amin+3)/7);
    }else{
        for(int i=1;i<=4;i++)pal[i+1]=(uint8_t)(((5-i)*(unsigned)amax+i*(unsigned)amin+2)/5);
        pal[6]=0;pal[7]=255;
    }
    uint64_t bits=0;
    for(int i=0;i<16;i++){
        int a=(int)(px[i]>>24),best=0,bestd=1000;
        for(int q=0;q<8;q++){
            int d=a-(int)pal[q];if(d<0)d=-d;
            if(d<bestd){bestd=d;best=q;}
        }
        bits|=(uint64_t)best<<(3*i);
    }
    out[0]=amax;out[1]=amin;
    for(int i=0;i<6;i++)out[2+i]=(uint8_t)(bits>>(8*i));
}

size_t etc2_bc_image_size(int mode,int width,int height){
    if(width<=0||height<=0)return 0;
    size_t bx=((size_t)width+3u)/4u,by=((size_t)height+3u)/4u;
    size_t block=(mode==ETC2_BC_RGB8)?8u:(mode==ETC2_BC_RGBA8)?16u:0u;
    if(!block||bx>SIZE_MAX/by)return 0;
    size_t n=bx*by;
    return n>SIZE_MAX/block?0:n*block;
}

int etc2_bc_transcode(void *dst,size_t dst_size,const void *src,size_t src_size,int width,int height,int mode){
    size_t need=etc2_bc_image_size(mode,width,height);
    if(!dst||!src||!need||dst_size<need||src_size<need)return 0;
    const uint8_t *s=(const uint8_t*)src;
    uint8_t *d=(uint8_t*)dst;
    size_t blocks=(((size_t)width+3u)/4u)*(((size_t)height+3u)/4u);
    uint32_t px[16];
    if(mode==ETC2_BC_RGB8){
        for(size_t i=0;i<blocks;i++,s+=8,d+=8){
            decode_etc2_rgb_block(s,px);
            encode_bc1_color(px,d);
        }
    }else if(mode==ETC2_BC_RGBA8){
        for(size_t i=0;i<blocks;i++,s+=16,d+=16){
            decode_etc2_rgb_block(s+8,px);
            decode_etc2_alpha_block(s,px);
            encode_bc3_alpha(px,d);
            encode_bc1_color(px,d+8);
        }
    }else return 0;
    return 1;
}
