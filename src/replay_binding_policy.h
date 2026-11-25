#ifndef AZIMUTH_REPLAY_BINDING_POLICY_H
#define AZIMUTH_REPLAY_BINDING_POLICY_H
#include <stdint.h>
#include <string.h>
#define AZRB_REQS 20u
#define AZRB_TABLES 32u
#define AZRB_HEAP_LIMIT 32768u
/* D3D12 Tier 3 permits up to one million descriptors in a shader-visible
   resource heap. Bound fallback memory; allocate the actual required count. */
#define AZRB_FALLBACK_LIMIT 1000000u
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
typedef enum {
    AZRB_FINISH_OK=0,AZRB_FINISH_INVALID,AZRB_FINISH_PS_T0,AZRB_FINISH_VS_T0,
    AZRB_FINISH_VS_ALIAS,AZRB_FINISH_ALIAS,AZRB_FINISH_MISSING,
    AZRB_FINISH_SRV_MODE,AZRB_FINISH_TABLE,AZRB_FINISH_SPAN,AZRB_FINISH_EMPTY
} AzrbFinish;
static inline const char *azrb_finish_name(AzrbFinish status) {
    switch(status){
        case AZRB_FINISH_OK:return "ok";
        case AZRB_FINISH_INVALID:return "layout_invalid";
        case AZRB_FINISH_PS_T0:return "ps_t0_not_table";
        case AZRB_FINISH_VS_T0:return "vs_t0_not_table";
        case AZRB_FINISH_VS_ALIAS:return "ps_vs_t0_alias";
        case AZRB_FINISH_ALIAS:return "ps_t0_alias";
        case AZRB_FINISH_MISSING:return "required_binding_missing";
        case AZRB_FINISH_SRV_MODE:return "srv_not_table";
        case AZRB_FINISH_TABLE:return "invalid_table";
        case AZRB_FINISH_SPAN:return "heap_span_limit";
        case AZRB_FINISH_EMPTY:return "empty_resource_tables";
    }
    return "unknown";
}
/* Same acceptance policy as azrb_finish; req_index is diagnostic only. */
static inline AzrbFinish azrb_finish_limit(const AzrbLayout *l,uint32_t base[AZRB_TABLES],uint32_t *total,uint32_t *mask,unsigned *req_index,uint32_t limit) {
    *req_index=UINT32_MAX;
    if(!l->valid)return AZRB_FINISH_INVALID;
    if(l->req[0].mode!=AZRB_TABLE){*req_index=0;return AZRB_FINISH_PS_T0;}
    if(l->req[16].mode!=AZRB_TABLE){*req_index=16;return AZRB_FINISH_VS_T0;}
    if(l->req[0].root==l->req[16].root&&l->req[0].offset==l->req[16].offset){*req_index=16;return AZRB_FINISH_VS_ALIAS;}
    /* No other required binding may alias the substituted PS t0 slot,
       including differently typed descriptors or shared stage tables. */
    for(unsigned i=1;i<AZRB_REQS;++i)
        if(l->req[i].mode==AZRB_TABLE&&l->req[i].root==l->req[0].root&&l->req[i].offset==l->req[0].offset){*req_index=i;return AZRB_FINISH_ALIAS;}
    *total=0;*mask=0;memset(base,0,AZRB_TABLES*sizeof(*base));
    for(unsigned i=0;i<AZRB_REQS;++i){
        const AzrbReq *r=&l->req[i];
        if(!r->mode){*req_index=i;return AZRB_FINISH_MISSING;}
        if(r->type==AZRB_SRV&&r->reg!=6&&r->mode!=AZRB_TABLE){*req_index=i;return AZRB_FINISH_SRV_MODE;}
        if(r->mode!=AZRB_TABLE)continue;
        if(r->root>=AZRB_TABLES||r->offset>=l->span[r->root]||!l->kind[r->root]){*req_index=i;return AZRB_FINISH_TABLE;}
        if(l->kind[r->root]!=1)continue;
        if(*mask&(1u<<r->root))continue;
        uint32_t n=l->span[r->root];
        if(!n||(limit&&n>limit-*total)){*req_index=i;return AZRB_FINISH_SPAN;}
        if(limit){base[r->root]=*total;*total+=n;}
        *mask|=1u<<r->root;
    }
    return *mask!=0?AZRB_FINISH_OK:AZRB_FINISH_EMPTY;
}
static inline AzrbFinish azrb_finish_detail(const AzrbLayout *l,uint32_t base[AZRB_TABLES],uint32_t *total,uint32_t *mask,unsigned *req_index) {
    return azrb_finish_limit(l,base,total,mask,req_index,AZRB_HEAP_LIMIT);
}
/* Rebase all referenced original table starts into a heap with the SAME
   physical capacity and indices. Unlike packing, shared/overlapping ranges
   need no duplicate span. Exact required slots still must fit. Reject
   physical aliasing of the replacement slot even across different tables. */
typedef enum {AZRB_REBASE_OK=0,AZRB_REBASE_LAYOUT,AZRB_REBASE_CAPACITY,AZRB_REBASE_BOUNDS,AZRB_REBASE_ALIAS} AzrbRebase;
static inline AzrbRebase azrb_rebase(const AzrbLayout *l,const uint32_t original[AZRB_TABLES],uint32_t heap_count,uint32_t *mask,unsigned *bad) {
    uint32_t unused[AZRB_TABLES],total=0;
    *bad=UINT32_MAX;
    if(azrb_finish_limit(l,unused,&total,mask,bad,0)!=AZRB_FINISH_OK)return AZRB_REBASE_LAYOUT;
    if(!heap_count||heap_count>AZRB_FALLBACK_LIMIT)return AZRB_REBASE_CAPACITY;
    uint32_t scene=UINT32_MAX;
    for(unsigned i=0;i<AZRB_REQS;++i){
        const AzrbReq *r=&l->req[i];
        if(r->mode!=AZRB_TABLE||l->kind[r->root]!=1)continue;
        uint32_t start=original[r->root];
        if(start>=heap_count||r->offset>=heap_count-start){*bad=i;return AZRB_REBASE_BOUNDS;}
        uint32_t slot=start+r->offset;
        if(i==0)scene=slot;
        else if(slot==scene){*bad=i;return AZRB_REBASE_ALIAS;}
    }
    return AZRB_REBASE_OK;
}
static inline const char *azrb_rebase_name(AzrbRebase r) {
    switch(r){
        case AZRB_REBASE_OK:return "ok";
        case AZRB_REBASE_LAYOUT:return "invalid_required_layout";
        case AZRB_REBASE_CAPACITY:return "heap_capacity";
        case AZRB_REBASE_BOUNDS:return "source_bounds";
        case AZRB_REBASE_ALIAS:return "physical_ps_t0_alias";
    }
    return "unknown";
}
static inline int azrb_finish(const AzrbLayout *l,uint32_t base[AZRB_TABLES],uint32_t *total,uint32_t *mask) {
    unsigned ignored;
    return azrb_finish_detail(l,base,total,mask,&ignored)==AZRB_FINISH_OK;
}
#endif
