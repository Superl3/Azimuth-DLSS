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
    l=layout();l.req[3].mode=0;assert(!azrb_finish(&l,base,&total,&mask));
    l=layout();l.span[3]=AZRB_HEAP_LIMIT;assert(!azrb_finish(&l,base,&total,&mask));
    l=layout();azrb_range(&l,8,AZRB_ALL,AZRB_SRV,0,0,1,0);assert(!azrb_finish(&l,base,&total,&mask));
    puts("PASS: exact PS/VS bindings, separate t0, bounded tables and reject ambiguity");
    return 0;
}
