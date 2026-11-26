#ifndef AZIMUTH_PRESENT_COPY_POLICY_H
#define AZIMUTH_PRESENT_COPY_POLICY_H
#include <stdint.h>
#include <stddef.h>
#define AZPC_SIDE 64u
#define AZPC_PITCH (AZPC_SIDE*4u)
#define AZPC_BYTES (AZPC_PITCH*AZPC_SIDE)
#define AZPC_WINDOW 16u
#define AZPC_MAX 128u
/* Byte-level fingerprint of two sampled regions, not a whole-image hash.
   Pitch is ignored outside the 64 texels in each row. */
static inline uint64_t azpc_hash(const unsigned char *p,size_t pitch){
    uint64_t h=UINT64_C(14695981039346656037);
    for(unsigned y=0;y<AZPC_SIDE;++y)for(unsigned x=0;x<AZPC_PITCH;++x){
        h^=p[(size_t)y*pitch+x];h*=UINT64_C(1099511628211);
    }
    return h;
}
static inline int azpc_window(uint64_t first,uint64_t current){
    return current>=first&&current-first<AZPC_WINDOW;
}
static inline int azpc_crop_ok(unsigned w,unsigned h){
    return w>=96&&h>=96;
}
#endif
