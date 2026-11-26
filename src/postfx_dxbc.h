#ifndef AZIMUTH_POSTFX_DXBC_H
#define AZIMUTH_POSTFX_DXBC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Narrow SM4/5 token transformation, not a general disassembler.
   Token layout: Microsoft DirectX-Headers D3D12TokenizedProgramFormat.hpp.
   Retail checksum: Microsoft hlsl-specs INF-0004, RFC 1321 compression.
   No game bytecode is included. All integers are decoded little-endian.
   This checksum is for the shader container, not security. */
static inline uint32_t azdx_u32(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline void azdx_put(unsigned char *p,uint32_t v) {
    p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);
}
static inline uint32_t azdx_rot(uint32_t v,unsigned s) { return (v<<s)|(v>>(32-s)); }
static inline void azdx_md5_block(uint32_t h[4],const unsigned char p[64]) {
    static const uint32_t k[64]={
      0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
      0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
      0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
      0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
      0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
      0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
      0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
      0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const unsigned shifts[16]={7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],x[16];
    for(unsigned i=0;i<16;++i)x[i]=azdx_u32(p+i*4);
    for(unsigned i=0;i<64;++i){
        uint32_t f;unsigned g;
        if(i<16){f=(b&c)|(~b&d);g=i;}
        else if(i<32){f=(d&b)|(~d&c);g=(5*i+1)&15;}
        else if(i<48){f=b^c^d;g=(3*i+5)&15;}
        else{f=c^(b|~d);g=(7*i)&15;}
        uint32_t t=d;d=c;c=b;b+=azdx_rot(a+f+k[i]+x[g],shifts[(i/16)*4+i%4]);a=t;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;
}
static inline void azdx_retail(const unsigned char *p,size_t n,unsigned char digest[16]) {
    uint32_t h[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    uint32_t count=(uint32_t)n;
    while(n>=64){azdx_md5_block(h,p);p+=64;n-=64;}
    unsigned char tail[64]={0};
    if(n>=56){memcpy(tail,p,n);tail[n]=128;azdx_md5_block(h,tail);memset(tail,0,64);}
    else{memcpy(tail+4,p,n);tail[4+n]=128;}
    azdx_put(tail,count<<3);azdx_put(tail+60,(count<<1)|1u);
    azdx_md5_block(h,tail);
    for(unsigned i=0;i<4;++i)azdx_put(digest+i*4,h[i]);
}
static inline int azdx_position_ftoi(const unsigned char *ins,uint32_t words) {
    if(words!=5||azdx_u32(ins)!=0x0500001bu)return 0; /* ftoi */
    uint32_t dst=azdx_u32(ins+4),src=azdx_u32(ins+12);
    /* rN.xy, v0.xy**. Unextended, immediate 1D indices. Only the two
       written channels matter; do not rewrite a different input register. */
    return dst==0x00100032u&&(src&0xfffff00fu)==0x00101006u&&
        ((src>>4)&15u)==4u&&azdx_u32(ins+16)==0;
}
static inline uint32_t azdx_instruction_words(const unsigned char *p,uint32_t remaining) {
    if(!remaining)return 0;
    uint32_t op=azdx_u32(p);
    uint32_t n=(op&0x7ffu)==53u?(remaining>=2?azdx_u32(p+4):0):(op>>24)&127u;
    return n&&n<=remaining?n:0;
}
/* Caller provides non-overlapping output with at least size+40*expected.
   Replace ftoi rN.xy,v0.xy** by mul rN.xy,v0.xy**,l(sx,sy,0,0);
   ftoi rN.xy,rN.xy**. Normalized UV and other instructions stay unchanged.
   Strictly require original retail hash, a valid container and exact
   expected match count before returning any usable shader. */
static inline size_t azdx_patch_positions(const void *input,size_t size,void *output,size_t capacity,
                                           float sx,float sy,unsigned expected) {
    const unsigned char *p=(const unsigned char *)input;
    unsigned char *q=(unsigned char *)output;
    if(!p||!q||p==q||size<32||size>1048576||!expected||expected>8||
       !(sx>0&&sx<=1&&sy>0&&sy<=1)||capacity<size+40u*expected||
       memcmp(p,"DXBC",4)||azdx_u32(p+20)!=1||azdx_u32(p+24)!=size)return 0;
    unsigned char hash[16];azdx_retail(p+20,size-20,hash);
    if(memcmp(hash,p+4,16))return 0;
    uint32_t parts=azdx_u32(p+28),off=0,bytes=0;
    if(!parts||parts>64||32u+4u*parts>size)return 0;
    for(uint32_t i=0;i<parts;++i){
        uint32_t o=azdx_u32(p+32+4*i);
        if(o<32+4*parts||o>size-8||(o&3))return 0;
        uint32_t n=azdx_u32(p+o+4);
        if(n>size-o-8)return 0;
        for(uint32_t j=0;j<i;++j){
            uint32_t x=azdx_u32(p+32+4*j),y=azdx_u32(p+x+4);
            if(o<x+8+y&&x<o+8+n)return 0;
        }
        if(!memcmp(p+o,"SHEX",4)||!memcmp(p+o,"SHDR",4)){
            if(off)return 0;
            off=o;bytes=n;
        }
    }
    if(!off||bytes<8||(bytes&3))return 0;
    const unsigned char *code=p+off+8;
    uint32_t version=azdx_u32(code),words=bytes/4;
    if((version>>16)!=0||((version>>4)&15u)<4||((version>>4)&15u)>5||
       azdx_u32(code+4)!=words)return 0;
    unsigned count=0;
    for(uint32_t i=2;i<words;){
        uint32_t n=azdx_instruction_words(code+4*i,words-i);
        if(!n)return 0;
        count+=(unsigned)azdx_position_ftoi(code+4*i,n);i+=n;
    }
    if(count!=expected)return 0;
    uint32_t extra=40u*expected;
    memcpy(q,p,off+16);
    azdx_put(q+off+4,bytes+extra);azdx_put(q+off+12,words+10u*expected);
    unsigned char *dst=q+off+16;
    uint32_t x,y;memcpy(&x,&sx,4);memcpy(&y,&sy,4);
    for(uint32_t i=2;i<words;){
        const unsigned char *ins=code+4*i;
        uint32_t n=azdx_instruction_words(ins,words-i);
        if(azdx_position_ftoi(ins,n)){
            uint32_t reg=azdx_u32(ins+8),src=azdx_u32(ins+12);
            uint32_t mul[10]={0x0a000038u,0x00100032u,reg,src,0,0x00004002u,x,y,0,0};
            for(unsigned j=0;j<10;++j)azdx_put(dst+4*j,mul[j]);
            dst+=40;memcpy(dst,ins,20);
            azdx_put(dst+12,src&~0x000ff000u);azdx_put(dst+16,reg);
        }else memcpy(dst,ins,4u*n);
        dst+=4u*n;i+=n;
    }
    size_t old_end=(size_t)off+8+bytes;
    memcpy(q+old_end+extra,p+old_end,size-old_end);
    for(uint32_t i=0;i<parts;++i){
        uint32_t o=azdx_u32(p+32+4*i);
        if(o>off)azdx_put(q+32+4*i,o+extra);
    }
    azdx_put(q+24,(uint32_t)size+extra);
    azdx_retail(q+20,size+extra-20,q+4);
    return size+extra;
}
#endif
