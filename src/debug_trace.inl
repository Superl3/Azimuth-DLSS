/* Bounded diagnostic capture. No Present hook, no new GPU commands, no
   readback of arbitrary process memory, no automatic upload. */
/* RenderDoc application header uses C bool; include before vendored header. */
#include <stdbool.h>
#include "../third_party/renderdoc/renderdoc_app.h"
#include "debug_shader_dump.inl"
#include "postfx_replay.inl"
#include "preview_capture.inl"
#define AZDBG_GEOM_MAX 64
typedef struct AzdbgGeom {UINT cw,ch,dw,dh,vw,vh; int cf,df;} AzdbgGeom;
static AzdbgGeom azdbg_geom[AZDBG_GEOM_MAX];
static unsigned azdbg_geom_count;
static int azdbg_renderdoc;
static unsigned azdbg_limit=8,azdbg_ms=8000,azdbg_rdc_frames=2;
static unsigned azdbg_auto_resolve;
static RENDERDOC_API_1_4_1 *azdbg_rdc_api;
static uint32_t azdbg_rdc_seen;
static int azdbg_rdc_pending;
static uint64_t azdbg_rdc_started;

static void debug_trace_init(void) {
    if(!dlss_sr_debug_mode())return;
    debug_shader_dump_init();
    preview_init();
    UINT n=dlss_debug_ini_uint(L"Resolves",8);
    if(n>=1 && n<=64)azdbg_limit=n;
    n=dlss_debug_ini_uint(L"TimeoutMs",8000);
    if(n>=1000 && n<=30000)azdbg_ms=n;
    n=dlss_debug_ini_uint(L"RenderDocFrames",2);
    if(n>=1 && n<=4)azdbg_rdc_frames=n;
    char b[8];DWORD len=GetEnvironmentVariableA("AZIMUTH_DEBUG_RENDERDOC",b,sizeof(b));
    azdbg_renderdoc=len?len==1&&b[0]=='1':dlss_debug_ini_uint(L"RenderDoc",0)==1;
    if(azt_env_uint("AZIMUTH_DEBUG_AUTO_RESOLVE",&n) && n<=1000000)azdbg_auto_resolve=n;
    log_msg("DEBUG CONFIG resolves=%u timeoutMs=%u renderdoc=%d gpuFrames=%u autoResolve=%u",
        azdbg_limit,azdbg_ms,azdbg_renderdoc,azdbg_rdc_frames,azdbg_auto_resolve);
}

static void debug_rdc_trigger(uint64_t now) {
    if(!azdbg_renderdoc)return;
    /* Only attach to an already-instrumented process. Never load or inject
       a GPU capture runtime ourselves, and never override the active window. */
    HMODULE m=GetModuleHandleW(L"renderdoc.dll");
    if(!m){log_msg("DEBUG RENDERDOC unavailable: launch through RenderDoc or use PIX separately");return;}
    pRENDERDOC_GetAPI get_api=(pRENDERDOC_GetAPI)(void *)GetProcAddress(m,"RENDERDOC_GetAPI");
    void *api=NULL;
    if(!get_api || !get_api(eRENDERDOC_API_Version_1_4_1,&api) || !api){
        log_msg("DEBUG RENDERDOC unavailable: application API not supported");return;
    }
    azdbg_rdc_api=(RENDERDOC_API_1_4_1 *)api;
    if(azdbg_rdc_api->IsFrameCapturing() || azdbg_rdc_pending){
        log_msg("DEBUG RENDERDOC busy: metadata capture only");return;
    }
    azdbg_rdc_seen=azdbg_rdc_api->GetNumCaptures();
    azdbg_rdc_started=now;azdbg_rdc_pending=1;
    azdbg_rdc_api->TriggerMultiFrameCapture(azdbg_rdc_frames);
    log_msg("DEBUG RENDERDOC requested=%u actualPresentFrames unverified",azdbg_rdc_frames);
}
static int debug_trace_begin_locked(uint64_t now) {
    if(!azdebug_begin(&azdbg,azdbg_limit,now,azdbg_ms))return 0;
    azt_budget=4000;azt_resolve_logs=64;azt_copy_logs=0;
    azdbg_geometry=64;azdbg_velocity=64;azdbg_aux=2048;azdbg_aux_warned=0;
    azdbg_geom_count=0;azfg_general_rows=azfg_priority_rows=0;azfg_limit_logged=azfg_priority_limit_logged=0;
    log_msg("DEBUG BEGIN id=%u firstResolveAfter=%llu limit=%u timeoutMs=%u",azdbg.id,azt_frame,azdbg_limit,azdbg_ms);
    log_msg("DEBUG DESCRIPTOR_STATS id=%u used=%u capacity=%u incomplete=%d",azdbg.id,azt_desc_used,(unsigned)AZT_DESCS,azt_incomplete);
    return 1;
}
static void debug_trace_poll(void) {
    if(!dlss_sr_debug_mode())return;
    const uint64_t now=(uint64_t)GetTickCount64();
    int started=0;
    AcquireSRWLockExclusive(&azt_lock);
    if(azdebug_expired(&azdbg,now))azdbg_finish_locked("timeout");
    if(azdebug_auto_due(&azdbg,azdbg_auto_resolve,azt_frame)){
        started=debug_trace_begin_locked(now);
        if(started)log_msg("DEBUG AUTO_BEGIN id=%u requestedResolve=%u",azdbg.id,azdbg_auto_resolve);
    }
    ReleaseSRWLockExclusive(&azt_lock);
    /* RenderDoc API is only called on the existing hotkey thread. */
    if(started)debug_rdc_trigger(now);
    preview_poll();
    if(azdbg_rdc_pending && azdbg_rdc_api){
        uint32_t count=azdbg_rdc_api->GetNumCaptures();
        if(count>azdbg_rdc_seen && !azdbg_rdc_api->IsFrameCapturing()){
            log_msg("DEBUG RENDERDOC completed newCaptures=%u (save captures from RenderDoc UI; not included in metadata ZIP)",count-azdbg_rdc_seen);
            azdbg_rdc_pending=0;
        }else if(now-azdbg_rdc_started>=30000){
            log_msg("DEBUG RENDERDOC no completed capture observed within 30s");azdbg_rdc_pending=0;
        }
    }
}
static void debug_trace_toggle(void) {
    if(!dlss_sr_debug_mode())return;
    const uint64_t now=(uint64_t)GetTickCount64();
    int started=0;
    AcquireSRWLockExclusive(&azt_lock);
    if(azdbg.active)azdbg_finish_locked("manual");
    else started=debug_trace_begin_locked(now);
    ReleaseSRWLockExclusive(&azt_lock);
    if(started)debug_rdc_trigger(now);
}
static void debug_trace_draw(ID3D12GraphicsCommandList *cl,ListState *s,UINT count,int role) {
    if(!dlss_sr_debug_mode() || !s || !count)return;
    AcquireSRWLockExclusive(&azt_lock);
    if(!azdbg.active)goto done;
    /* A small, deduplicated set of actual depth-using draw dimensions.
       This is deliberately distinct from the HDR resolve viewport. */
    const ViewEntry *c=s->num_rtv?view_find(s->rtv[0].ptr):NULL;
    const ViewEntry *d=s->has_dsv?view_find(s->dsv.ptr):NULL;
    if(role!=ROLE_NONE){
        AztPso *p=azt_pso_slot(s->pso,0);
        if(azdbg_aux_locked()){
            log_msg("DEBUG ROLE id=%u resolve=%llu role=%d list=%p pso=%p root=%p color=%p depth=%p",
                azdbg.id,azt_frame,role,(void *)cl,(void *)s->pso,(void *)s->trace_graphics_rs,
                (void *)(c?c->resource:NULL),(void *)(d?d->resource:NULL));
            if(p)log_msg("DEBUG ROLE_HASH id=%u role=%d ps=%08lx-%08lx-%08lx-%08lx vs=%08lx-%08lx-%08lx-%08lx",
                azdbg.id,role,(unsigned long)p->hash[0],(unsigned long)p->hash[1],(unsigned long)p->hash[2],(unsigned long)p->hash[3],
                (unsigned long)p->vs_hash[0],(unsigned long)p->vs_hash[1],(unsigned long)p->vs_hash[2],(unsigned long)p->vs_hash[3]);
            AztRoot *r=azt_root_slot(s->trace_graphics_rs,0);
            /* Partial layouts are useful for diagnostics, never for enabling rendering. */
            if(r && p && p->rs==s->trace_graphics_rs){
                int complete=r->complete&&!azt_incomplete;
                unsigned emitted=0;
                for(UINT i=0;i<r->count && emitted<24;++i){
                    AztRange *x=&r->ranges[i];
                    for(UINT k=0;k<x->count && emitted<24;++k){
                        AztDesc *a=azt_bound_srv(s,x,k);
                        if(!a || !a->res || a->desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D)continue;
                        ++emitted;
                        log_msg("DEBUG ROLE_SRV id=%u role=%d t%u space%u resource=%p size=%ux%u fmt=%d boundOnly=1 layout=%d",
                            azdbg.id,role,x->reg+k,x->space,(void *)a->res,(UINT)a->desc.Width,a->desc.Height,(int)a->format,complete);
                    }
                }
            }
        }
    }
    if(d && s->num_vp==1 && s->vp[0].Width>0 && s->vp[0].Height>0 &&
       s->vp[0].Width<=16384 && s->vp[0].Height<=16384 && azdbg_geom_count<AZDBG_GEOM_MAX){
        AzdbgGeom x={c?c->width:0,c?c->height:0,d->width,d->height,(UINT)s->vp[0].Width,(UINT)s->vp[0].Height,c?(int)c->format:0,(int)d->format};
        int seen=0;
        for(unsigned i=0;i<azdbg_geom_count;++i){
            AzdbgGeom *v=&azdbg_geom[i];
            if(v->cw==x.cw&&v->ch==x.ch&&v->dw==x.dw&&v->dh==x.dh&&v->vw==x.vw&&v->vh==x.vh&&v->cf==x.cf&&v->df==x.df){seen=1;break;}
        }
        if(!seen){
            azdbg_geom[azdbg_geom_count++]=x;
            log_msg("DEBUG GEOMETRY_DRAW id=%u list=%p color=%ux%u depth=%ux%u viewport=%ux%u formats=%d/%d count=%u",
                azdbg.id,(void *)cl,x.cw,x.ch,x.dw,x.dh,x.vw,x.vh,x.cf,x.df,count);
        }
    }
done:
    ReleaseSRWLockExclusive(&azt_lock);
}
static void debug_trace_copy(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst,ID3D12Resource *src,const char *kind) {
    if(!dlss_sr_debug_mode()||!dst||!src)return;
    AcquireSRWLockExclusive(&azt_lock);
    if(azdbg.active){
        D3D12_RESOURCE_DESC a,b;
        dst->lpVtbl->GetDesc(dst,&a);src->lpVtbl->GetDesc(src,&b);
        if(a.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && b.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && azdbg_aux_locked())
            log_msg("DEBUG COPY id=%u resolve=%llu list=%p kind=%s src=%p size=%ux%u fmt=%d dst=%p size=%ux%u fmt=%d",
                azdbg.id,azt_frame,(void *)cl,kind,(void *)src,(UINT)b.Width,b.Height,(int)b.Format,(void *)dst,(UINT)a.Width,a.Height,(int)a.Format);
    }
    ReleaseSRWLockExclusive(&azt_lock);
}
static void debug_trace_dispatch(ID3D12GraphicsCommandList *cl,UINT x,UINT y,UINT z) {
    if(!dlss_sr_debug_mode())return;
    ListState *s=state_for(cl);
    AcquireSRWLockExclusive(&azt_lock);
    if(azdbg_aux_locked()){
        AztPso *p=s?azt_pso_slot(s->pso,0):NULL;
        log_msg("DEBUG DISPATCH id=%u resolve=%llu list=%p pso=%p root=%p groups=%u,%u,%u cs=%08lx-%08lx-%08lx-%08lx",
            azdbg.id,azt_frame,(void *)cl,(void *)(s?s->pso:NULL),(void *)(s?s->compute_rs:NULL),x,y,z,
            (unsigned long)(p&&p->compute?p->hash[0]:0),(unsigned long)(p&&p->compute?p->hash[1]:0),
            (unsigned long)(p&&p->compute?p->hash[2]:0),(unsigned long)(p&&p->compute?p->hash[3]:0));
    }
    ReleaseSRWLockExclusive(&azt_lock);
}
/* Observe actual queue submissions without adding Present hooks. Only patch
   vtables whose target is the original function already recorded, never
   replace a differing wrapper. Never insert/signal a new GPU fence. */
static void (STDMETHODCALLTYPE *dbg_real_execute)(ID3D12CommandQueue *,UINT,ID3D12CommandList *const *);
static HRESULT (STDMETHODCALLTYPE *dbg_real_signal)(ID3D12CommandQueue *,ID3D12Fence *,UINT64);
static HRESULT (STDMETHODCALLTYPE *dbg_real_wait)(ID3D12CommandQueue *,ID3D12Fence *,UINT64);
static void STDMETHODCALLTYPE dbg_execute(ID3D12CommandQueue *q,UINT n,ID3D12CommandList *const *lists) {
    AcquireSRWLockExclusive(&azt_lock);
    if(azdbg_aux_locked()){
        log_msg("DEBUG SUBMIT id=%u resolve=%llu queue=%p count=%u",azdbg.id,azt_frame,(void *)q,n);
        for(UINT i=0;lists&&i<n&&i<32;++i)log_msg("DEBUG SUBMIT_LIST id=%u queue=%p index=%u list=%p",azdbg.id,(void *)q,i,(void *)lists[i]);
        if(n>32)log_msg("DEBUG TRUNCATED id=%u section=submit_list",azdbg.id);
    }
    ReleaseSRWLockExclusive(&azt_lock);
    preview_execute(q,n,lists,dbg_real_execute);
}
static HRESULT STDMETHODCALLTYPE dbg_signal(ID3D12CommandQueue *q,ID3D12Fence *f,UINT64 value) {
    HRESULT hr=dbg_real_signal(q,f,value);
    AcquireSRWLockExclusive(&azt_lock);
    if(azdbg_aux_locked())log_msg("DEBUG SIGNAL id=%u queue=%p fence=%p value=%llu hr=%08lx",azdbg.id,(void *)q,(void *)f,(unsigned long long)value,(unsigned long)hr);
    ReleaseSRWLockExclusive(&azt_lock);
    return hr;
}
static HRESULT STDMETHODCALLTYPE dbg_wait(ID3D12CommandQueue *q,ID3D12Fence *f,UINT64 value) {
    HRESULT hr=dbg_real_wait(q,f,value);
    AcquireSRWLockExclusive(&azt_lock);
    if(azdbg_aux_locked())log_msg("DEBUG WAIT id=%u queue=%p fence=%p value=%llu hr=%08lx",azdbg.id,(void *)q,(void *)f,(unsigned long long)value,(unsigned long)hr);
    ReleaseSRWLockExclusive(&azt_lock);
    return hr;
}
static void debug_trace_queue_hook(ID3D12CommandQueue *q,const D3D12_COMMAND_QUEUE_DESC *desc) {
    if(!dlss_sr_debug_mode() || !q || !desc)return;
    char b[8]; DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_QUEUE_TRACE",b,sizeof(b));
    int trace=n?(n==1&&b[0]=='1'):dlss_debug_ini_uint(L"QueueTrace",0)==1;
    int preview=dlss_sr_preview_mode()&&azpv_enabled&&desc->Type==D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(GetModuleHandleW(L"renderdoc.dll") || (!trace&&!preview))return;
    /* Optional: off by default to avoid conflicts with capture/overlay wrappers. */
    AcquireSRWLockExclusive(&azt_lock);
#define DBG_QUEUE_HOOK(method,hook,orig) do { \
    void **slot=VT_SLOT(q,ID3D12CommandQueueVtbl,method); \
    if(!(orig) || *slot==(void *)(orig) || *slot==(void *)(hook))patch_slot(slot,(void *)(hook),(void **)&(orig)); \
    else log_msg("DEBUG QUEUE wrapper differs; skipped " #method); \
} while(0)
    DBG_QUEUE_HOOK(ExecuteCommandLists,dbg_execute,dbg_real_execute);
    if(trace){DBG_QUEUE_HOOK(Signal,dbg_signal,dbg_real_signal);DBG_QUEUE_HOOK(Wait,dbg_wait,dbg_real_wait);}
    if(preview && *VT_SLOT(q,ID3D12CommandQueueVtbl,ExecuteCommandLists)==(void *)dbg_execute)
        InterlockedExchange(&azpv_queue_ready,1);
#undef DBG_QUEUE_HOOK
    ReleaseSRWLockExclusive(&azt_lock);
}
static HRESULT (STDMETHODCALLTYPE *dbg_real_create_compute)(ID3D12Device *,const D3D12_COMPUTE_PIPELINE_STATE_DESC *,REFIID,void **);
static HRESULT STDMETHODCALLTYPE dbg_create_compute(ID3D12Device *dev,const D3D12_COMPUTE_PIPELINE_STATE_DESC *d,REFIID riid,void **out) {
    HRESULT hr=dbg_real_create_compute(dev,d,riid,out);
    if(SUCCEEDED(hr)&&d&&out&&*out){
        ID3D12PipelineState *pso=NULL;
        if(SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out,&IID_ID3D12PipelineState,(void **)&pso))){
            AztPso v;memset(&v,0,sizeof(v));v.pso=pso;v.rs=d->pRootSignature;v.compute=1;
            if(d->CS.pShaderBytecode&&d->CS.BytecodeLength>=20&&!memcmp(d->CS.pShaderBytecode,"DXBC",4))memcpy(v.hash,(const BYTE *)d->CS.pShaderBytecode+4,16);
            AcquireSRWLockExclusive(&azt_lock);AztPso *p=azt_pso_slot(pso,1);if(p)*p=v;ReleaseSRWLockExclusive(&azt_lock);
            ID3D12PipelineState_Release(pso);
        }
    }
    return hr;
}
static void debug_trace_device_hook(ID3D12Device *dev) {
    if(dlss_sr_debug_mode())patch_slot(VT_SLOT(dev,ID3D12DeviceVtbl,CreateComputePipelineState),(void *)dbg_create_compute,(void **)&dbg_real_create_compute);
}
