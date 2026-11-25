/* Value-only output lineage. Never retain swapchain/backbuffer COM
   interfaces, never poll or patch Present/Present1. Use the existing
   factory's short-lived FG creation snapshot; release behavior unchanged.
   Present/COMMON barriers and submissions are candidates, not Present.
   Actual presents/display require independent ETW/video evidence. */
#include "visible_lineage_policy.h"
#define AZVIS_EVENTS 128u
#define AZVIS_BARRIERS 128u
static SRWLOCK azvis_trace_lock=SRWLOCK_INIT;
static AzvisChainIdentity azvis_chains[AZVIS_CHAINS];
static volatile LONG64 azvis_create_serial;
static int azvis_trace_closed;
static volatile LONG azvis_trace_active;
static int azvis_observing(void){return InterlockedCompareExchange(&azvis_trace_active,0,0)!=0;}
typedef struct {
    uintptr_t resource,list,queue;
    unsigned long long resolve,recording;
    long long qpc;
    int submitted,ambiguous,copy,routed;
} AzvisEvent;
static AzvisEvent azvis_events[AZVIS_EVENTS];
static unsigned azvis_event_next,azvis_event_count,azvis_edge_logs,azvis_hud_logs;
typedef struct {
    uintptr_t resource,list;
    unsigned long long resolve,recording;
    AzvisBufferMatch match;
    int routed,submitted;
} AzvisBarrier;
static AzvisBarrier azvis_barriers[AZVIS_BARRIERS];
static unsigned azvis_barrier_next,azvis_barrier_count;
static unsigned azvis_candidates,azvis_routed,azvis_submitted_routed;
static uintptr_t azvis_hud_target;
static unsigned long long azvis_hud_resolve;
static long long azvis_qpc(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return (long long)t.QuadPart;}
/* The work counter is unchanged for the previously intercepted methods.
   Record the actual method and identities only in the short watched
   tail, never infer shader access from a bound descriptor. */
static void visible_count_work(ID3D12GraphicsCommandList *cl,ListState *s,const char *method,ID3D12Resource *dst,ID3D12Resource *src,UINT a,UINT b,UINT c,UINT d){
    if(!azvis_observing()||!s)return;
    ++s->trace_work_seq;
    if(!s->trace_watch_active||s->trace_watch_count>=64)return;
    ++s->trace_watch_count;
    AcquireSRWLockShared(&azt_lock);
    const ViewEntry *rt=s->num_rtv?view_find(s->rtv[0].ptr):NULL,*ds=s->has_dsv?view_find(s->dsv.ptr):NULL;
    AztPso *p=azt_pso_slot(s->pso,0);
    log_msg("VISIBLE WORK resolve=%llu list=%p recording=%llu work=%llu method=%s dst=%p src=%p pso=%p ps=%08lx-%08lx-%08lx-%08lx root=%p rtv=%p dsv=%p argc=%u,%u,%u,%u qpc=%lld",
        s->trace_watch_resolve,(void *)cl,s->trace_recording_id,s->trace_work_seq,method,(void *)dst,(void *)src,(void *)s->pso,
        (unsigned long)(p?p->hash[0]:0),(unsigned long)(p?p->hash[1]:0),(unsigned long)(p?p->hash[2]:0),(unsigned long)(p?p->hash[3]:0),
        (void *)s->trace_graphics_rs,rt?(void *)rt->resource:NULL,ds?(void *)ds->resource:NULL,a,b,c,d,azvis_qpc());
    if(s->num_rtv>1)for(UINT i=1;i<s->num_rtv&&i<8;++i){
        const ViewEntry *r=view_find(s->rtv[i].ptr);
        log_msg("VISIBLE WORK_RTV resolve=%llu recording=%llu work=%llu slot=%u resource=%p",s->trace_watch_resolve,s->trace_recording_id,s->trace_work_seq,i,r?(void *)r->resource:NULL);
    }
    if(!strcmp(method,"DrawInstanced")||!strcmp(method,"DrawIndexedInstanced")||!strcmp(method,"Dispatch")){
        int compute=!strcmp(method,"Dispatch");
        AztRoot *r=azt_root_slot(compute?s->compute_rs:s->trace_graphics_rs,0);
        unsigned scanned=0,rows=0;
        /* For compute the generic graphics SRV resolver is inapplicable.
           Report unknown instead of fabricating access information. */
        if(!compute&&r)for(UINT i=0;i<r->count&&scanned<256&&rows<24;++i)for(UINT j=0;j<r->ranges[i].count&&scanned<256&&rows<24;++j){
            ++scanned;AztDesc *x=azt_bound_srv(s,&r->ranges[i],j);
            if(x&&x->res){++rows;log_msg("VISIBLE WORK_BIND resolve=%llu recording=%llu work=%llu t=%u space=%u root=%u resource=%p boundOnly=1",s->trace_watch_resolve,s->trace_recording_id,s->trace_work_seq,r->ranges[i].reg+j,r->ranges[i].space,r->ranges[i].root,(void *)x->res);}
        }
        log_msg("VISIBLE WORK_ACCESS resolve=%llu recording=%llu work=%llu rootKnown=%d scanned=%u rows=%u truncated=%d shaderAccessProven=0",s->trace_watch_resolve,s->trace_recording_id,s->trace_work_seq,r!=NULL,scanned,rows,scanned==256||rows==24);
    }
    ReleaseSRWLockShared(&azt_lock);
    if(s->trace_watch_count==64)log_msg("VISIBLE WORK_LIMIT resolve=%llu recording=%llu",s->trace_watch_resolve,s->trace_recording_id);
}
static void visible_work_barriers(ID3D12GraphicsCommandList *cl,ListState *s,UINT n,const D3D12_RESOURCE_BARRIER *b){
    if(!s||!s->trace_watch_active||!b||s->trace_watch_count>=64)return;
    for(UINT i=0;i<n&&s->trace_watch_count<64;++i){
        const D3D12_RESOURCE_BARRIER *x=&b[i];++s->trace_watch_count;
        uintptr_t a=0,c=0;UINT before=0,after=0,sub=0;
        if(x->Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION){a=(uintptr_t)x->Transition.pResource;before=x->Transition.StateBefore;after=x->Transition.StateAfter;sub=x->Transition.Subresource;}
        else if(x->Type==D3D12_RESOURCE_BARRIER_TYPE_UAV)a=(uintptr_t)x->UAV.pResource;
        else if(x->Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){a=(uintptr_t)x->Aliasing.pResourceBefore;c=(uintptr_t)x->Aliasing.pResourceAfter;}
        log_msg("VISIBLE WORK_BARRIER resolve=%llu list=%p recording=%llu afterWork=%llu type=%u flags=%u resource=%p second=%p before=%u after=%u sub=%u qpc=%lld",s->trace_watch_resolve,(void *)cl,s->trace_recording_id,s->trace_work_seq,(unsigned)x->Type,(unsigned)x->Flags,(void *)a,(void *)c,before,after,sub,azvis_qpc());
    }
    if(s->trace_watch_count==64)log_msg("VISIBLE WORK_LIMIT resolve=%llu recording=%llu",s->trace_watch_resolve,s->trace_recording_id);
}
static void visible_work_submit(ID3D12CommandQueue *q,UINT n,ID3D12CommandList *const *lists){
    if(!azvis_observing())return;
    for(UINT i=0;lists&&i<n;++i){
        ListState *s=state_for((ID3D12GraphicsCommandList *)lists[i]);
        if(s&&s->trace_watch_resolve)log_msg("VISIBLE WORK_SUBMIT resolve=%llu list=%p recording=%llu lastWork=%llu queue=%p index=%u count=%u qpc=%lld",s->trace_watch_resolve,(void *)lists[i],s->trace_recording_id,s->trace_work_seq,(void *)q,i,n,azvis_qpc());
    }
}
static uint64_t visible_create_begin(HWND hwnd,IUnknown *device,const DXGI_SWAP_CHAIN_DESC1 *d,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs){
    if(!azvis_enabled)return 0;
    uint64_t attempt=(uint64_t)InterlockedIncrement64(&azvis_create_serial);
    AcquireSRWLockShared(&azt_lock);
    int begun=azdbg.id==1;
    ReleaseSRWLockShared(&azt_lock);
    if(begun){InterlockedExchange(&azvis_chain_changed,1);log_msg("VISIBLE TRIAL_DISABLED reason=swapchain_creation_during_trial attempt=%llu",(unsigned long long)attempt);}
    LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);
    log_msg("VISIBLE SWAPCHAIN_REQUEST attempt=%llu hwnd=%p deviceOrQueue=%p tid=%lu size=%ux%u fmt=%u buffers=%u effect=%u flags=%u fullscreenDesc=%d windowed=%d retainedRefs=0 qpc=%lld qpcFrequency=%lld",
        (unsigned long long)attempt,(void *)hwnd,(void *)device,(unsigned long)GetCurrentThreadId(),
        d?d->Width:0,d?d->Height:0,d?(unsigned)d->Format:0,d?d->BufferCount:0,d?(unsigned)d->SwapEffect:0,d?d->Flags:0,
        fs!=NULL,fs?fs->Windowed:1,azvis_qpc(),(long long)freq.QuadPart);
    return attempt;
}
static void visible_note_chain(uint64_t attempt,uintptr_t hwnd,uintptr_t chain,const AzfgChain *snapshot){
    if(!attempt||!chain)return;
    AzvisChainIdentity v={0};
    v.hwnd=hwnd;v.chain=chain;v.generation=attempt;
    if(snapshot&&snapshot->chain==chain){
        v.queue=snapshot->queue;v.w=snapshot->w;v.h=snapshot->h;
        v.count=snapshot->count<AZVIS_BUFFERS?snapshot->count:AZVIS_BUFFERS;
        memcpy(v.buffers,snapshot->buffers,v.count*sizeof(v.buffers[0]));
    }
    AcquireSRWLockExclusive(&azvis_trace_lock);
    int registered=azvis_register_chain(azvis_chains,&v);
    ReleaseSRWLockExclusive(&azvis_trace_lock);
    log_msg("VISIBLE CHAIN attempt=%llu hwnd=%p chain=%p queue=%p size=%ux%u buffers=%u registered=%d retainedForTrial=0 noBufferRefs=1 creationSnapshot=1",
        (unsigned long long)attempt,(void *)hwnd,(void *)chain,(void *)v.queue,v.w,v.h,v.count,registered);
    for(unsigned i=0;i<v.count;++i)if(v.buffers[i])
        log_msg("VISIBLE BUFFER attempt=%llu chain=%p index=%u resource=%p identityOnly=1",(unsigned long long)attempt,(void *)chain,i,(void *)v.buffers[i]);
}
static AzvisEvent *azvis_find_event(uintptr_t resource){
    if(!resource)return NULL;
    for(unsigned i=0;i<azvis_event_count;++i){
        unsigned j=(azvis_event_next+AZVIS_EVENTS-1-i)%AZVIS_EVENTS;
        AzvisEvent *e=&azvis_events[j];
        if(e->resource==resource)return e->ambiguous?NULL:e;
    }
    return NULL;
}
static void azvis_event(uintptr_t resource,ID3D12GraphicsCommandList *cl,unsigned long long recording,unsigned long long resolve,int copy,int routed){
    AzvisEvent *e=&azvis_events[azvis_event_next++%AZVIS_EVENTS];
    azvis_event_next%=AZVIS_EVENTS;
    if(azvis_event_count<AZVIS_EVENTS)++azvis_event_count;
    *e=(AzvisEvent){resource,(uintptr_t)cl,0,resolve,recording,azvis_qpc(),0,0,copy,routed};
}
static void visible_note_final(uintptr_t target,uintptr_t source,ID3D12GraphicsCommandList *cl,unsigned long long recording,unsigned long long resolve,DWORD hash,int routed,unsigned missing){
    if(!azvis_observing())return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(!azvis_trace_closed){
        AzvisBufferMatch m;int match=azvis_match_buffer(azvis_chains,target,&m);
        log_msg("VISIBLE FINAL_CANDIDATE resolve=%llu ps=%08lx target=%p input=%p list=%p recording=%llu routed=%d missing=0x%x snapshotMatch=%d chain=%p chainAttempt=%llu index=%u qpc=%lld identityOnly=1",
            resolve,(unsigned long)hash,(void *)target,(void *)source,(void *)cl,recording,routed,missing,match,(void *)m.chain,(unsigned long long)m.generation,m.index,azvis_qpc());
        ++azvis_candidates;if(routed)++azvis_routed;
        if(target&&recording&&resolve)azvis_event(target,cl,recording,resolve,0,routed);
    }
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
static void visible_note_hud(uintptr_t target,unsigned long long resolve){
    if(!azvis_observing()||!target)return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    azvis_hud_target=target;azvis_hud_resolve=resolve;
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
static void visible_trace_copy(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst,ID3D12Resource *src,int full){
    if(!azvis_observing()||!dst||!src||dst==src)return;
    unsigned long long resolve=0;
    AcquireSRWLockShared(&azvis_trace_lock);
    if(!azvis_trace_closed&&azvis_edge_logs<256){
        AzvisEvent *from=azvis_find_event((uintptr_t)src);
        if(from)resolve=from->resolve;
    }
    ReleaseSRWLockShared(&azvis_trace_lock);
    if(!resolve)return;
    /* Do not call a COM method or the command-list state table while
       holding the lineage lock. DXGI can be wrapped by other modules. */
    ListState *s=state_for(cl);
    unsigned long long recording=s?s->trace_recording_id:0;
    D3D12_RESOURCE_DESC a,b;src->lpVtbl->GetDesc(src,&a);dst->lpVtbl->GetDesc(dst,&b);
    full=full&&a.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&b.Dimension==a.Dimension&&a.Width==b.Width&&a.Height==b.Height&&
        a.Format==b.Format&&a.DepthOrArraySize==1&&b.DepthOrArraySize==1&&a.MipLevels==1&&b.MipLevels==1&&a.SampleDesc.Count==1&&b.SampleDesc.Count==1;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    AzvisEvent *from=azvis_find_event((uintptr_t)src);
    if(!azvis_trace_closed&&azvis_edge_logs<256&&from&&from->resolve==resolve){
        log_msg("VISIBLE EDGE resolve=%llu kind=copy list=%p recording=%llu src=%p dst=%p size=%llux%u full=%d qpc=%lld identityOnly=1",resolve,(void *)cl,recording,(void *)src,(void *)dst,(unsigned long long)b.Width,b.Height,full,azvis_qpc());
        ++azvis_edge_logs;
        if(full&&recording)azvis_event((uintptr_t)dst,cl,recording,resolve,1,from->routed);
    }
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
static void visible_trace_barriers(ID3D12GraphicsCommandList *cl,ListState *s,UINT n,const D3D12_RESOURCE_BARRIER *b){
    if(!azvis_observing()||!s||!s->trace_recording_id||!b)return;
    AcquireSRWLockShared(&azt_lock);
    int active=azdbg.id==1&&GetTickCount64()-azdbg.started_ms<AZVIS_MS+1000u;
    ReleaseSRWLockShared(&azt_lock);
    if(!active)return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(!azvis_trace_closed)for(UINT i=0;i<n;++i){
        const D3D12_RESOURCE_BARRIER *x=&b[i];
        if(x->Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION||x->Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE||
           !x->Transition.pResource||x->Transition.StateAfter!=D3D12_RESOURCE_STATE_PRESENT||
           (x->Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES&&x->Transition.Subresource!=0)||
           (x->Transition.StateBefore!=D3D12_RESOURCE_STATE_RENDER_TARGET&&x->Transition.StateBefore!=D3D12_RESOURCE_STATE_COPY_DEST&&
            x->Transition.StateBefore!=D3D12_RESOURCE_STATE_RESOLVE_DEST&&x->Transition.StateBefore!=D3D12_RESOURCE_STATE_UNORDERED_ACCESS))continue;
        uintptr_t id=(uintptr_t)x->Transition.pResource;
        AzvisBufferMatch m;int match=azvis_match_buffer(azvis_chains,id,&m);
        AzvisEvent *e=azvis_find_event(id);
        if(!match&&!e)continue;
        int same=e&&e->list==(uintptr_t)cl&&e->recording==s->trace_recording_id;
        unsigned long long resolve=e?e->resolve:0;
        AzvisBarrier *p=&azvis_barriers[azvis_barrier_next++%AZVIS_BARRIERS];
        azvis_barrier_next%=AZVIS_BARRIERS;
        if(azvis_barrier_count<AZVIS_BARRIERS)++azvis_barrier_count;
        *p=(AzvisBarrier){id,(uintptr_t)cl,resolve,s->trace_recording_id,m,e?e->routed:0,0};
        log_msg("VISIBLE PRESENT_BARRIER resolveCandidate=%llu resource=%p list=%p recording=%llu before=%u snapshotMatch=%d chain=%p chainAttempt=%llu index=%u sameRecording=%d routedCandidate=%d qpc=%lld stateAfter=PRESENT_OR_COMMON presentVerified=0",
            resolve,(void *)id,(void *)cl,s->trace_recording_id,(unsigned)x->Transition.StateBefore,match,(void *)m.chain,(unsigned long long)m.generation,m.index,same,p->routed,azvis_qpc());
    }
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
static void visible_trace_submit(ID3D12CommandQueue *q,UINT n,ID3D12CommandList *const *lists){
    if(!azvis_observing())return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(!azvis_trace_closed)for(unsigned k=0;k<azvis_event_count;++k){
        AzvisEvent *e=&azvis_events[k];
        for(UINT i=0;lists&&i<n;++i)if(e->list==(uintptr_t)lists[i]){
            ListState *s=state_for((ID3D12GraphicsCommandList *)lists[i]);
            if(!s||s->trace_recording_id!=e->recording)continue;
            if(e->submitted){e->ambiguous=1;continue;}
            e->submitted=1;e->queue=(uintptr_t)q;e->qpc=azvis_qpc();
            if(e->routed&&!e->copy)++azvis_submitted_routed;
            AzvisBufferMatch m;int match=azvis_match_buffer(azvis_chains,e->resource,&m);
            log_msg("VISIBLE TARGET_SUBMITTED resolve=%llu target=%p list=%p recording=%llu queue=%p copy=%d routed=%d snapshotMatch=%d chain=%p chainAttempt=%llu index=%u qpc=%lld",
                e->resolve,(void *)e->resource,(void *)e->list,e->recording,(void *)q,e->copy,e->routed,match,(void *)m.chain,(unsigned long long)m.generation,m.index,e->qpc);
        }
    }
    if(!azvis_trace_closed)for(unsigned k=0;k<azvis_barrier_count;++k){
        AzvisBarrier *p=&azvis_barriers[k];
        for(UINT i=0;lists&&i<n;++i)if(p->list==(uintptr_t)lists[i]){
            ListState *s=state_for((ID3D12GraphicsCommandList *)lists[i]);
            if(!s||s->trace_recording_id!=p->recording)continue;
            if(p->submitted){log_msg("VISIBLE PRESENT_BARRIER_REPEATED resource=%p recording=%llu",(void *)p->resource,p->recording);continue;}
            p->submitted=1;
            log_msg("VISIBLE PRESENT_BARRIER_SUBMITTED resolveCandidate=%llu resource=%p list=%p recording=%llu queue=%p chain=%p chainAttempt=%llu index=%u queueMatch=%d routedCandidate=%d qpc=%lld presentVerified=0",
                p->resolve,(void *)p->resource,(void *)p->list,p->recording,(void *)q,(void *)p->match.chain,(unsigned long long)p->match.generation,p->match.index,
                p->match.queue&&p->match.queue==(uintptr_t)q,p->routed,azvis_qpc());
        }
    }
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
static void visible_trace_draw(ID3D12GraphicsCommandList *cl,ListState *s,UINT count,UINT instances,int indexed,UINT first,INT base,UINT first_instance){
    if(!azvis_observing()||!s||s->num_rtv!=1)return;
    uintptr_t hud=0;unsigned long long hud_resolve=0;
    uintptr_t sources[AZVIS_EVENTS];unsigned long long resolves[AZVIS_EVENTS];unsigned source_count=0;
    AzvisChainIdentity chains[AZVIS_CHAINS];
    AcquireSRWLockExclusive(&azvis_trace_lock);
    memcpy(chains,azvis_chains,sizeof(chains));
    if(!azvis_trace_closed&&azvis_hud_logs<128){hud=azvis_hud_target;hud_resolve=azvis_hud_resolve;}
    if(!azvis_trace_closed&&azvis_edge_logs<256)for(unsigned i=0;i<azvis_event_count;++i)
        if(!azvis_events[i].ambiguous&&azvis_events[i].resource){sources[source_count]=azvis_events[i].resource;resolves[source_count++]=azvis_events[i].resolve;}
    ReleaseSRWLockExclusive(&azvis_trace_lock);
    if(!hud&&!source_count)return;
    unsigned hud_logged=0,edge_logged=0;
    AcquireSRWLockExclusive(&azt_lock);
    int hud_window=azdbg.active&&azdbg.observed<=AZPV_CAPTURE_COUNT;
    int lineage_window=azdbg.id==1&&GetTickCount64()-azdbg.started_ms<AZVIS_MS+1000u;
    if(!hud_window&&!lineage_window)goto done;
    AztDesc *t=azt_slot(azt_heap_for_cpu(s->rtv[0].ptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV),s->rtv[0].ptr,0);
    AztPso *p=azt_pso_slot(s->pso,0);
    if(!t||!t->res||!p)goto done;
    if(hud_window&&hud==(uintptr_t)t->res&&hud_resolve==azt_frame&&p->blend_enabled){
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
        hud_logged=1;
    }
    /* The observed game chain was created at 1280x720, while the
       final RTV was 1920x1080. Never restrict diagnostic consumers
       to the final output size; a downscale or format conversion may
       be involved. A bound-only edge is NOT promoted to proven lineage. */
    AzvisBufferMatch bm;
    int bb=azvis_match_buffer(chains,(uintptr_t)t->res,&bm);
    if(lineage_window&&source_count&&t->desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&
       (bb||(!s->has_dsv&&!p->depth_enabled&&instances==1&&(count==3||count==6)))){
        if(bb&&azvis_edge_logs<256){
            log_msg("VISIBLE BACKBUFFER_DRAW resolveObserved=%llu list=%p recording=%llu target=%p chain=%p chainAttempt=%llu index=%u size=%llux%u pso=%p ps=%08lx-%08lx-%08lx-%08lx snapshotOnly=1 qpc=%lld",
                azt_frame,(void *)cl,s->trace_recording_id,(void *)t->res,(void *)bm.chain,(unsigned long long)bm.generation,bm.index,(unsigned long long)t->desc.Width,t->desc.Height,(void *)s->pso,(unsigned long)p->hash[0],(unsigned long)p->hash[1],(unsigned long)p->hash[2],(unsigned long)p->hash[3],azvis_qpc());
        }
        AztRoot *r=azt_root_slot(s->trace_graphics_rs,0);
        int matched=0;unsigned scanned=0;
        if(r)for(UINT i=0;i<r->count&&!matched&&scanned<256;++i)for(UINT k=0;k<r->ranges[i].count&&!matched&&scanned<256;++k){
            ++scanned;
            AztDesc *d=azt_bound_srv(s,&r->ranges[i],k);
            if(!d||!d->res)continue;
            for(unsigned j=0;j<source_count;++j)if(sources[j]==(uintptr_t)d->res){
                log_msg("VISIBLE EDGE resolve=%llu kind=draw list=%p recording=%llu src=%p dst=%p size=%llux%u snapshotMatch=%d chain=%p t=%u ps=%08lx-%08lx-%08lx-%08lx boundOnly=1 qpc=%lld",
                    resolves[j],(void *)cl,s->trace_recording_id,(void *)d->res,(void *)t->res,(unsigned long long)t->desc.Width,t->desc.Height,bb,(void *)bm.chain,r->ranges[i].reg+k,
                    (unsigned long)p->hash[0],(unsigned long)p->hash[1],(unsigned long)p->hash[2],(unsigned long)p->hash[3],azvis_qpc());
                edge_logged=1;
                matched=1;break;
            }
        }
    }
done: ReleaseSRWLockExclusive(&azt_lock);
    if(hud_logged||edge_logged){AcquireSRWLockExclusive(&azvis_trace_lock);azvis_hud_logs+=hud_logged;azvis_edge_logs+=edge_logged;ReleaseSRWLockExclusive(&azvis_trace_lock);}
}
static void visible_poll_trial(void){
    if(!azvis_observing())return;
    AcquireSRWLockShared(&azt_lock);
    int expired=azdbg.id==1&&GetTickCount64()-azdbg.started_ms>=AZVIS_MS+1000u;
    unsigned long long observed=azt_frame;
    ReleaseSRWLockShared(&azt_lock);
    if(!expired)return;
    AcquireSRWLockExclusive(&azvis_trace_lock);
    if(!azvis_trace_closed){
        azvis_trace_closed=1;
        InterlockedExchange(&azvis_trace_active,0);
        log_msg("VISIBLE TRIAL_END resolve=%llu reason=%s candidates=%u routed=%u nativeCandidates=%u routedSubmitted=%u retainedSwapchainRefs=0 actualPresent=external_etw_or_video qpc=%lld",observed,InterlockedCompareExchange(&azvis_chain_changed,0,0)?"swapchain_changed":"duration_elapsed",azvis_candidates,azvis_routed,azvis_candidates-azvis_routed,azvis_submitted_routed,azvis_qpc());
    }
    ReleaseSRWLockExclusive(&azvis_trace_lock);
}
