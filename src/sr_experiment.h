#ifndef AZIMUTH_SR_EXPERIMENT_H
#define AZIMUTH_SR_EXPERIMENT_H

/* C / C++ ABI shared by the Zig proxy and MSVC NGX bridge. */
typedef enum AzSRMode {
    AZSR_DLAA = 0, AZSR_DIAGNOSTIC = 1, AZSR_PROBE = 2, AZSR_PADDED = 3, AZSR_ROUTE = 4
} AzSRMode;
typedef enum AzSRQuality {
    AZSR_NATIVE = 0, AZSR_QUALITY = 1, AZSR_BALANCED = 2, AZSR_PERFORMANCE = 3
} AzSRQuality;
typedef struct AzSRSize { unsigned w, h; } AzSRSize;
typedef struct AzSROptimal {
    unsigned optimal_w, optimal_h, max_w, max_h, min_w, min_h;
    float sharpness;
} AzSROptimal;
typedef struct AzSRFrame {
    AzSRSize color, depth, motion, resolve, output;
    int fh2, top_left, copy_compatible;
} AzSRFrame;
typedef enum AzSRDecision {
    AZSR_REJECT = 0, AZSR_EVALUATE_ONLY = 1, AZSR_COPY_PADDED = 2, AZSR_ROUTE_OUTPUT = 3
} AzSRDecision;

static inline int azsr_equal(AzSRSize a, AzSRSize b) {
    return a.w == b.w && a.h == b.h;
}
static inline int azsr_nonzero(AzSRSize s) { return s.w && s.h; }
static inline int azsr_less(AzSRSize a, AzSRSize b) {
    return a.w < b.w && a.h < b.h;
}
static inline int azsr_same_aspect(AzSRSize a, AzSRSize b) {
    /* Allow small alignment/rounding errors, at most 1% aspect difference. */
    double x = (double)a.w * b.h, y = (double)b.w * a.h;
    return x > 0 && y > 0 && x >= y * 0.99 && x <= y * 1.01;
}
static inline AzSRDecision azsr_decide(AzSRMode mode, AzSRFrame f) {
    if (!f.fh2 || !f.top_left || !azsr_nonzero(f.motion) ||
        !azsr_nonzero(f.output) || !azsr_equal(f.motion, f.resolve) ||
        !azsr_less(f.motion, f.output) || !azsr_same_aspect(f.motion, f.output))
        return AZSR_REJECT;
    if ((mode == AZSR_PROBE || mode == AZSR_ROUTE) && azsr_equal(f.color, f.motion) &&
        azsr_equal(f.depth, f.motion))
        return mode == AZSR_ROUTE ? AZSR_ROUTE_OUTPUT : AZSR_EVALUATE_ONLY;
    if (mode == AZSR_PADDED && f.copy_compatible &&
        azsr_equal(f.color, f.output) &&
        (azsr_equal(f.depth, f.output) || azsr_equal(f.depth, f.motion)))
        return AZSR_COPY_PADDED;
    return AZSR_REJECT;
}
static inline unsigned azsr_phase_count(AzSRSize render, AzSRSize output) {
    if (!render.h || output.h <= render.h) return 16;
    double ratio = (double)output.h / render.h;
    double n = 16.0 * ratio * ratio;
    if (n >= 128.0) return 128;
    unsigned rounded = (unsigned)n;
    return rounded + (n > rounded);
}
static inline int azsr_in_range(AzSRSize s, AzSROptimal o) {
    return azsr_nonzero(s) && o.min_w && o.min_h &&
        s.w >= o.min_w && s.h >= o.min_h && s.w <= o.max_w && s.h <= o.max_h;
}
#endif
