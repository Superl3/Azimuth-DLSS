#ifndef AZIMUTH_VISIBLE_LINEAGE_POLICY_H
#define AZIMUTH_VISIBLE_LINEAGE_POLICY_H
#include <stdint.h>
#include <string.h>
#define AZVIS_CHAINS 4u
#define AZVIS_BUFFERS 16u
/* Value-only creation observations. No COM object is owned or borrowed
   after the factory hook returns. Never dereference these integers.
   These identities cannot establish a live buffer or authorize GPU work. */
typedef struct {
    uintptr_t hwnd,chain,queue,buffers[AZVIS_BUFFERS];
    uint64_t generation;
    unsigned count,w,h;
} AzvisChainIdentity;
typedef struct {
    uintptr_t hwnd,chain,queue;
    uint64_t generation;
    unsigned index;
} AzvisBufferMatch;
/* A successful replacement invalidates prior observations on the same
   HWND or with the same chain address. A failed creation never calls
   this function. If registration cannot be recorded, fail observationally. */
static inline int azvis_register_chain(AzvisChainIdentity chains[AZVIS_CHAINS],const AzvisChainIdentity *v){
    if(!v->hwnd||!v->chain||!v->generation||v->count>AZVIS_BUFFERS)return 0;
    unsigned slot=AZVIS_CHAINS;
    for(unsigned i=0;i<AZVIS_CHAINS;++i){
        if(chains[i].generation&&(chains[i].hwnd==v->hwnd||chains[i].chain==v->chain)){
            if(chains[i].generation>=v->generation)return 0;
        }
    }
    for(unsigned i=0;i<AZVIS_CHAINS;++i){
        if(chains[i].generation&&(chains[i].hwnd==v->hwnd||chains[i].chain==v->chain))
            memset(&chains[i],0,sizeof(chains[i]));
        if(slot==AZVIS_CHAINS&&!chains[i].generation)slot=i;
    }
    if(slot==AZVIS_CHAINS)return 0;
    chains[slot]=*v;return 1;
}
static inline int azvis_match_buffer(const AzvisChainIdentity chains[AZVIS_CHAINS],uintptr_t res,AzvisBufferMatch *out){
    memset(out,0,sizeof(*out));
    if(!res)return 0;
    unsigned hits=0;AzvisBufferMatch m={0};
    for(unsigned i=0;i<AZVIS_CHAINS;++i){
        const AzvisChainIdentity *c=&chains[i];
        if(!c->generation||c->count>AZVIS_BUFFERS)continue;
        for(unsigned j=0;j<c->count;++j)if(c->buffers[j]==res){
            m.hwnd=c->hwnd;m.chain=c->chain;m.queue=c->queue;m.generation=c->generation;m.index=j;++hits;
        }
    }
    if(hits!=1)return 0;
    *out=m;return 1;
}
#endif
