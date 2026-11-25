#include <assert.h>
#include <stdio.h>
#include "../src/postfx_dxbc.h"
static void fixture(unsigned char *p,size_t size) {
    memset(p,0,size);memcpy(p,"DXBC",4);azdx_put(p+20,1);azdx_put(p+24,(uint32_t)size);
    azdx_put(p+28,2);azdx_put(p+32,40);azdx_put(p+36,76);
    memcpy(p+40,"SHEX",4);azdx_put(p+44,28);
    uint32_t tokens[]={0x50,7,0x0500001b,0x00100032,1,0x00101046,0};
    for(unsigned i=0;i<7;++i)azdx_put(p+48+4*i,tokens[i]);
    memcpy(p+76,"TEST",4);azdx_put(p+80,4);azdx_put(p+84,0x12345678);
    azdx_retail(p+20,size-20,p+4);
}
int main(void) {
    unsigned char src[88],dst[168],digest[16];fixture(src,sizeof(src));
    size_t n=azdx_patch_positions(src,sizeof(src),dst,sizeof(dst),2.0f/3.0f,2.0f/3.0f,1);
    assert(n==128&&azdx_u32(dst+24)==128&&azdx_u32(dst+36)==116);
    assert(azdx_u32(dst+44)==68&&azdx_u32(dst+52)==17);
    assert(azdx_u32(dst+56)==0x0a000038&&azdx_u32(dst+96)==0x0500001b);
    assert(azdx_u32(dst+108)==0x00100046&&azdx_u32(dst+112)==1);
    assert(!memcmp(dst+116,"TEST",4)&&azdx_u32(dst+124)==0x12345678);
    azdx_retail(dst+20,n-20,digest);assert(!memcmp(digest,dst+4,16));
    assert(!azdx_patch_positions(src,sizeof(src),dst,sizeof(dst),.5f,.5f,2));
    assert(!azdx_patch_positions(src,sizeof(src),dst,100,.5f,.5f,1));
    assert(!azdx_patch_positions(src,sizeof(src),dst,sizeof(dst),0,.5f,1));
    src[4]^=1;assert(!azdx_patch_positions(src,sizeof(src),dst,sizeof(dst),.5f,.5f,1));
    fixture(src,sizeof(src));azdx_put(src+56,0);azdx_retail(src+20,sizeof(src)-20,src+4);
    assert(!azdx_patch_positions(src,sizeof(src),dst,sizeof(dst),.5f,.5f,1));
    fixture(src,sizeof(src));azdx_put(src+36,44);azdx_retail(src+20,sizeof(src)-20,src+4);
    assert(!azdx_patch_positions(src,sizeof(src),dst,sizeof(dst),.5f,.5f,1));
    puts("PASS: DXBC position-remap token/container integrity and fail-closed gates");
    return 0;
}
