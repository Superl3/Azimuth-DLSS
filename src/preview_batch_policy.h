#ifndef AZIMUTH_PREVIEW_BATCH_POLICY_H
#define AZIMUTH_PREVIEW_BATCH_POLICY_H
#include <stdint.h>
enum {PV_RECORDED=1,PV_SUBMITTED,PV_SAVED,PV_FAILED};
typedef struct {int state,current;} AzpvBatchLife;
static inline int azpv_can_append(AzpvBatchLife b,uint64_t old_recording,uint64_t recording) {
    return b.state==PV_RECORDED&&b.current&&recording&&old_recording==recording;
}
static inline void azpv_on_reset(AzpvBatchLife *b) {
    b->current=0;
    if(b->state==PV_RECORDED)b->state=PV_FAILED;
}
static inline void azpv_on_submit(AzpvBatchLife *b,unsigned matches,int signaled) {
    if(!matches||!b->current)return;
    b->state=(matches==1&&b->state==PV_RECORDED&&signaled)?PV_SUBMITTED:PV_FAILED;
}
static inline int azpv_can_map(AzpvBatchLife b,uint64_t completed) {
    return b.state==PV_SUBMITTED&&!b.current&&completed>=1&&completed!=UINT64_MAX;
}
/* A private SR snapshot is immutable only after the producer recording
   is retired and its fence completes, or earlier in the same recording. */
static inline int azpv_source_ready(AzpvBatchLife b,uint64_t producer,uint64_t consumer,int same_list,uint64_t completed) {
    if(same_list&&azpv_can_append(b,producer,consumer))return 1;
    return (b.state==PV_SUBMITTED||b.state==PV_SAVED)&&!b.current&&completed>=1&&completed!=UINT64_MAX;
}
static inline int azpv_source_waitable(AzpvBatchLife b,uint64_t completed){
    return b.state==PV_SUBMITTED&&!b.current&&completed==0;
}
/* A completed core fence alone must not retire a group still waiting
   for its consumer recording. Old groups cannot acquire more work
   once the global resolve has advanced; closed groups have seen final. */
static inline int azpv_group_retirable(uint64_t resolve,uint64_t retire_before,int closed){
    return resolve&&(closed||resolve<retire_before);
}
static inline int azpv_adjacent_work(uint64_t before,uint64_t after){
    return before&&before!=UINT64_MAX&&after==before+1;
}
#endif
