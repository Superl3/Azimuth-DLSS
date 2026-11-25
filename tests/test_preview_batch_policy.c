#include <assert.h>
#include <stdio.h>
#include "../src/preview_batch_policy.h"
int main(void) {
    AzpvBatchLife core={PV_RECORDED,1},post={PV_RECORDED,1};
    assert(azpv_can_append(core,1,1));
    assert(!azpv_can_append(core,1,2));
    assert(!azpv_can_append(core,0,0));
    azpv_on_submit(&core,1,1);
    assert(core.state==PV_SUBMITTED&&!azpv_can_append(core,1,1));
    assert(!azpv_can_map(core,1));
    azpv_on_reset(&core);
    assert(azpv_can_map(core,1)&&!azpv_can_map(core,0)&&!azpv_can_map(core,UINT64_MAX));
    assert(azpv_can_append(post,2,2));
    azpv_on_submit(&post,1,1);azpv_on_reset(&post);
    assert(azpv_can_map(post,1));
    AzpvBatchLife repeat={PV_RECORDED,1};
    azpv_on_submit(&repeat,1,1);azpv_on_submit(&repeat,1,1);azpv_on_reset(&repeat);
    assert(repeat.state==PV_FAILED&&!azpv_can_map(repeat,1));
    AzpvBatchLife abandoned={PV_RECORDED,1};azpv_on_reset(&abandoned);
    assert(abandoned.state==PV_FAILED);
    AzpvBatchLife signal={PV_RECORDED,1};azpv_on_submit(&signal,1,0);
    assert(signal.state==PV_FAILED);
    AzpvBatchLife multiple={PV_RECORDED,1};azpv_on_submit(&multiple,2,1);
    assert(multiple.state==PV_FAILED);
    assert(azpv_source_ready((AzpvBatchLife){PV_RECORDED,1},5,5,1,0));
    assert(!azpv_source_ready((AzpvBatchLife){PV_SUBMITTED,1},5,6,1,1));
    assert(!azpv_source_ready((AzpvBatchLife){PV_SUBMITTED,0},5,6,1,0));
    assert(azpv_source_ready((AzpvBatchLife){PV_SUBMITTED,0},5,6,1,1));
    assert(!azpv_source_ready((AzpvBatchLife){PV_FAILED,0},5,6,1,1));
    assert(!azpv_group_retirable(671,671,0));
    assert(azpv_group_retirable(671,672,0));
    assert(azpv_group_retirable(671,671,1));
    assert(!azpv_group_retirable(0,672,1));
    assert(azpv_adjacent_work(88,89));
    assert(!azpv_adjacent_work(88,90));
    assert(!azpv_adjacent_work(UINT64_MAX,0));
    puts("PASS: independent recording batches, reset, submit, repeated submit, immutable SR source and fence policy");
    return 0;
}
