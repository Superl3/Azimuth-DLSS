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
    puts("PASS: manual capture start/rearm/resolve interval/timeout policy");
    return 0;
}
