#ifndef AZIMUTH_HDR_REPLAY_POLICY_H
#define AZIMUTH_HDR_REPLAY_POLICY_H
#include <stdint.h>
/* t0, t1, t2 and optional table CB0 must have disjoint descriptor slots.
   Pack only the known shader's required resource tables. */
static inline int azhdr_distinct_slots(const unsigned *roots,const unsigned *offsets,unsigned count){
    if(count!=3u&&count!=4u)return 0;
    for(unsigned i=0;i<count;++i)for(unsigned j=0;j<i;++j)
        if(roots[i]==roots[j]&&offsets[i]==offsets[j])return 0;
    return 1;
}
/* Live replay is explicit; otherwise only a selected private probe may run. */
static inline int azhdr_route_mode(int requested,int selected,int contract,int copy,int recording,int draw){
    if(!contract||!copy||!recording||!draw)return 0;
    return requested?1:selected?2:0;
}
#endif
