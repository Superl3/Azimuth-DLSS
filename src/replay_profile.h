#ifndef AZIMUTH_REPLAY_PROFILE_H
#define AZIMUTH_REPLAY_PROFILE_H
#include <stdint.h>
#include <string.h>
#include "sr_experiment.h"
typedef struct {unsigned rw,rh,ow,oh;int wide;} AzReplayProfile;
/* Explicit, measured FH2 profiles only. Wide denotes dimensions, NOT HDR.
   The final PS identity decides SDR/PQ and the output signal is separate. */
static inline AzReplayProfile azrp_profile_for(const char *output){
    if(output&&!strcmp(output,"1920x1080"))return (AzReplayProfile){1280,720,1920,1080,0};
    if(output&&!strcmp(output,"3840x1600"))return (AzReplayProfile){2592,1080,3840,1600,1};
    return (AzReplayProfile){0,0,0,0,0};
}
/* Exact explicit profile, bounded to 4K and near-matching aspect. */
static inline int azrp_parse_size(const char *s,AzSRSize *o){
    unsigned v[2]={0,0};if(!s||!o)return 0;
    for(unsigned k=0;k<2;++k){
        if(*s<'1'||*s>'9')return 0;
        while(*s>='0'&&*s<='9'){if(v[k]>768u)return 0;v[k]=v[k]*10u+(unsigned)(*s++-'0');}
        if(k==0&&*s++!='x')return 0;
    }
    if(*s||v[0]<320||v[1]<200||v[0]>3840||v[1]>2160)return 0;
    *o=(AzSRSize){v[0],v[1]};return 1;
}
static inline AzReplayProfile azrp_profile_explicit(const char *render,const char *output){
    AzSRSize r={0},o={0};
    if(!azrp_parse_size(render,&r)||!azrp_parse_size(output,&o)||!azsr_less(r,o)||!azsr_same_aspect(r,o)||
       r.w*3u<o.w||r.h*3u<o.h)return (AzReplayProfile){0,0,0,0,0};
    return (AzReplayProfile){r.w,r.h,o.w,o.h,(uint64_t)o.w*o.h>1920ull*1080u};
}
static inline int azrp_match_render(AzReplayProfile p,uint64_t w,unsigned h){return p.rw&&w==p.rw&&h==p.rh;}
static inline int azrp_match_output(AzReplayProfile p,uint64_t w,unsigned h){return p.ow&&w==p.ow&&h==p.oh;}
/* Only one wide RAW group, leaving room for HDR t2 and full-size stages
   in the original 256 MiB cap; native and SR replay are still separate. */
static inline unsigned azrp_capture_groups(AzReplayProfile p){return p.wide?1u:2u;}
#endif
