#ifndef AZIMUTH_REPLAY_PROFILE_H
#define AZIMUTH_REPLAY_PROFILE_H
#include <stdint.h>
#include <string.h>
typedef struct {unsigned rw,rh,ow,oh;int wide;} AzReplayProfile;
/* Explicit, measured FH2 profiles only. Wide denotes dimensions, NOT HDR.
   The final PS identity decides SDR/PQ and the output signal is separate. */
static inline AzReplayProfile azrp_profile_for(const char *output){
    if(output&&!strcmp(output,"1920x1080"))return (AzReplayProfile){1280,720,1920,1080,0};
    if(output&&!strcmp(output,"3840x1600"))return (AzReplayProfile){2592,1080,3840,1600,1};
    return (AzReplayProfile){0,0,0,0,0};
}
static inline int azrp_match_render(AzReplayProfile p,uint64_t w,unsigned h){return p.rw&&w==p.rw&&h==p.rh;}
static inline int azrp_match_output(AzReplayProfile p,uint64_t w,unsigned h){return p.ow&&w==p.ow&&h==p.oh;}
/* Only one wide RAW group, leaving room for HDR t2 and full-size stages
   in the original 256 MiB cap; native and SR replay are still separate. */
static inline unsigned azrp_capture_groups(AzReplayProfile p){return p.wide?1u:2u;}
#endif
