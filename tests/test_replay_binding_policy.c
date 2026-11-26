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
    uint32_t base[AZRB_TABLES],total;uint64_t mask;AzrbLayout l=layout();
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
    /* The independent schema resolver can recover a valid physical
       plan when an unrelated table range defeated full-span parsing. */
    AzrbSchemaRow schema[]={
        {0,AZRB_PS,AZRB_SRV,0,0,12,0,AZRB_TABLE},
        {1,AZRB_PS,AZRB_CBV,1,0,1,0,AZRB_DIRECT},
        {2,AZRB_PS,AZRB_CBV,13,0,1,0,AZRB_DIRECT},
        {0,AZRB_PS,AZRB_SAMPLER,0,0,1,0,AZRB_STATIC},
        {1,AZRB_PS,AZRB_SAMPLER,1,0,1,0,AZRB_STATIC},
        {3,AZRB_VS,AZRB_SRV,0,0,UINT32_MAX,5,AZRB_TABLE},
        {4,AZRB_VS,AZRB_CBV,0,0,1,0,AZRB_DIRECT},
        {5,AZRB_VS,AZRB_CBV,1,0,1,0,AZRB_DIRECT},
        {6,AZRB_VS,AZRB_CBV,13,0,1,0,AZRB_DIRECT},
        {7,AZRB_PS,AZRB_SRV,0,1,UINT32_MAX,UINT32_MAX,AZRB_TABLE}
    };
    unsigned rows=(unsigned)(sizeof(schema)/sizeof(schema[0]));
    original[3]=100;
    assert(azrb_project_schema(&l,schema,rows,8,0,0,&req));
    assert(azrb_rebase(&l,original,256,&mask,&req)==AZRB_REBASE_OK);
    assert(!azrb_project_schema(&l,schema,rows,8,0,1,&req));
    assert(!azrb_project_schema(&l,schema,rows,8,0x400,0,&req));
    assert(!azrb_project_schema(&l,schema,rows,65,0,0,&req));
    schema[5].offset=UINT32_MAX;
    assert(!azrb_project_schema(&l,schema,rows,8,0,0,&req)&&req==16);
    schema[5].offset=5;
    schema[0].count=11;
    assert(!azrb_project_schema(&l,schema,rows,8,0,0,&req)&&req==11);
    schema[0].count=12;
    schema[9]=(AzrbSchemaRow){7,AZRB_PS,AZRB_SRV,0,0,1,0,AZRB_TABLE};
    assert(!azrb_project_schema(&l,schema,rows,8,0,0,&req)&&req==0);
    /* Capture-derived required table positions from Resolve 3668:
       VS t0=root1, PS t0=root17, PS s0/s1=root18, PS b13=root19.
       The previous 32-entry logger omitted roots 32..39. Synthetic
       high indices exercise both a direct b1 and a high table without
       guessing the real unobserved root number. */
    AzrbLayout large;azrb_init(&large);
    azrb_range(&large,17,AZRB_PS,AZRB_SRV,0,0,128,0);
    azrb_range(&large,1,AZRB_VS,AZRB_SRV,0,0,128,0);
    azrb_range(&large,18,AZRB_PS,AZRB_SAMPLER,0,0,16,0);
    azrb_direct(&large,39,AZRB_PS,AZRB_CBV,1,0,AZRB_DIRECT);
    azrb_direct(&large,19,AZRB_PS,AZRB_CBV,13,0,AZRB_CONSTANT);
    azrb_direct(&large,27,AZRB_VS,AZRB_CBV,0,0,AZRB_DIRECT);
    azrb_direct(&large,21,AZRB_VS,AZRB_CBV,1,0,AZRB_DIRECT);
    azrb_direct(&large,3,AZRB_VS,AZRB_CBV,13,0,AZRB_CONSTANT);
    assert(azrb_finish(&large,base,&total,&mask)&&total==256&&large.req[12].root==39);
    assert(large.req[13].mode==AZRB_CONSTANT);
    large.req[16].root=63;large.span[63]=128;large.kind[63]=1;
    assert(azrb_finish(&large,base,&total,&mask)&&(mask&azrb_bit(63)));
    assert(!azrb_bit(64));
    const uint64_t declared=UINT64_C(0x177777)|azrb_bit(40);
    const uint64_t bound=UINT64_C(0x60002);
    assert(azrb_restore_mask_ok(1,0,declared,bound,bound));
    assert(azrb_restore_mask_ok(1,0,15,14,14));
    assert(!azrb_restore_mask_ok(0,0,declared,bound,bound));
    assert(!azrb_restore_mask_ok(1,1,declared,bound,bound));
    assert(!azrb_restore_mask_ok(1,0,declared,bound|azrb_bit(63),UINT64_MAX));
    assert(!azrb_restore_mask_ok(1,0,declared,bound,bound&~azrb_bit(17)));
    assert(azrb_restore_mask_ok(1,0,declared,bound|azrb_bit(40),bound|azrb_bit(40)));
    assert(azrb_words(0,64)==UINT64_MAX&&azrb_words(63,1)==azrb_bit(63));
    assert(!azrb_words(63,2)&&!azrb_words(64,1)&&!azrb_words(0,0));
    puts("PASS: 64-root layouts, exact bindings, independent plans, complete-epoch table restore and direct root argument masks");
    return 0;
}
