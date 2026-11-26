#include <assert.h>
#include <stdio.h>
#include "../src/replay_profile.h"
int main(void){
    AzReplayProfile s=azrp_profile_for("1920x1080"),w=azrp_profile_for("3840x1600");
    assert(azrp_match_render(s,1280,720)&&azrp_match_output(s,1920,1080)&&!s.wide);
    assert(azrp_match_render(w,2592,1080)&&azrp_match_output(w,3840,1600)&&w.wide);
    assert(w.rw*40==w.ow*27&&w.rh*40==w.oh*27);
    assert(!azrp_match_render(w,2560,1080)&&!azrp_match_output(w,1920,1080));
    assert(!azrp_profile_for("3840x2160").ow&&!azrp_profile_for(NULL).ow);
    assert(azrp_capture_groups(s)==2&&azrp_capture_groups(w)==1);
    uint64_t wide_bytes=4ull*(6ull*w.rw*w.rh+4ull*w.ow*w.oh);
    assert(wide_bytes<256ull*1024*1024);
    puts("PASS: explicit SDR/wide replay profiles, exact 27/40 coordinate scale, bounded wide RAW group");
    return 0;
}
