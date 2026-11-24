#ifndef AZIMUTH_SR_ROUTE_POLICY_H
#define AZIMUTH_SR_ROUTE_POLICY_H
#include "sr_experiment.h"
#include <stdint.h>
/* Pure policy. A successful predicate is NOT proof of shader semantics. */
typedef struct AzRouteGate {
    AzSRSize output, target, viewport;
    int confirmed, fresh, same_list, source_binding, hash_match;
    int layout_known, single_rtv, full_rect, pure_pso, compatible;
    int state_known_rt, draw_shape, source_unmodified, enabled;
} AzRouteGate;
static inline int azroute_allow(AzRouteGate g) {
    return g.confirmed && g.fresh && g.same_list && g.source_binding && g.hash_match &&
        g.layout_known && g.single_rtv && g.full_rect && g.pure_pso && g.compatible &&
        g.state_known_rt && g.draw_shape && g.source_unmodified && g.enabled &&
        azsr_nonzero(g.output) && azsr_equal(g.output,g.target) && azsr_equal(g.output,g.viewport);
}
/* Convert an actual GPU descriptor handle to a CPU descriptor handle in the same heap.
   Return false on misalignment, overflow, or a slot outside the heap. */
static inline int azroute_descriptor_cpu(uint64_t gpu, uint64_t gpu_base, uintptr_t cpu_base,
    unsigned inc, unsigned count, unsigned offset, uintptr_t *cpu) {
    if (!inc || !count || !gpu_base || !cpu_base || gpu < gpu_base) return 0;
    uint64_t delta = gpu - gpu_base;
    if (delta % inc) return 0;
    uint64_t slot = delta / inc;
    if (slot >= count || offset >= count - slot) return 0;
    slot += offset;
    if (slot > (UINTPTR_MAX - cpu_base) / inc) return 0;
    *cpu = cpu_base + (uintptr_t)slot * inc;
    return 1;
}
#endif
