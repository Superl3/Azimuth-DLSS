#include <assert.h>
#include <stdio.h>
#include "../src/debug_capture_policy.h"
int main(void){
    AzDebugWindow w={0};
    assert(!azdebug_begin(&w,0,100,8000));
    assert(!azdebug_begin(&w,65,100,8000));
    assert(!azdebug_begin(&w,1,100,0));
    assert(!azdebug_begin(&w,1,100,30001));
    assert(azdebug_begin(&w,3,100,8000));
    assert(w.active&&w.id==1&&w.observed==0);
    assert(!azdebug_begin(&w,3,200,8000));
    assert(azdebug_resolve(&w));assert(azdebug_resolve(&w));assert(azdebug_resolve(&w));
    assert(!azdebug_resolve(&w));assert(w.observed==3);
    assert(!azdebug_expired(&w,8099));assert(azdebug_expired(&w,8100));
    azdebug_end(&w);assert(!azdebug_resolve(&w));assert(!azdebug_expired(&w,10000));
    assert(azdebug_begin(&w,8,20000,8000));assert(w.id==2&&w.observed==0);
    assert(!azdebug_expired(&w,19999));assert(!azdebug_expired(&w,27999));
    assert(azdebug_expired(&w,28000));azdebug_end(&w);
    assert(azdebug_pass_kind(1,1,0,0,0,0,3,1)==2);
    assert(azdebug_pass_kind(0,0,0,1,1,0,6000,1)==0);
    assert(azdebug_pass_kind(0,1,0,1,1,1,342,1)==1);
    assert(azdebug_pass_kind(0,0,1,1,1,1,30,1)==1);
    assert(azdebug_pass_kind(0,0,0,0,0,0,3,1)==1);
    assert(azdebug_pass_kind(0,0,0,0,0,1,3,1)==0);
    puts("PASS: manual capture window and prioritized postprocess logging policy");
    return 0;
}
