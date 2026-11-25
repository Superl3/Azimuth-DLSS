#ifndef AZIMUTH_DEBUG_CAPTURE_POLICY_H
#define AZIMUTH_DEBUG_CAPTURE_POLICY_H
#include <stdint.h>
/* No Windows dependencies. A resolve is not a real Present or GPU frame ID.
   Keep N complete resolve intervals; terminate at the next recognized resolve
   or a bounded wall-clock timeout. A caller owns synchronization. */
typedef struct AzDebugWindow {
    unsigned id, limit, observed;
    int active;
    uint64_t started_ms, timeout_ms;
} AzDebugWindow;
static inline int azdebug_begin(AzDebugWindow *w,unsigned limit,uint64_t now,uint64_t timeout) {
    if(w->active || !limit || limit>64 || !timeout || timeout>30000) return 0;
    ++w->id;
    w->limit=limit; w->observed=0; w->active=1;
    w->started_ms=now; w->timeout_ms=timeout;
    return 1;
}
static inline int azdebug_resolve(AzDebugWindow *w) {
    if(!w->active || w->observed>=w->limit) return 0;
    ++w->observed; return 1;
}
static inline int azdebug_expired(const AzDebugWindow *w,uint64_t now) {
    return w->active && now>=w->started_ms && now-w->started_ms>=w->timeout_ms;
}
static inline void azdebug_end(AzDebugWindow *w) { w->active=0; }
/* Preview capture is one-shot, but an early hotkey never consumes it.
   Require a short streak of fresh, successful scene SR resolves. This
   is a readiness heuristic, not a gameplay/menu classifier. */
typedef struct {uint64_t last_resolve,last_ms;unsigned streak;} AzPreviewReady;
static inline void azdebug_preview_note(AzPreviewReady *r,uint64_t resolve,uint64_t now,int eligible){
    if(!eligible){r->streak=0;r->last_resolve=resolve;r->last_ms=now;return;}
    if(r->last_resolve&&resolve==r->last_resolve+1&&now>=r->last_ms&&now-r->last_ms<=500){
        if(r->streak<3)++r->streak;
    }else r->streak=1;
    r->last_resolve=resolve;r->last_ms=now;
}
static inline int azdebug_preview_ready(const AzPreviewReady *r,uint64_t observed,uint64_t now){
    return r->streak>=3&&r->last_resolve&&observed<=r->last_resolve&&
        r->last_resolve-observed<=1&&now>=r->last_ms&&now-r->last_ms<=500;
}
/* Opt-in one shot. A window started manually also consumes the opportunity. */
static inline int azdebug_auto_due(const AzDebugWindow *w,unsigned requested,uint64_t resolve) {
    return requested && requested<=1000000 && !w->active && !w->id && resolve>=requested;
}
/* Trace-only prioritization: a known candidate bypasses the general draw
   quota. Otherwise keep 10-bit output composition, observed backbuffers
   and simple no-depth/no-blend postprocess draws. No rendering decisions. */
static inline int azdebug_pass_kind(int priority,int output10,int backbuffer,
                                   int depth,int dsv,int blend,unsigned count,unsigned instances) {
    if(priority)return 2;
    if(backbuffer||output10)return 1;
    return !depth&&!dsv&&!blend&&instances==1&&(count==3||count==6);
}
#endif
