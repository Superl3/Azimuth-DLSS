/* Passive, opt-in FG discovery. Included after sr_route.inl.
   No new GPU commands, no Present or ResizeBuffers hook, no FG SDK loaded.
   Resource identities are creation/descriptor observations, not owned resources.
   A resolve sequence is NOT a present/frame token. */
#define AZFG_CHAINS 4
#define AZFG_BUFFERS 16
#define AZFG_INPUTS 8
static int azfg_trace;
static unsigned azfg_start=1, azfg_resolves=8, azfg_budget=4096;
static unsigned azfg_rows, azfg_chains, azfg_base_rows;
static int azfg_limit_logged;
typedef struct {
    unsigned w,h, count;
    uintptr_t chain, queue, buffers[AZFG_BUFFERS];
} AzfgChain;
static AzfgChain azfg_chain[AZFG_CHAINS];

static void fg_trace_init(void) {
    char yes[8];
    DWORD n=GetEnvironmentVariableA("AZIMUTH_FG_TRACE",yes,sizeof(yes));
    if (!dlss_sr_trace_only() || (!dlss_sr_debug_mode() && (n!=1 || yes[0]!='1'))) return;
    UINT x;
    if (azt_env_uint("AZIMUTH_FG_TRACE_START",&x) && x>=1 && x<=1000000) azfg_start=x;
    if (azt_env_uint("AZIMUTH_FG_TRACE_RESOLVES",&x) && x>=1 && x<=64) azfg_resolves=x;
    if (azt_env_uint("AZIMUTH_FG_TRACE_BUDGET",&x) && x>=64 && x<=20000) azfg_budget=x;
    azfg_trace=1;
    log_msg("FG TRACE ONLY start=%u resolves=%u budget=%u (recording order; no frame generation)",azfg_start,azfg_resolves,azfg_budget);
}
static void fg_trace_swapchain(IDXGISwapChain1 *sc,IUnknown *device) {
    if (!azfg_trace || !sc) return;
    DXGI_SWAP_CHAIN_DESC1 d;
    memset(&d,0,sizeof(d));
    if (FAILED(sc->lpVtbl->GetDesc1(sc,&d))) {
        log_msg("FG TRACE swapchain desc unavailable");
        return;
    }
    AzfgChain v;
    memset(&v,0,sizeof(v));
    v.chain=(uintptr_t)sc;v.w=d.Width;v.h=d.Height;
    ID3D12CommandQueue *q=NULL;
    if (device && SUCCEEDED(IUnknown_QueryInterface(device,&IID_ID3D12CommandQueue,(void **)&q))) {
        v.queue=(uintptr_t)q;ID3D12CommandQueue_Release(q);
    }
    /* Snapshot only. Release each GetBuffer ref immediately, so later resize
       is not prevented. A stale snapshot can never authorize an FG operation. */
    v.count=d.BufferCount<AZFG_BUFFERS?d.BufferCount:AZFG_BUFFERS;
    for (UINT i=0;i<v.count;++i) {
        ID3D12Resource *r=NULL;
        if (SUCCEEDED(sc->lpVtbl->GetBuffer(sc,i,&IID_ID3D12Resource,(void **)&r)) && r) {
            v.buffers[i]=(uintptr_t)r;
            ID3D12Resource_Release(r);
        }
    }
    AcquireSRWLockExclusive(&azt_lock);
    unsigned slot=azfg_chains;
    for (unsigned i=0;i<azfg_chains;++i) if (azfg_chain[i].chain==v.chain) {slot=i;break;}
    if (slot>=AZFG_CHAINS) {
        log_msg("FG TRACE swapchain snapshot capacity reached");
        ReleaseSRWLockExclusive(&azt_lock);
        return;
    }
    if (slot==azfg_chains) ++azfg_chains;
    azfg_chain[slot]=v;
    log_msg("FG SWAPCHAIN slot=%u chain=%p queue=%p size=%ux%u format=%d buffers=%u effect=%d flags=%u creationSnapshot=1",
        slot,(void *)v.chain,(void *)v.queue,v.w,v.h,(int)d.Format,d.BufferCount,(int)d.SwapEffect,d.Flags);
    for (UINT i=0;i<v.count;++i) if(v.buffers[i])
        log_msg("FG BACKBUFFER slot=%u index=%u resource=%p creationSnapshot=1",slot,i,(void *)v.buffers[i]);
    ReleaseSRWLockExclusive(&azt_lock);
}

static void fg_trace_draw(ID3D12GraphicsCommandList *cl,ListState *s,UINT count,UINT instances,int role,int indexed) {
    if (!azfg_trace || !s || role!=ROLE_NONE || s->num_rtv!=1 || !s->pso) return;
    AcquireSRWLockExclusive(&azt_lock);
    /* This is a bounded observation window after known scene resolves. Recording
       order across threads/lists is not execution order. Confirm in PIX. */
    if (dlss_sr_debug_mode()) { if(!azdbg.active) goto done; }
    else if (azt_frame<azfg_start || azt_frame-azfg_start>=azfg_resolves || !azfg_chains) goto done;
    AztDesc *t=azt_slot(azt_heap_for_cpu(s->rtv[0].ptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV),s->rtv[0].ptr,0);
    if (!t || !t->res || t->desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D) goto done;
    int display_size=0,snapshot_match=0;
    for (unsigned i=0;i<azfg_chains;++i) {
        const AzfgChain *c=&azfg_chain[i];
        if (c->w && c->h && t->desc.Width==c->w && t->desc.Height==c->h) display_size=1;
        for (UINT j=0;j<c->count;++j) if(c->buffers[j] && c->buffers[j]==(uintptr_t)t->res) snapshot_match=1;
    }
    /* Debug also follows small fullscreen postprocess intermediates. */
    if (!display_size && !snapshot_match && !(dlss_sr_debug_mode() && !s->has_dsv && instances==1 && (count==3 || count==6))) goto done;
    AztPso *p=azt_pso_slot(s->pso,0);
    if (!p) goto done;
    if (azfg_rows-azfg_base_rows>=azfg_budget) {
        if (!azfg_limit_logged++) log_msg("FG TRACE truncated: draw budget exhausted; not a complete HUD pass history");
        goto done;
    }
    unsigned seq=++azfg_rows;
    AztRoot *r=azt_root_slot(s->trace_graphics_rs,0);
    int complete=r && r->complete && !azt_incomplete && p->rs==s->trace_graphics_rs;
    /* Unknown state/format or unknown layout is logged, never guessed. */
    log_msg("FG PASS resolve=%llu seq=%u list=%p target=%p size=%ux%u fmt=%d bbSnapshot=%d blend=%d depth=%d dsv=%d writeMask=%u layout=%d viewport=%.2f,%.2f,%.2f,%.2f draw=%u/%u indexed=%d ps=%08lx-%08lx-%08lx-%08lx vs=%08lx-%08lx-%08lx-%08lx",
        azt_frame,seq,(void *)cl,(void *)t->res,(UINT)t->desc.Width,t->desc.Height,(int)t->format,snapshot_match,p->blend_enabled,p->depth_enabled,s->has_dsv,p->color_write_mask,complete,
        s->num_vp?s->vp[0].TopLeftX:0.0,s->num_vp?s->vp[0].TopLeftY:0.0,s->num_vp?s->vp[0].Width:0.0,s->num_vp?s->vp[0].Height:0.0,
        count,instances,indexed,(unsigned long)p->hash[0],(unsigned long)p->hash[1],(unsigned long)p->hash[2],(unsigned long)p->hash[3],
        (unsigned long)p->vs_hash[0],(unsigned long)p->vs_hash[1],(unsigned long)p->vs_hash[2],(unsigned long)p->vs_hash[3]);
    /* Debug can log known ranges from a partial layout as candidate evidence.
       layout=0 above explicitly marks the observation as unverified. */
    if (complete || (dlss_sr_debug_mode() && r && p->rs==s->trace_graphics_rs)) {
        uintptr_t seen[AZFG_INPUTS]={0};
        unsigned n=0,excess=0;
        for (UINT i=0;i<r->count;++i) {
            const AztRange *x=&r->ranges[i];
            for (UINT k=0;k<x->count;++k) {
                AztDesc *a=azt_bound_srv(s,x,k);
                if(!a || !a->res || a->desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D) continue;
                uintptr_t id=(uintptr_t)a->res;
                int duplicate=0;
                for(unsigned j=0;j<n;++j) if(seen[j]==id){duplicate=1;break;}
                if(duplicate)continue;
                if(n>=AZFG_INPUTS){excess=1;continue;}
                seen[n++]=id;
                log_msg("FG INPUT seq=%u resource=%p size=%ux%u fmt=%d t%u space%u scene=%d boundOnly=1",
                    seq,(void *)id,(UINT)a->desc.Width,a->desc.Height,(int)a->format,x->reg+k,x->space,a->res==azt_pending.scene);
            }
        }
        if(excess) log_msg("FG INPUT truncated seq=%u",seq);
    }
done:
    ReleaseSRWLockExclusive(&azt_lock);
}
