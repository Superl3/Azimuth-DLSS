#include <assert.h>
#include <stdio.h>
#include "../src/sr_experiment.h"
int main(void) {
    assert(azsr_preset("default")==0&&azsr_preset("J")==10&&azsr_preset("k")==11&&azsr_preset("L")==12&&azsr_preset("M")==13);
    assert(azsr_preset("B")==-1&&azsr_preset("Kjunk")==-1);
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
    AzSRSize fh2={2592,1080};
    assert(azsr_scissor_ok(AZSR_PROBE,fh2,0,0,32767,32767));
    assert(azsr_scissor_ok(AZSR_PROBE,fh2,0,0,2592,1080));
    assert(!azsr_scissor_ok(AZSR_PROBE,fh2,0,0,2591,1080));
    assert(!azsr_scissor_ok(AZSR_PROBE,fh2,0,1,32767,32767));
    assert(!azsr_scissor_ok(AZSR_ROUTE,fh2,0,0,32767,32767));
    assert(!azsr_scissor_ok(AZSR_PADDED,fh2,0,0,32767,32767));
    assert(azsr_scissor_ok(AZSR_ROUTE,fh2,0,0,2592,1080));
    f=(AzSRFrame){fh2,fh2,fh2,fh2,o,1,1,1};
    assert(azsr_decide(AZSR_PROBE,f)==AZSR_EVALUATE_ONLY);
    puts("PASS: SR policy including observed FH2 covering scissor and probe dimensions");
    return 0;
}
