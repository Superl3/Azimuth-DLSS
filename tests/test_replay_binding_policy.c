#include <assert.h>
#include <stdio.h>
#include "../src/replay_binding_policy.h"
static AzrbLayout layout(void){
    AzrbLayout l;azrb_init(&l);
    azrb_range(&l,0,AZRB_PS,AZRB_SRV,0,0,12,0);
    azrb_direct(&l,1,AZRB_PS,AZRB_CBV,1,0,AZRB_DIRECT);
    azrb_direct(&l,2,AZRB_PS,AZRB_CBV,13,0,AZRB_DIRECT);
    azrb_direct(&l,0,AZRB_PS,AZRB_SAMPLER,0,0,AZRB_STATIC);
    azrb_direct(&l,0,AZRB_PS,AZRB_SAMPLER,1,0,AZRB_STATIC);
    azrb_range(&l,3,AZRB_VS,AZRB_SRV,0,0,UINT32_MAX,5);
    azrb_direct(&l,4,AZRB_VS,AZRB_CBV,0,0,AZRB_DIRECT);
    azrb_direct(&l,5,AZRB_VS,AZRB_CBV,1,0,AZRB_DIRECT);
    azrb_direct(&l,6,AZRB_VS,AZRB_CBV,13,0,AZRB_DIRECT);
    return l;
}
int main(void){
    uint32_t base[32],total,mask;AzrbLayout l=layout();
    assert(azrb_finish(&l,base,&total,&mask)&&total==18&&base[3]==12&&mask==9);
    l.req[16].root=l.req[0].root;l.req[16].offset=l.req[0].offset;
    assert(!azrb_finish(&l,base,&total,&mask));
    l=layout();l.req[3].root=l.req[0].root;l.req[3].offset=l.req[0].offset;assert(!azrb_finish(&l,base,&total,&mask));
    l=layout();l.req[3].mode=0;assert(!azrb_finish(&l,base,&total,&mask));
    l=layout();l.span[3]=AZRB_HEAP_LIMIT;assert(!azrb_finish(&l,base,&total,&mask));
    l=layout();azrb_range(&l,8,AZRB_ALL,AZRB_SRV,0,0,1,0);assert(!azrb_finish(&l,base,&total,&mask));
    unsigned req;
    l=layout();assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_OK&&req==UINT32_MAX);
    l.valid=0;assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_INVALID);
    l=layout();l.req[0].mode=AZRB_UNBOUND;assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_PS_T0&&req==0);
    l=layout();l.req[16].mode=AZRB_UNBOUND;assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_VS_T0&&req==16);
    l=layout();l.req[16].root=l.req[0].root;l.req[16].offset=l.req[0].offset;
    assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_VS_ALIAS&&req==16);
    l=layout();l.req[3].offset=0;assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_ALIAS&&req==3);
    l=layout();l.req[3].mode=AZRB_UNBOUND;assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_MISSING&&req==3);
    l=layout();l.req[3].mode=AZRB_DIRECT;assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_SRV_MODE&&req==3);
    l=layout();l.req[3].offset=12;assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_TABLE&&req==3);
    l=layout();l.span[3]=AZRB_HEAP_LIMIT;assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_SPAN&&req==16);
    /* Large declared extents need not require disjoint packed copies.
       Physical rebase preserves original capacity and indices; extended
       packing can instead separate a physical alias between tables. */
    uint32_t original[AZRB_TABLES]={0};
    original[0]=20;original[3]=100;
    l=layout();
    assert(azrb_rebase(&l,original,256,&mask,&req)==AZRB_REBASE_OK);
    l.span[0]=40000;
    assert(azrb_finish_detail(&l,base,&total,&mask,&req)==AZRB_FINISH_SPAN);
    assert(azrb_rebase(&l,original,50000,&mask,&req)==AZRB_REBASE_OK);
    assert(azrb_finish_limit(&l,base,&total,&mask,&req,AZRB_FALLBACK_LIMIT)==AZRB_FINISH_OK&&total==40006);
    original[3]=15;
    assert(azrb_rebase(&l,original,50000,&mask,&req)==AZRB_REBASE_ALIAS&&req==16);
    assert(azrb_finish_limit(&l,base,&total,&mask,&req,AZRB_FALLBACK_LIMIT)==AZRB_FINISH_OK);
    original[3]=UINT32_MAX;
    assert(azrb_rebase(&l,original,50000,&mask,&req)==AZRB_REBASE_BOUNDS&&req==16);
    assert(azrb_rebase(&l,original,AZRB_FALLBACK_LIMIT+1,&mask,&req)==AZRB_REBASE_CAPACITY);
    l.req[3].mode=AZRB_UNBOUND;
    assert(azrb_rebase(&l,original,50000,&mask,&req)==AZRB_REBASE_LAYOUT&&req==3);
    assert(azrb_finish_limit(&l,base,&total,&mask,&req,AZRB_FALLBACK_LIMIT)==AZRB_FINISH_MISSING);
    l=layout();l.valid=0;
    assert(azrb_rebase(&l,original,50000,&mask,&req)==AZRB_REBASE_LAYOUT);
    puts("PASS: exact bindings, legacy/physical/extended plans, bounds, aliases and fail-closed diagnostics");
    return 0;
}
