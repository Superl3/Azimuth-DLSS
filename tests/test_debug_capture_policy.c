#include <assert.h>
#include <stdio.h>
#include "../src/debug_capture_policy.h"
int main(void){
    AzDebugWindow w={0};
    assert(!azdebug_auto_due(&w,0,1000));
    assert(!azdebug_auto_due(&w,1000001,1000001));
    assert(!azdebug_auto_due(&w,900,899));
    assert(azdebug_auto_due(&w,900,900));
    assert(azdebug_auto_due(&w,900,901));
    assert(!azdebug_begin(&w,0,100,8000));
    assert(!azdebug_begin(&w,65,100,8000));
    assert(!azdebug_begin(&w,1,100,0));
    assert(!azdebug_begin(&w,1,100,30001));
    assert(azdebug_begin(&w,3,100,8000));
    assert(w.active&&w.id==1&&w.observed==0);
    assert(!azdebug_auto_due(&w,900,900));
    assert(!azdebug_begin(&w,3,200,8000));
    assert(azdebug_resolve(&w));assert(azdebug_resolve(&w));assert(azdebug_resolve(&w));
    assert(!azdebug_resolve(&w));assert(w.observed==3);
    assert(!azdebug_expired(&w,8099));assert(azdebug_expired(&w,8100));
    azdebug_end(&w);assert(!azdebug_resolve(&w));assert(!azdebug_expired(&w,10000));
    assert(!azdebug_auto_due(&w,900,2000));
    assert(azdebug_begin(&w,8,20000,8000));assert(w.id==2&&w.observed==0);
    assert(!azdebug_expired(&w,19999));assert(!azdebug_expired(&w,27999));
    assert(azdebug_expired(&w,28000));azdebug_end(&w);
    assert(azdebug_pass_kind(1,1,0,0,0,0,3,1)==2);
    assert(azdebug_pass_kind(0,0,0,1,1,0,6000,1)==0);
    assert(azdebug_pass_kind(0,1,0,1,1,1,342,1)==1);
    assert(azdebug_pass_kind(0,0,1,1,1,1,30,1)==1);
    assert(azdebug_pass_kind(0,0,0,0,0,0,3,1)==1);
    assert(azdebug_pass_kind(0,0,0,0,0,1,3,1)==0);
    AzPreviewReady ready={0};
    assert(!azdebug_preview_ready(&ready,0,100));
    azdebug_preview_note(&ready,10,100,1);azdebug_preview_note(&ready,11,110,1);
    assert(!azdebug_preview_ready(&ready,11,115));
    azdebug_preview_note(&ready,12,120,1);
    assert(azdebug_preview_ready(&ready,11,121)&&azdebug_preview_ready(&ready,12,121));
    assert(!azdebug_preview_ready(&ready,13,121)&&!azdebug_preview_ready(&ready,12,621));
    azdebug_preview_note(&ready,13,130,0);assert(!azdebug_preview_ready(&ready,13,131));
    azdebug_preview_note(&ready,15,150,1);assert(ready.streak==1);
    AzDebugWindow auto_window={0};AzAutoScene scene={0};AzPreviewReady fresh={0};
    for(unsigned i=1;i<=9;++i){
        uint64_t now=100ull*i;
        azdebug_preview_note(&fresh,i,now,1);azdebug_auto_scene_note(&scene,i,now);
    }
    assert(scene.count==9);
    assert(!azdebug_auto_scene_due(&auto_window,&scene,&fresh,9,900,0,2000,1));
    for(unsigned i=10;i<=22;++i){
        uint64_t now=100ull*i;
        azdebug_preview_note(&fresh,i,now,1);azdebug_auto_scene_note(&scene,i,now);
    }
    assert(azdebug_auto_scene_due(&auto_window,&scene,&fresh,22,2200,0,2000,1));
    assert(!azdebug_auto_scene_due(&auto_window,&scene,&fresh,22,2200,0,2000,0));
    assert(!azdebug_auto_scene_due(&auto_window,&scene,&fresh,22,2200,23,2000,1));
    assert(!azdebug_auto_scene_due(&auto_window,&scene,&fresh,22,2800,0,2000,1));
    assert(azdebug_begin(&auto_window,8,2200,8000));azdebug_end(&auto_window);
    assert(!azdebug_auto_scene_due(&auto_window,&scene,&fresh,22,2200,0,2000,1));
    azdebug_auto_scene_note(&scene,28,2300);assert(scene.count==1&&scene.first_ms==2300);
    azdebug_auto_scene_note(&scene,28,2400);assert(scene.count==1&&scene.last_ms==2300);
    azdebug_auto_scene_note(&scene,29,3000);assert(scene.count==1&&scene.first_ms==3000);
    puts("PASS: manual and automatic capture windows and prioritized postprocess policy");
    return 0;
}
