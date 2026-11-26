#ifndef AZIMUTH_VISIBLE_TAIL_POLICY_H
#define AZIMUTH_VISIBLE_TAIL_POLICY_H
#include <stdint.h>
/* Proof of the exact observed SDR tail. No generic "ignore clear".
   Stage 1: original composite completed.
   Stage 2: composite RT -> PS resource.
   Stage 3: different final resource COPY_SOURCE -> RT.
   Stage 4: whole final RTV cleared.
   Stage 5: immediately following fullscreen final draw.
   Stage 6: invalid. Unknown work/barriers invalidate this alternative.
   Caller proves identities, live views, recording and final shader. */
typedef struct {uint64_t start;uintptr_t composite,target;unsigned stage;} AzvisTail;
enum {AZTAIL_OTHER,AZTAIL_CLEAR,AZTAIL_FINAL};
static inline void azvis_tail_begin(AzvisTail *t,uint64_t work,uintptr_t composite){
    *t=(AzvisTail){work,composite,0,work&&work<UINT64_MAX-1&&composite?1u:6u};
}
static inline void azvis_tail_barrier(AzvisTail *t,uint64_t work,int transition,int full,uintptr_t res,unsigned before,unsigned after){
    if(!t->stage||t->stage>=5)return;
    if(work!=t->start||!transition||!full||!res){t->stage=6;return;}
    if(t->stage==1&&res==t->composite&&before==4&&after==128)t->stage=2;
    else if(t->stage==2&&res!=t->composite&&before==2048&&after==4){t->target=res;t->stage=3;}
    else t->stage=6;
}
static inline void azvis_tail_work(AzvisTail *t,uint64_t work,unsigned kind,uintptr_t target){
    if(!t->stage||t->stage>=6)return;
    if(t->stage==3&&work==t->start+1&&kind==AZTAIL_CLEAR&&target==t->target)t->stage=4;
    else if(t->stage==4&&work==t->start+2&&kind==AZTAIL_FINAL&&target==t->target)t->stage=5;
    else t->stage=6;
}
static inline int azvis_tail_proven(const AzvisTail *t,uint64_t start,uint64_t end,uintptr_t composite,uintptr_t target){
    return t->stage==5&&t->start==start&&start<UINT64_MAX-1&&end==start+2&&t->composite==composite&&t->target==target&&composite&&target&&composite!=target;
}
#endif
