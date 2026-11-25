#include "../src/visible_lineage_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    AzvisChainIdentity chains[AZVIS_CHAINS]={0},a={0};
    AzvisBufferMatch m;
    a.hwnd=11;a.chain=100;a.queue=50;a.count=2;a.buffers[0]=200;a.buffers[1]=201;a.generation=1;
    assert(azvis_register_chain(chains,&a));
    assert(azvis_match_buffer(chains,201,&m)&&m.chain==100&&m.generation==1&&m.index==1);
    /* Launcher's flip chain is replaced on the same HWND: only its
       numeric observation is invalidated. There is no reference to release. */
    AzvisChainIdentity b=a;b.chain=101;b.generation=2;b.buffers[0]=300;b.buffers[1]=301;
    assert(azvis_register_chain(chains,&b));
    assert(!azvis_match_buffer(chains,200,&m)&&!m.chain);
    assert(azvis_match_buffer(chains,300,&m)&&m.generation==2&&m.chain==101&&m.queue==50);
    assert(!azvis_register_chain(chains,&a));
    /* An address reused for a different HWND replaces the old observation. */
    b.hwnd=12;b.generation=3;b.buffers[0]=400;b.buffers[1]=401;
    assert(azvis_register_chain(chains,&b));
    assert(!azvis_match_buffer(chains,300,&m));
    a.generation=4;a.hwnd=13;a.chain=102;a.buffers[0]=400;a.buffers[1]=402;
    assert(azvis_register_chain(chains,&a));
    assert(!azvis_match_buffer(chains,400,&m)&&!m.chain); /* ambiguous */
    assert(azvis_match_buffer(chains,402,&m)&&m.hwnd==13);
    a.generation=5;a.hwnd=14;a.chain=103;a.buffers[0]=500;a.buffers[1]=501;
    assert(azvis_register_chain(chains,&a));
    a.generation=6;a.hwnd=15;a.chain=104;a.buffers[0]=600;a.buffers[1]=601;
    assert(azvis_register_chain(chains,&a));
    a.generation=7;a.hwnd=16;a.chain=105;
    assert(!azvis_register_chain(chains,&a));
    assert(!azvis_match_buffer(chains,0,&m));
    puts("PASS: value-only swapchain identity replacement, ambiguity and capacity");
    return 0;
}
