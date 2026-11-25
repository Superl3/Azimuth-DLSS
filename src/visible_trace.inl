/* Non-invasive output lineage and actual DXGI present counters. Never
   patch Present/Present1. Only explicit trial mode retains the chain
   interfaces, and no backbuffer reference survives a sampling call.
   Counts and resource identities are evidence, not a scan-out proof. */
#define AZVIS_CHAINS 4
#define AZVIS_EVENTS 128
static SRWLOCK azvis_trace_lock=SRWLOCK_INIT;
typedef struct {
    IDXGISwapChain3 *chain;
    uintptr_t queue,buffer;
    UINT count,index;
    int sampled;
    long long qpc;
} AzvisChain;
static AzvisChain azvis_chains[AZVIS_CHAINS];
static unsigned azvis_chain_count;
static int azvis_trace_closed;
typedef struct {
    uintptr_t resource,list,queue;
    unsigned long long resolve,recording;
    long long qpc;
    int submitted,ambiguous,copy;
} AzvisEvent;
static AzvisEvent azvis_events[AZVIS_EVENTS];
static unsigned azvis_event_next,azvis_event_count,azvis_edge_logs,azvis_hud_logs;
static uintptr_t azvis_hud_target;
static unsigned long long azvis_hud_resolve;
static long long azvis_qpc(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return (long long)t.QuadPart;}
static void visible_swapchain(IDXGISwapChain1 *sc,IUnknown *device){
    if(!azvis_enabled||!sc)return;
    IDXGISwapChain3 *chain=NULL;
    if(FAILED(IUnknown_QueryInterface((IUnknown *)sc,&IID_IDXGISwapChain3,(void **)&chain))||!chain){
        log_msg("VISIBLE PRESENT_COUNTER unavailable: swapchain3");return;
    }
    ID3D12CommandQueue *q=NULL;
    if(device)IUnknown_QueryInterface(device,&IID_ID3D12CommandQueue,(void **)&q);
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(!azvis_trace_closed&&azvis_chain_count<AZVIS_CHAINS){
        int duplicate=0;
        for(unsigned i=0;i<azvis_chain_count;++i)if(azvis_chains[i].chain==chain)duplicate=1;
        if(!duplicate){
            AzvisChain *c=&azvis_chains[azvis_chain_count++];
            c->chain=chain;c->queue=(uintptr_t)q;chain=NULL;
            log_msg("VISIBLE CHAIN chain=%p queue=%p retainedForTrial=1 noBufferRefs=1",(void *)c->chain,(void *)c->queue);
        }
    }
    ReleaseSRWLockExclusive(&azvis_trace_lock);
    if(q)ID3D12CommandQueue_Release(q);
    if(chain)chain->lpVtbl->Release(chain);
}
static AzvisEvent *azvis_find_event(uintptr_t resource){
    if(!resource)return NULL;
    for(unsigned i=0;i<azvis_event_count;++i){
        unsigned j=(azvis_event_next+AZVIS_EVENTS-1-i)%AZVIS_EVENTS;
        AzvisEvent *e=&azvis_events[j];
        if(e->resource==resource&&!e->ambiguous)return e;
    }
    return NULL;
}
static void azvis_event(uintptr_t resource,ID3D12GraphicsCommandList *cl,unsigned long long recording,unsigned long long resolve,int copy){
    AzvisEvent *e=&azvis_events[azvis_event_next++%AZVIS_EVENTS];
    azvis_event_next%=AZVIS_EVENTS;
    if(azvis_event_count<AZVIS_EVENTS)++azvis_event_count;
    *e=(AzvisEvent){resource,(uintptr_t)cl,0,resolve,recording,azvis_qpc(),0,0,copy};
}
static void visible_note_final(ID3D12Resource *target,ID3D12GraphicsCommandList *cl,unsigned long long recording,unsigned long long resolve){
    if(!azvis_enabled||!target)return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(!azvis_trace_closed)azvis_event((uintptr_t)target,cl,recording,resolve,0);
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
static void visible_note_hud(uintptr_t target,unsigned long long resolve){
    if(!azvis_enabled||!target)return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    azvis_hud_target=target;azvis_hud_resolve=resolve;
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
static void visible_trace_copy(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst,ID3D12Resource *src,int full){
    if(!azvis_enabled||!dst||!src||dst==src)return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(!azvis_trace_closed&&azvis_edge_logs<256){
        AzvisEvent *from=azvis_find_event((uintptr_t)src);
        if(from){
            unsigned long long resolve=from->resolve;
            ListState *s=state_for(cl);
            D3D12_RESOURCE_DESC a,b;src->lpVtbl->GetDesc(src,&a);dst->lpVtbl->GetDesc(dst,&b);
            full=full&&a.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&b.Dimension==a.Dimension&&a.Width==b.Width&&a.Height==b.Height&&
                a.Format==b.Format&&a.DepthOrArraySize==1&&b.DepthOrArraySize==1&&a.MipLevels==1&&b.MipLevels==1&&a.SampleDesc.Count==1&&b.SampleDesc.Count==1;
            log_msg("VISIBLE EDGE resolve=%llu kind=copy list=%p recording=%llu src=%p dst=%p size=%llux%u full=%d qpc=%lld identityOnly=1",resolve,(void *)cl,s?s->trace_recording_id:0,(void *)src,(void *)dst,(unsigned long long)b.Width,b.Height,full,azvis_qpc());
            ++azvis_edge_logs;
            if(full&&s&&s->trace_recording_id)azvis_event((uintptr_t)dst,cl,s->trace_recording_id,resolve,1);
        }
    }
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
static void visible_trace_submit(ID3D12CommandQueue *q,UINT n,ID3D12CommandList *const *lists){
    if(!azvis_enabled)return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(!azvis_trace_closed)for(unsigned k=0;k<azvis_event_count;++k){
        AzvisEvent *e=&azvis_events[k];
        for(UINT i=0;lists&&i<n;++i)if(e->list==(uintptr_t)lists[i]){
            ListState *s=state_for((ID3D12GraphicsCommandList *)lists[i]);
            if(!s||s->trace_recording_id!=e->recording)continue;
            if(e->submitted){e->ambiguous=1;continue;}
            e->submitted=1;e->queue=(uintptr_t)q;e->qpc=azvis_qpc();
            log_msg("VISIBLE TARGET_SUBMITTED resolve=%llu target=%p list=%p recording=%llu queue=%p copy=%d qpc=%lld",
                e->resolve,(void *)e->resource,(void *)e->list,e->recording,(void *)q,e->copy,e->qpc);
        }
    }
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
static void visible_trace_draw(ID3D12GraphicsCommandList *cl,ListState *s,UINT count,UINT instances,int indexed,UINT first,INT base,UINT first_instance){
    if(!azvis_enabled||!s||s->num_rtv!=1)return;
    uintptr_t hud=0;unsigned long long hud_resolve=0;
    uintptr_t sources[AZVIS_EVENTS];unsigned long long resolves[AZVIS_EVENTS];unsigned source_count=0;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(!azvis_trace_closed&&azvis_hud_logs<128){hud=azvis_hud_target;hud_resolve=azvis_hud_resolve;}
    if(!azvis_trace_closed&&azvis_edge_logs<256)for(unsigned i=0;i<azvis_event_count;++i)
        if(!azvis_events[i].ambiguous&&azvis_events[i].resource){sources[source_count]=azvis_events[i].resource;resolves[source_count++]=azvis_events[i].resolve;}
    ReleaseSRWLockExclusive(&azvis_trace_lock);
    if(!hud&&!source_count)return;
    AcquireSRWLockExclusive(&azt_lock);
    if(!azdbg.active||azdbg.observed>AZPV_CAPTURE_COUNT)goto done;
    AztDesc *t=azt_slot(azt_heap_for_cpu(s->rtv[0].ptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV),s->rtv[0].ptr,0);
    AztPso *p=azt_pso_slot(s->pso,0);
    if(!t||!t->res||!p)goto done;
    if(hud==(uintptr_t)t->res&&hud_resolve==azt_frame&&p->blend_enabled){
        const ViewEntry *dsv=s->has_dsv?view_find(s->dsv.ptr):NULL;
        log_msg("VISIBLE HUD_DRAW resolve=%llu list=%p recording=%llu target=%p root=%p pso=%p ps=%08lx-%08lx-%08lx-%08lx vs=%08lx-%08lx-%08lx-%08lx count=%u instances=%u indexed=%d first=%u base=%d firstInstance=%u depth=%d dsv=%p dsvFmt=%d dsvSize=%ux%u viewport=%.2f,%.2f,%.2f,%.2f scissor=%ld,%ld,%ld,%ld cbvMask=0x%016llx",
            azt_frame,(void *)cl,s->trace_recording_id,(void *)t->res,(void *)s->trace_graphics_rs,(void *)s->pso,
            (unsigned long)p->hash[0],(unsigned long)p->hash[1],(unsigned long)p->hash[2],(unsigned long)p->hash[3],
            (unsigned long)p->vs_hash[0],(unsigned long)p->vs_hash[1],(unsigned long)p->vs_hash[2],(unsigned long)p->vs_hash[3],
            count,instances,indexed,first,base,first_instance,p->depth_enabled,dsv?(void *)dsv->resource:NULL,dsv?(int)dsv->format:0,
            dsv?dsv->width:0,dsv?dsv->height:0,(double)s->vp[0].TopLeftX,(double)s->vp[0].TopLeftY,(double)s->vp[0].Width,(double)s->vp[0].Height,
            (long)s->sc[0].left,(long)s->sc[0].top,(long)s->sc[0].right,(long)s->sc[0].bottom,(unsigned long long)s->trace_graphics_cbv_mask);
        /* Bound-resource observations, not proof of shader use. Include
           the root and physical offsets so disassembly can resolve them. */
        AztRoot *root=azt_root_slot(s->trace_graphics_rs,0);
        unsigned rows=0;
        if(root)for(UINT i=0;i<root->count&&rows<48;++i)for(UINT j=0;j<root->ranges[i].count&&rows<48;++j){
            AztDesc *d=azt_bound_srv(s,&root->ranges[i],j);
            if(d&&d->res){
                log_msg("VISIBLE HUD_BIND resolve=%llu pso=%p rootIndex=%u offset=%u t=%u space=%u res=%p dim=%u size=%llux%u fmt=%u shadow=%d boundOnly=1",
                    azt_frame,(void *)s->pso,root->ranges[i].root,root->ranges[i].offset+j,root->ranges[i].reg+j,root->ranges[i].space,(void *)d->res,(unsigned)d->desc.Dimension,(unsigned long long)d->desc.Width,d->desc.Height,(unsigned)d->format,d->shadow_valid!=0);
                ++rows;
            }
        }
        if(rows==48)log_msg("VISIBLE HUD_BIND_LIMIT resolve=%llu pso=%p",azt_frame,(void *)s->pso);
        AcquireSRWLockExclusive(&azvis_trace_lock);++azvis_hud_logs;ReleaseSRWLockExclusive(&azvis_trace_lock);
    }
    if(source_count&&t->desc.Width==1920&&t->desc.Height==1080){
        AztRoot *r=azt_root_slot(s->trace_graphics_rs,0);
        int matched=0;
        if(r)for(UINT i=0;i<r->count&&!matched;++i)for(UINT k=0;k<r->ranges[i].count&&!matched;++k){
            AztDesc *d=azt_bound_srv(s,&r->ranges[i],k);
            if(!d||!d->res)continue;
            for(unsigned j=0;j<source_count;++j)if(sources[j]==(uintptr_t)d->res){
                log_msg("VISIBLE EDGE resolve=%llu kind=draw list=%p recording=%llu src=%p dst=%p t=%u ps=%08lx-%08lx-%08lx-%08lx boundOnly=1 qpc=%lld",
                    resolves[j],(void *)cl,s->trace_recording_id,(void *)d->res,(void *)t->res,r->ranges[i].reg+k,
                    (unsigned long)p->hash[0],(unsigned long)p->hash[1],(unsigned long)p->hash[2],(unsigned long)p->hash[3],azvis_qpc());
                AcquireSRWLockExclusive(&azvis_trace_lock);++azvis_edge_logs;ReleaseSRWLockExclusive(&azvis_trace_lock);
                matched=1;break;
            }
        }
    }
done: ReleaseSRWLockExclusive(&azt_lock);
}
static uintptr_t azvis_buffer(IDXGISwapChain3 *c,UINT i){
    ID3D12Resource *r=NULL;
    if(FAILED(c->lpVtbl->GetBuffer(c,i,&IID_ID3D12Resource,(void **)&r))||!r)return 0;
    uintptr_t id=(uintptr_t)r;ID3D12Resource_Release(r);return id;
}
static void visible_poll_present(void){
    if(!azvis_enabled)return;
    AcquireSRWLockExclusive(&azt_lock);
    int begun=azdbg.id==1;
    int expired=begun&&GetTickCount64()-azdbg.started_ms>=AZVIS_MS+1000u;
    unsigned long long observed=azt_frame;
    ReleaseSRWLockExclusive(&azt_lock);
    if(!begun)return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(azvis_trace_closed)goto done;
    if(expired){
        for(unsigned i=0;i<azvis_chain_count;++i)if(azvis_chains[i].chain){
            azvis_chains[i].chain->lpVtbl->Release(azvis_chains[i].chain);azvis_chains[i].chain=NULL;
        }
        azvis_trace_closed=1;log_msg("VISIBLE TRIAL_END resolve=%llu chainRefsReleased=1",observed);goto done;
    }
    for(unsigned i=0;i<azvis_chain_count;++i){
        AzvisChain *x=&azvis_chains[i];IDXGISwapChain3 *c=x->chain;
        if(!c)continue;
        UINT count=0;HRESULT hr=c->lpVtbl->GetLastPresentCount(c,&count);
        if(FAILED(hr)){if(!x->sampled){x->sampled=-1;log_msg("VISIBLE PRESENT_COUNTER chain=%p hr=%08lx unavailable=1",(void *)c,(unsigned long)hr);}continue;}
        if(x->sampled>0&&count==x->count)continue;
        UINT index=c->lpVtbl->GetCurrentBackBufferIndex(c);
        uintptr_t buffer=azvis_buffer(c,index);
        long long qpc=azvis_qpc();
        UINT delta=x->sampled>0?count-x->count:0;
        DXGI_FRAME_STATISTICS stats={0};
        HRESULT shr=c->lpVtbl->GetFrameStatistics(c,&stats);
        /* A single increment plus previous-current-buffer identity and a
           submission in that sampled interval is a candidate association.
           Never label it a Present hook or actual displayed-pixel proof. */
        const AzvisEvent *e=x->sampled>0&&delta==1?azvis_find_event(x->buffer):NULL;
        int candidate=e&&e->submitted&&e->qpc>=x->qpc&&e->qpc<=qpc;
        log_msg("VISIBLE PRESENT_COUNT chain=%p queue=%p count=%u delta=%u currentIndex=%u currentBuffer=%p previousBuffer=%p qpc=%lld frameStatsHr=%08lx statsPresent=%u statsQpc=%lld observedResolve=%llu candidateResolve=%llu identityMatch=%d queueMatch=%d association=sample_interval",
            (void *)c,(void *)x->queue,count,delta,index,(void *)buffer,(void *)x->buffer,qpc,(unsigned long)shr,
            SUCCEEDED(shr)?stats.PresentCount:0,SUCCEEDED(shr)?(long long)stats.SyncQPCTime.QuadPart:0,
            observed,candidate?e->resolve:0,candidate,candidate&&e->queue==x->queue);
        x->count=count;x->index=index;x->buffer=buffer;x->qpc=qpc;x->sampled=1;
    }
done: ReleaseSRWLockExclusive(&azvis_trace_lock);
}
