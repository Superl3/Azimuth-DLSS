#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include "../src/sr_route_policy.h"
int main(void) {
    AzSRSize r={2560,1067}, o={3840,1600};
    AzSRFrame f={r,r,r,r,o,1,1,1};
    assert(azsr_decide(AZSR_ROUTE,f)==AZSR_ROUTE_OUTPUT);
    f.color=o;assert(azsr_decide(AZSR_ROUTE,f)==AZSR_REJECT);f.color=r;
    f.depth=o;assert(azsr_decide(AZSR_ROUTE,f)==AZSR_REJECT);f.depth=r;
    f.fh2=0;assert(azsr_decide(AZSR_ROUTE,f)==AZSR_REJECT);f.fh2=1;
    AzRouteGate g={o,o,o,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    assert(azroute_allow(g));
#define CHECK(field) do{g.field=0;assert(!azroute_allow(g));g.field=1;}while(0)
    CHECK(confirmed);CHECK(fresh);CHECK(same_list);CHECK(source_binding);CHECK(hash_match);
    CHECK(layout_known);CHECK(single_rtv);CHECK(full_rect);CHECK(pure_pso);CHECK(compatible);
    CHECK(state_known_rt);CHECK(draw_shape);CHECK(source_unmodified);CHECK(enabled);
#undef CHECK
    g.viewport=r;assert(!azroute_allow(g));g.viewport=o;
    g.target=r;assert(!azroute_allow(g));g.target=o;
    g.output=(AzSRSize){0,0};assert(!azroute_allow(g));
    uintptr_t out=0;
    assert(azroute_descriptor_cpu(0x1040,0x1000,0x2000,32,16,2,&out)&&out==0x2080);
    assert(!azroute_descriptor_cpu(0x1041,0x1000,0x2000,32,16,2,&out));
    assert(!azroute_descriptor_cpu(0x0fe0,0x1000,0x2000,32,16,0,&out));
    assert(!azroute_descriptor_cpu(0x1200,0x1000,0x2000,32,16,0,&out));
    assert(!azroute_descriptor_cpu(0x11e0,0x1000,0x2000,32,16,1,&out));
    assert(!azroute_descriptor_cpu(0x1040,0x1000,UINTPTR_MAX-32,32,16,0,&out));
    assert(!azroute_descriptor_cpu(0x1040,0x1000,0x2000,0,16,0,&out));
    assert(!azroute_descriptor_cpu(0x1040,0x1000,0x2000,32,16,UINT_MAX,&out));
    puts("PASS: route policy, all 14 boolean gates, descriptor arithmetic");return 0;
}
