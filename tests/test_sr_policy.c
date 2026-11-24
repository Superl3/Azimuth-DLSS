#include <assert.h>
#include <stdio.h>
#include "../src/sr_experiment.h"
int main(void) {
    AzSRSize r = {2560, 1067}, o = {3840, 1600};
    AzSRFrame f = {r,r,r,r,o,1,1,1};
    assert(azsr_decide(AZSR_PROBE,f) == AZSR_EVALUATE_ONLY);
    assert(azsr_decide(AZSR_PADDED,f) == AZSR_REJECT);
    f.color = o; f.depth = o;
    assert(azsr_decide(AZSR_PADDED,f) == AZSR_COPY_PADDED);
    assert(azsr_decide(AZSR_PROBE,f) == AZSR_REJECT);
    f.depth = r;
    assert(azsr_decide(AZSR_PADDED,f) == AZSR_COPY_PADDED);
    f.copy_compatible = 0;
    assert(azsr_decide(AZSR_PADDED,f) == AZSR_REJECT);
    f.copy_compatible = 1; f.top_left = 0;
    assert(azsr_decide(AZSR_PADDED,f) == AZSR_REJECT);
    f.top_left = 1; f.fh2 = 0;
    assert(azsr_decide(AZSR_PADDED,f) == AZSR_REJECT);
    f.fh2 = 1; f.resolve = o;
    assert(azsr_decide(AZSR_PADDED,f) == AZSR_REJECT);
    f.resolve = r; f.output = r;
    assert(azsr_decide(AZSR_PADDED,f) == AZSR_REJECT);
    f.output = (AzSRSize){3840,2160};
    assert(azsr_decide(AZSR_PADDED,f) == AZSR_REJECT);
    assert(azsr_phase_count(r,o) == 36);
    assert(azsr_phase_count(o,o) == 16);
    assert(azsr_phase_count((AzSRSize){1280,533},o) == 128);
    assert(azsr_phase_count((AzSRSize){0,0},o) == 16);
    AzSROptimal optimal = {2560,1067,2700,1125,2400,1000,0};
    assert(azsr_in_range(r,optimal));
    assert(!azsr_in_range(o,optimal));
    assert(!azsr_in_range((AzSRSize){0,0},optimal));
    puts("PASS: 18 SR policy assertions");
    return 0;
}
