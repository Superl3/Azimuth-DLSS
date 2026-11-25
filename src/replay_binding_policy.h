#ifndef AZIMUTH_REPLAY_BINDING_POLICY_H
#define AZIMUTH_REPLAY_BINDING_POLICY_H
#include <stdint.h>
#include <string.h>
#define AZRB_REQS 20u
#define AZRB_TABLES 32u
#define AZRB_HEAP_LIMIT 32768u
enum {AZRB_SRV=0,AZRB_UAV=1,AZRB_CBV=2,AZRB_SAMPLER=3};
enum {AZRB_ALL=0,AZRB_VS=1,AZRB_PS=5};
enum {AZRB_UNBOUND=0,AZRB_TABLE=1,AZRB_DIRECT=2,AZRB_STATIC=3};
typedef struct {unsigned stage,type,reg,mode,root,offset;} AzrbReq;
typedef struct {
    AzrbReq req[AZRB_REQS];
    uint32_t span[AZRB_TABLES];
    unsigned kind[AZRB_TABLES]; /* 1 resource table, 2 sampler table */
    int valid;
} AzrbLayout;
static inline void azrb_init(AzrbLayout *l) {
    memset(l,0,sizeof(*l));l->valid=1;
    unsigned j=0;
    for(unsigned i=0;i<12;++i)l->req[j++]=(AzrbReq){AZRB_PS,AZRB_SRV,i,0,0,0};
    l->req[j++]=(AzrbReq){AZRB_PS,AZRB_CBV,1,0,0,0};
    l->req[j++]=(AzrbReq){AZRB_PS,AZRB_CBV,13,0,0,0};
    l->req[j++]=(AzrbReq){AZRB_PS,AZRB_SAMPLER,0,0,0,0};
    l->req[j++]=(AzrbReq){AZRB_PS,AZRB_SAMPLER,1,0,0,0};
    l->req[j++]=(AzrbReq){AZRB_VS,AZRB_SRV,0,0,0,0};
    l->req[j++]=(AzrbReq){AZRB_VS,AZRB_CBV,0,0,0,0};
    l->req[j++]=(AzrbReq){AZRB_VS,AZRB_CBV,1,0,0,0};
    l->req[j++]=(AzrbReq){AZRB_VS,AZRB_CBV,13,0,0,0};
}
static inline void azrb_assign(AzrbLayout *l,AzrbReq *r,unsigned mode,unsigned root,unsigned offset) {
    if(r->mode){l->valid=0;return;}
    r->mode=mode;r->root=root;r->offset=offset;
}
static inline int azrb_vis(unsigned vis,unsigned stage){return vis==AZRB_ALL||vis==stage;}
static inline void azrb_range(AzrbLayout *l,unsigned root,unsigned vis,unsigned type,unsigned reg,unsigned space,
                              uint32_t count,uint32_t offset) {
    if(root>=AZRB_TABLES||type>AZRB_SAMPLER||!count||offset==UINT32_MAX){l->valid=0;return;}
    unsigned kind=type==AZRB_SAMPLER?2u:1u;
    if(l->kind[root]&&l->kind[root]!=kind){l->valid=0;return;}
    l->kind[root]=kind;
    /* For unbounded ranges, the actual required slots determine the span.
       A finite table reserves its full declared extent. */
    uint64_t end=(uint64_t)offset+(count==UINT32_MAX?1u:count);
    if(end>UINT32_MAX){l->valid=0;return;}
    if(end>l->span[root])l->span[root]=(uint32_t)end;
    if(space)return;
    for(unsigned i=0;i<AZRB_REQS;++i){
        AzrbReq *r=&l->req[i];
        if(r->type!=type||!azrb_vis(vis,r->stage)||r->reg<reg||r->reg-reg>=count)continue;
        uint64_t slot=(uint64_t)offset+r->reg-reg;
        if(slot>=UINT32_MAX){l->valid=0;continue;}
        azrb_assign(l,r,AZRB_TABLE,root,(unsigned)slot);
        if(slot+1>l->span[root])l->span[root]=(uint32_t)(slot+1);
    }
}
static inline void azrb_direct(AzrbLayout *l,unsigned root,unsigned vis,unsigned type,unsigned reg,unsigned space,unsigned mode) {
    if(space)return;
    for(unsigned i=0;i<AZRB_REQS;++i){
        AzrbReq *r=&l->req[i];
        if(r->type==type&&r->reg==reg&&azrb_vis(vis,r->stage))azrb_assign(l,r,mode,root,0);
    }
}
/* Exact shader pair: pixel t0 is scene color; vertex t0 is a different
   exposure texture. Never substitute an alias used by the VS. */
static inline int azrb_finish(const AzrbLayout *l,uint32_t base[AZRB_TABLES],uint32_t *total,uint32_t *mask) {
    if(!l->valid||l->req[0].mode!=AZRB_TABLE||l->req[16].mode!=AZRB_TABLE||
       (l->req[0].root==l->req[16].root&&l->req[0].offset==l->req[16].offset))return 0;
    *total=0;*mask=0;memset(base,0,AZRB_TABLES*sizeof(*base));
    for(unsigned i=0;i<AZRB_REQS;++i){
        const AzrbReq *r=&l->req[i];
        if(!r->mode||(r->type==AZRB_SRV&&r->reg!=6&&r->mode!=AZRB_TABLE))return 0;
        if(r->mode!=AZRB_TABLE)continue;
        if(r->root>=AZRB_TABLES||r->offset>=l->span[r->root]||!l->kind[r->root])return 0;
        if(l->kind[r->root]!=1)continue;
        if(*mask&(1u<<r->root))continue;
        uint32_t n=l->span[r->root];
        if(!n||n>AZRB_HEAP_LIMIT-*total)return 0;
        base[r->root]=*total;*total+=n;*mask|=1u<<r->root;
    }
    return *total!=0;
}
#endif
