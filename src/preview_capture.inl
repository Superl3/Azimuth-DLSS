/* A resolve group correlates observations; each physical command-list
   recording has its own batch and completion fence. A reused COM pointer
   is not a recording identity. Readback never guesses queue/state. */
#include "preview_batch_policy.h"
#define AZPV_GROUPS AZPV_CAPTURE_COUNT
#define AZPV_SHOTS 18
#define AZPV_LIVE 4
#define AZPV_TOTAL (AZPV_GROUPS+AZPV_LIVE)
#define AZPV_BATCHES (AZPV_TOTAL * 4)
#define AZPV_LIMIT (256ull*1024*1024)
#define AZPV_FILE_LIMIT (64ull*1024*1024)
typedef struct {
    ID3D12Resource *source,*buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT rows,w,h,format;
    UINT64 row_bytes,total;
    const char *name;
} AzpvShot;
typedef struct {
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence,*producer_dependency; /* owned until this batch retires */
    HANDLE ready_event;
    unsigned long long resolve;
    unsigned count,good;
    AzpvBatchLife life;
    unsigned long long recording;
    unsigned group;
    uintptr_t queue;
    uint64_t started;
    AzpvShot shots[AZPV_SHOTS];
 } AzpvBatch;
typedef struct {
    unsigned long long resolve;
    ID3D12Resource *scene,*sr_snapshot; /* retained to process exit */
    AzpvBatch *core_batch;
    uintptr_t postfx,composite,hdr_copy,hdr_copy_list;
    unsigned long long hdr_copy_recording;
    int hdr_copy_full,hdr_copy_ambiguous;
    unsigned postfx_seen,requested_mask,closed;
    AzvisFrame visible;
} AzpvGroup;
static SRWLOCK azpv_lock=SRWLOCK_INIT;
static AzpvGroup azpv_groups[AZPV_TOTAL];
static AzpvBatch azpv_batches[AZPV_BATCHES];
static unsigned azpv_count,azpv_batch_count;
static UINT64 azpv_bytes;
static volatile LONG azpv_queue_ready;
static int azpv_enabled,azpv_postfx,azpv_done;
static AzPreviewReady azpv_ready;
static AzAutoScene azpv_auto_scene,azpv_auto_sr;
static int azpv_auto_test,azpv_temporal_probe;
static unsigned azpv_capture_group_limit(void){return (azpv_temporal_probe||azvis_fg_probe)?2u:azrp_capture_groups(azrp_profile);}
static unsigned azpv_producer_wait_ms;
static int azpv_gpu_wait;
/* Reuse only private immutable snapshots after all recording fences retire. */
static ID3D12Resource *azpv_snapshot_cache[AZPV_TOTAL];
static uint64_t azpv_play_attempts,azpv_play_recorded,azpv_play_completed,azpv_play_busy,azpv_play_native,azpv_play_reuse,azpv_play_stats_ms;
static struct {uint64_t resolve;uintptr_t postfx,composite;unsigned stages;} azpv_auto_chain;
static wchar_t azpv_dir[MAX_PATH];
static char azpv_dirname[80];

static void preview_init(void) {
    if(!dlss_sr_preview_mode())return;
    char b[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_PREVIEW_POSTFX",b,sizeof(b));
    azpv_postfx=n==1&&b[0]=='1';
    n=GetEnvironmentVariableA("AZIMUTH_DEBUG_AUTO_TEST",b,sizeof(b));
    azpv_auto_test=n==1&&b[0]=='1';
    n=GetEnvironmentVariableA("AZIMUTH_DEBUG_NR_GUIDES",b,sizeof(b));
    azguide_enabled=!dlss_sr_play_mode()&&n==1&&b[0]=='1';
    n=GetEnvironmentVariableA("AZIMUTH_DEBUG_NR_TEMPORAL",b,sizeof(b));
    azpv_temporal_probe=azguide_enabled&&n==1&&b[0]=='1';
    if(azpv_temporal_probe)log_msg("NR TEMPORAL_PROBE groups=2 coreOnly=1 postfxReadback=0 requireAdjacentAnalysis=1");
    if(azguide_enabled)log_msg("NR GUIDE_PROBE config=stencil_and_selected_cb_rows maxPasses=%u sameRecordingRequired=1",AZGUIDE_MAX);
    postfx_replay_init();
    visible_init();
    UINT wait_ms=0;
    if(azt_env_uint("AZIMUTH_DEBUG_PRODUCER_WAIT_MS",&wait_ms)&&wait_ms<=12)azpv_producer_wait_ms=wait_ms;
    n=GetEnvironmentVariableA("AZIMUTH_DEBUG_PRODUCER_GPU_WAIT",b,sizeof(b));
    azpv_gpu_wait=n==1&&b[0]=='1';
    log_msg("PREVIEW PRODUCER_CONFIG waitMs=%u gpuWait=%d policy=submitted_retired_private_fence_only",azpv_producer_wait_ms,azpv_gpu_wait);
    azpv_enabled=GetModuleHandleW(L"renderdoc.dll")==NULL;
    if(dlss_sr_play_mode()){
        azpv_done=1;
        if(!azpv_enabled||!azvis_enabled)dlss_sr_play_disable("replay_or_capture_runtime");
    }
    log_msg("PREVIEW CONFIG readback=%d postfx=%d groups=%u maxBytes=%llu",
        azpv_enabled,azpv_postfx,azpv_capture_group_limit(),(unsigned long long)AZPV_LIMIT);
}
int hooks_sr_accept_profile(unsigned rw,unsigned rh,unsigned ow,unsigned oh){
    if(!azrp_accept_profile(rw,rh,ow,oh))return 0;
    azvis_accept_profile();return 1;
}
static int preview_format(DXGI_FORMAT f) {
    return f==DXGI_FORMAT_R11G11B10_FLOAT||f==DXGI_FORMAT_R16G16_FLOAT||
           f==DXGI_FORMAT_D32_FLOAT||f==DXGI_FORMAT_R32_FLOAT||f==DXGI_FORMAT_R32_TYPELESS||
           f==DXGI_FORMAT_R10G10B10A2_TYPELESS||f==DXGI_FORMAT_R10G10B10A2_UNORM;
}
static int preview_add(AzpvBatch *g,ID3D12Resource *res,D3D12_RESOURCE_STATES state,const char *name) {
    if(!res||!real_CopyTextureRegion||g->count>=AZPV_SHOTS)return 0;
    D3D12_RESOURCE_DESC d;res->lpVtbl->GetDesc(res,&d);
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!d.Width||d.Width>7680||
       !d.Height||d.Height>4320||d.DepthOrArraySize!=1||d.MipLevels!=1||
       d.SampleDesc.Count!=1||d.SampleDesc.Quality||!preview_format(d.Format))return 0;
    AzpvShot s;memset(&s,0,sizeof(s));s.w=(UINT)d.Width;s.h=d.Height;s.format=(UINT)d.Format;s.name=name;
    g_device->lpVtbl->GetCopyableFootprints(g_device,&d,0,1,0,&s.footprint,&s.rows,&s.row_bytes,&s.total);
    if(!s.total||s.total>AZPV_FILE_LIMIT||s.total>AZPV_LIMIT-azpv_bytes||
       s.rows!=s.h||s.row_bytes!=(UINT64)s.w*4||
       s.footprint.Footprint.RowPitch<s.row_bytes||
       s.footprint.Offset+(UINT64)(s.rows-1)*s.footprint.Footprint.RowPitch+s.row_bytes>s.total)return 0;
    D3D12_HEAP_PROPERTIES hp;memset(&hp,0,sizeof(hp));hp.Type=D3D12_HEAP_TYPE_READBACK;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC bd;memset(&bd,0,sizeof(bd));bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width=s.total;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(ID3D12Device_CreateCommittedResource(g_device,&hp,D3D12_HEAP_FLAG_NONE,&bd,
       D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void **)&s.buffer))||!s.buffer)return 0;
    ID3D12Resource_AddRef(res);s.source=res;
    D3D12_TEXTURE_COPY_LOCATION src,dst;memset(&src,0,sizeof(src));memset(&dst,0,sizeof(dst));
    src.pResource=res;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=0;
    dst.pResource=s.buffer;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=s.footprint;
    if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE)hooks_transition(g->list,res,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
    real_CopyTextureRegion(g->list,&dst,0,0,0,&src,NULL);
    if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE)hooks_transition(g->list,res,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
    g->shots[g->count++]=s;azpv_bytes+=s.total;
    log_msg("PREVIEW RECORD resolve=%llu stage=%s size=%ux%u format=%u bytes=%llu",g->resolve,name,s.w,s.h,s.format,(unsigned long long)s.total);
    return 1;
}
/* Called with azpv_lock held. Never attach new work to a submitted batch.
   A successful Reset invalidates the old recording even if the pointer
   is immediately reused. The recording ID is also logged independently. */
static AzpvBatch *preview_batch(unsigned group,ID3D12GraphicsCommandList *cl,unsigned long long recording) {
    for(unsigned i=0;i<azpv_batch_count;++i){
        AzpvBatch *b=&azpv_batches[i];
        if(b->group==group&&b->list==cl&&azpv_can_append(b->life,b->recording,recording))return b;
    }
    unsigned slot=0;
    for(;slot<azpv_batch_count;++slot)if(!azpv_batches[slot].life.state)break;
    if(slot>=AZPV_BATCHES)return NULL;
    if(!recording||cl->lpVtbl->GetType(cl)!=D3D12_COMMAND_LIST_TYPE_DIRECT)return NULL;
    ID3D12Fence *fence=NULL;
    if(FAILED(ID3D12Device_CreateFence(g_device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void **)&fence))||!fence)return NULL;
    if(slot==azpv_batch_count)++azpv_batch_count;
    AzpvBatch *b=&azpv_batches[slot];
    b->fence=fence;b->group=group;b->resolve=azpv_groups[group].resolve;b->recording=recording;
    b->started=GetTickCount64();b->life=(AzpvBatchLife){PV_RECORDED,1};
    ID3D12GraphicsCommandList_AddRef(cl);b->list=cl;
    log_msg("PREVIEW BATCH resolve=%llu batch=%u recording=%llu list=%p",b->resolve,slot+1,b->recording,(void *)cl);
    return b;
}
/* Capture an immutable GPU-local SR result in its actual producer list.
   Unlike the reusable NGX UAV, this snapshot cannot be overwritten by a
   later resolve. Replay waits only by checking completion, never blocking
   the render thread or inventing a cross-queue dependency. */
static ID3D12Resource *preview_sr_snapshot(ID3D12GraphicsCommandList *cl,ID3D12Resource *out,unsigned group) {
    if(!azrp_sr_enabled||!real_CopyResource||!out)return NULL;
    D3D12_RESOURCE_DESC rd;out->lpVtbl->GetDesc(out,&rd);
    if(!azrp_match_output(azrp_profile,rd.Width,rd.Height)||rd.Format!=DXGI_FORMAT_R11G11B10_FLOAT||
       rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||rd.DepthOrArraySize!=1||rd.MipLevels!=1||rd.SampleDesc.Count!=1)return NULL;
    rd.Flags=D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    ID3D12Resource *copy=NULL;
    if(dlss_sr_play_mode()&&group>=AZPV_GROUPS&&group<AZPV_TOTAL&&azpv_snapshot_cache[group]){
        copy=azpv_snapshot_cache[group];azpv_snapshot_cache[group]=NULL;
        hooks_transition(cl,copy,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        ++azpv_play_reuse;
    }else if(FAILED(ID3D12Device_CreateCommittedResource(g_device,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void **)&copy))||!copy)return NULL;
    hooks_transition(cl,out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    real_CopyResource(cl,copy,out);
    hooks_transition(cl,out,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    hooks_transition(cl,copy,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return copy;
}
static int preview_sr_ready(const AzpvGroup *g,ID3D12GraphicsCommandList *cl,unsigned long long recording,int draw_ready,int *deferred) {
    *deferred=0;
    AzpvBatch *b=g->core_batch;
    if(!g->sr_snapshot||!b){
        log_msg("POSTFX SR_PRODUCER resolve=%llu reason=no_snapshot_or_batch ready=0",g->resolve);return 0;
    }
    UINT64 before=0,value=0;
    if(b->life.state==PV_SUBMITTED||b->life.state==PV_SAVED)before=b->fence->lpVtbl->GetCompletedValue(b->fence);
    value=before;
    int ready=azpv_source_ready(b->life,b->recording,recording,b->list==cl,value);
    DWORD wr=WAIT_FAILED;uint64_t start=GetTickCount64();int attempted=0;
    long long wait_us=-1; /* No OS wait, or high-resolution clock unavailable. */
    /* CPU-side bounded alpha wait on the private producer signal ALREADY
       submitted and on a successfully retired recording. No queue Wait,
       no guessed ordering, no source reuse. A timeout leaves native
       rendering untouched. The batch owns the event until completion. */
    if(!ready&&draw_ready&&azpv_source_defer(b->life,value,b->recording,recording,b->queue!=0,azpv_gpu_wait))*deferred=1;
    if(!ready&&!*deferred&&draw_ready&&azpv_producer_wait_ms&&b->queue&&azpv_source_waitable(b->life,value)){
        attempted=1;
        if(!b->ready_event){
            HANDLE event=CreateEventW(NULL,FALSE,FALSE,NULL);
            if(event){
                HRESULT hr=b->fence->lpVtbl->SetEventOnCompletion(b->fence,1,event);
                if(SUCCEEDED(hr))b->ready_event=event;else CloseHandle(event);
            }
        }
        if(b->ready_event){
            LARGE_INTEGER freq,t0,t1;
            int clock_ok=QueryPerformanceFrequency(&freq)&&freq.QuadPart>0&&QueryPerformanceCounter(&t0);
            wr=WaitForSingleObject(b->ready_event,azpv_producer_wait_ms);
            if(clock_ok&&QueryPerformanceCounter(&t1)&&t1.QuadPart>=t0.QuadPart)
                wait_us=(long long)((double)(t1.QuadPart-t0.QuadPart)*1000000.0/(double)freq.QuadPart);
        }
        value=b->fence->lpVtbl->GetCompletedValue(b->fence);
        ready=azpv_source_ready(b->life,b->recording,recording,b->list==cl,value);
    }
    log_msg("POSTFX SR_PRODUCER resolve=%llu producerRecording=%llu consumerRecording=%llu sameList=%d state=%d current=%d queue=%p before=%llu completed=%llu waitAttempted=%d waitResult=%lu waitElapsedMs=%llu ready=%d waitElapsedUs=%lld gpuDeferred=%d",
        g->resolve,b->recording,recording,b->list==cl,b->life.state,b->life.current,(void *)b->queue,(unsigned long long)before,(unsigned long long)value,attempted,(unsigned long)wr,(unsigned long long)(GetTickCount64()-start),ready,wait_us,*deferred);
    return ready||*deferred;
}
/* Only live groups are recycled. Both recordings must be retired and
   complete. Failed or repeated submissions quarantine the slot until
   exit; never free a resource on an ambiguous fence. */
static void preview_reap_live(unsigned long long retire_before){
    for(unsigned g=AZPV_GROUPS;g<AZPV_TOTAL;++g){
        AzpvGroup *p=&azpv_groups[g];if(!azpv_group_retirable(p->resolve,retire_before,p->closed))continue;
        int ready=1;
        for(unsigned i=0;i<azpv_batch_count;++i){
            AzpvBatch *b=&azpv_batches[i];
            if(!b->life.state||b->group!=g)continue;
            if(b->life.state!=PV_SUBMITTED||!azpv_can_map(b->life,b->fence->lpVtbl->GetCompletedValue(b->fence))){ready=0;break;}
        }
        if(!ready)continue;
        for(unsigned i=0;i<azpv_batch_count;++i){
            AzpvBatch *b=&azpv_batches[i];
            if(!b->life.state||b->group!=g)continue;
            if(b->ready_event)CloseHandle(b->ready_event);
            if(b->producer_dependency)ID3D12Fence_Release(b->producer_dependency);
            ID3D12Fence_Release(b->fence);ID3D12GraphicsCommandList_Release(b->list);memset(b,0,sizeof(*b));
        }
        if(p->visible.routed)log_msg("VISIBLE FINAL_GPU_COMPLETE resolve=%llu target=%p liveSlot=%u",p->resolve,(void *)p->visible.target_id,g);
        if(dlss_sr_play_mode()){if(p->visible.routed)++azpv_play_completed;else {++azpv_play_native;dlss_sr_play_miss();}}
        if(p->scene)ID3D12Resource_Release(p->scene);
        if(p->sr_snapshot){
            if(dlss_sr_play_mode()&&!azpv_snapshot_cache[g])azpv_snapshot_cache[g]=p->sr_snapshot;
            else ID3D12Resource_Release(p->sr_snapshot);
        }
        visible_release(&p->visible);
        memset(p,0,sizeof(*p));
    }
}
void hooks_preview_record(ID3D12GraphicsCommandList *cl,ID3D12Resource *color,ID3D12Resource *depth,
                          ID3D12Resource *motion,ID3D12Resource *out,D3D12_RESOURCE_STATES read_state,int reset,int jitter_used,float jx,float jy,int fresh_mv) {
    if(!azpv_enabled||GetModuleHandleW(L"renderdoc.dll")||!cl||!g_device||
       cl->lpVtbl->GetType(cl)!=D3D12_COMMAND_LIST_TYPE_DIRECT||!InterlockedCompareExchange(&azpv_queue_ready,0,0))return;
    AcquireSRWLockExclusive(&azt_lock);
    uint64_t now=GetTickCount64();
    azdebug_preview_note(&azpv_ready,azt_frame+1,now,fresh_mv&&!reset);
    if(azpv_auto_test&&!azdbg.id){
        if(fresh_mv&&!reset)azdebug_auto_scene_note(&azpv_auto_sr,azt_frame+1,now);
        else memset(&azpv_auto_sr,0,sizeof(azpv_auto_sr));
    }
    int wanted=!dlss_sr_play_mode()&&azdbg.active&&azdbg.id==1&&azdbg.observed<azpv_capture_group_limit();
    int live=azvis_window_locked()&&(!azvis_hdr||azvis_hdr_live||wanted);
    unsigned long long resolve=azt_frame+1;
    ListState *s=state_for(cl);
    if(s&&!s->trace_recording_id)s->trace_recording_id=++azt_generation;
    unsigned long long recording=s?s->trace_recording_id:0;
    ReleaseSRWLockExclusive(&azt_lock);
    if(!wanted&&!live)return;
    AcquireSRWLockExclusive(&azpv_lock);
    preview_reap_live(resolve);
    for(unsigned i=0;i<AZPV_TOTAL;++i)if(azpv_groups[i].resolve==resolve)goto done;
    unsigned group=AZPV_TOTAL;
    if(wanted&&azpv_count<azpv_capture_group_limit()&&!azpv_done)group=azpv_count++;
    else if(live)for(unsigned i=AZPV_GROUPS;i<AZPV_TOTAL;++i)if(!azpv_groups[i].resolve){group=i;break;}
    if(dlss_sr_play_mode()&&live)++azpv_play_attempts;
    if(group==AZPV_TOTAL){if(live){if(dlss_sr_play_mode()){++azpv_play_busy;dlss_sr_play_miss();}log_msg("VISIBLE SKIP resolve=%llu stage=producer reason=slot_busy",resolve);}goto done;}
    int capture=group<AZPV_GROUPS;
    AzpvGroup *g=&azpv_groups[group];g->resolve=resolve;g->visible.enabled=live;
    ID3D12Resource_AddRef(color);g->scene=color;g->requested_mask=15u;
    AzpvBatch *batch=preview_batch(group,cl,recording);
    if(!batch){log_msg("PREVIEW FAILED resolve=%llu reason=core_batch",resolve);goto done;}
    log_msg("PREVIEW GROUP resolve=%llu reset=%d jitter=%d list=%p jitterX=%.7f jitterY=%.7f",resolve,reset,jitter_used,(void *)cl,(double)jx,(double)jy);
    if(capture&&azvis_fg_probe)log_msg("FG INPUT_CORE resolve=%llu reset=%d fresh=%d jitter=%d jitterX=%.7f jitterY=%.7f recording=%llu",resolve,reset,fresh_mv,jitter_used,(double)jx,(double)jy,recording);
    if(capture){
    if(!azvis_fg_probe&&!preview_add(batch,color,read_state,"color"))log_msg("PREVIEW SKIP resolve=%llu stage=color",resolve);
    if(!preview_add(batch,depth,read_state,"depth"))log_msg("PREVIEW SKIP resolve=%llu stage=depth",resolve);
    if(!preview_add(batch,motion,read_state,"mv"))log_msg("PREVIEW SKIP resolve=%llu stage=mv",resolve);
    if(!azvis_fg_probe&&!preview_add(batch,out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"sr"))log_msg("PREVIEW SKIP resolve=%llu stage=sr",resolve);
    if(azvis_fg_probe)g->requested_mask=6u;
    if(azguide_enabled){
        g->requested_mask|=32768u|65536u;
        AzGuideFrame *q=azguide_latest;
        D3D12_RESOURCE_DESC md;motion->lpVtbl->GetDesc(motion,&md);
        if(q&&fresh_mv&&q->resolve==resolve&&q->seq==(uint64_t)mv_seq&&q->recording==recording&&q->list==(uintptr_t)cl&&q->w==md.Width&&q->h==md.Height){
            int a=preview_add(batch,q->stencil,D3D12_RESOURCE_STATE_RENDER_TARGET,"stencil");
            int b=preview_add(batch,q->constants,D3D12_RESOURCE_STATE_RENDER_TARGET,"guidecb");
            log_msg("NR GUIDE_MATCH resolve=%llu recording=%llu mvSeq=%llu stencil=%d constants=%d sameRecording=1 receiverUnverified=1",resolve,recording,q->seq,a,b);
        }else log_msg("NR GUIDE_SKIP resolve=%llu reason=missing_or_different_recording",resolve);
    }
    }
    g->core_batch=batch;g->sr_snapshot=preview_sr_snapshot(cl,out,group);
    if(capture&&!batch->count)batch->life.state=PV_FAILED;
done: ReleaseSRWLockExclusive(&azpv_lock);
}
/* Identity-only observation of the native preoverlay copy used by HDR.
   The original copy has already been recorded; no GPU state changes.
   A full-region same-recording copy before composite is a candidate,
   not proof that no later write touched the destination. */
static void preview_hdr_copy(ID3D12GraphicsCommandList *cl,const D3D12_TEXTURE_COPY_LOCATION *dst,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION *src,const D3D12_BOX *box){
    if(!azpv_enabled||!azrp_ready()||(!azrp_profile.wide&&!azvis_hdr)||!azrp_sr_enabled||!dst||!src||!dst->pResource||!src->pResource)return;
    AcquireSRWLockShared(&azt_lock);unsigned long long resolve=azt_frame;
    ListState *s=state_for(cl);unsigned long long recording=s?s->trace_recording_id:0;
    int active=(azdbg.active&&azdbg.id==1)||(azvis_hdr&&azvis_window_locked());
    ReleaseSRWLockShared(&azt_lock);
    if(!active||!recording)return;
    AcquireSRWLockExclusive(&azpv_lock);
    for(unsigned i=0;i<AZPV_TOTAL;++i){
        AzpvGroup *g=&azpv_groups[i];
        if(!g->resolve||g->resolve!=resolve||g->postfx!=(uintptr_t)src->pResource||g->postfx_seen!=1u)continue;
        if(g->hdr_copy){g->hdr_copy_ambiguous=1;g->hdr_copy_full=0;continue;}
        D3D12_RESOURCE_DESC a,b;src->pResource->lpVtbl->GetDesc(src->pResource,&a);dst->pResource->lpVtbl->GetDesc(dst->pResource,&b);
        int full=!x&&!y&&!z&&dst->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&src->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&
            !dst->SubresourceIndex&&!src->SubresourceIndex&&(!box||(!box->left&&!box->top&&!box->front&&box->right==a.Width&&box->bottom==a.Height&&box->back==1))&&
            a.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&b.Dimension==a.Dimension&&azrp_match_render(azrp_profile,a.Width,a.Height)&&
            a.Width==b.Width&&a.Height==b.Height&&a.DepthOrArraySize==1&&b.DepthOrArraySize==1&&a.MipLevels==1&&b.MipLevels==1&&
            a.SampleDesc.Count==1&&b.SampleDesc.Count==1&&(a.Format==23||a.Format==24)&&(b.Format==23||b.Format==24);
        g->hdr_copy=(uintptr_t)dst->pResource;g->hdr_copy_list=(uintptr_t)cl;g->hdr_copy_recording=recording;g->hdr_copy_full=full;
        log_msg("POSTFX HDR_COPY resolve=%llu list=%p recording=%llu src=%p dst=%p srcFmt=%u dstFmt=%u size=%llux%u full=%d box=%d beforeComposite=1 qpc=%lld",resolve,(void *)cl,recording,(void *)src->pResource,(void *)dst->pResource,(unsigned)a.Format,(unsigned)b.Format,(unsigned long long)a.Width,a.Height,full,box!=NULL,azvis_qpc());
    }
    ReleaseSRWLockExclusive(&azpv_lock);
}
/* Before the one-shot begins, observe only identities and dimensions.
   Do not allocate surfaces, record GPU work, or require replay safety
   gates just to start diagnostics. Full PS/VS identity is mandatory.
   The three observed native passes plus fresh SR are a scene readiness
   signal, not proof of gameplay or actual presentation. azt_lock held. */
static void preview_auto_observe_locked(ID3D12GraphicsCommandList *cl,ListState *s,int start_zero){
    (void)cl;
    uint64_t resolve=azt_frame;
    if(!azpv_auto_test||azdbg.id||!resolve||!azrp_profile.ow||
       azpv_ready.last_resolve!=resolve||azpv_ready.streak<3||s->num_rtv!=1||
       s->has_dsv||s->num_vp!=1||s->num_sc!=1||!start_zero)return;
    AztPso *p=azt_pso_slot(s->pso,0);
    if(!p||!azfg_known_candidate(p)||p->rs!=s->trace_graphics_rs||
       p->blend_enabled||p->depth_enabled)return;
    AztRoot *r=azt_root_slot(s->trace_graphics_rs,0);
    AztDesc *t=azt_slot(azt_heap_for_cpu(s->rtv[0].ptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV),s->rtv[0].ptr,0);
    const ViewEntry *live=view_find(s->rtv[0].ptr);
    if(!r||!t||!t->res||!live||live->resource!=t->res||
       t->format!=DXGI_FORMAT_R10G10B10A2_UNORM||
       s->vp[0].TopLeftX!=0||s->vp[0].TopLeftY!=0||
       s->vp[0].Width!=(float)t->desc.Width||s->vp[0].Height!=(float)t->desc.Height)return;
    uintptr_t input=0;unsigned found=0;
    for(UINT i=0;i<r->count;++i)if(!r->ranges[i].reg&&!r->ranges[i].space&&r->ranges[i].count){
        AztDesc *x=azt_bound_srv(s,&r->ranges[i],0);
        if(x&&x->res){input=(uintptr_t)x->res;++found;}
    }
    if(found!=1||!input||input==(uintptr_t)t->res)return;
    if(azpv_auto_chain.resolve!=resolve){
        memset(&azpv_auto_chain,0,sizeof(azpv_auto_chain));azpv_auto_chain.resolve=resolve;
    }
    DWORD hash=p->hash[0];unsigned bit=hash==0x479be24a?1u:hash==0xa7840ad0?2u:4u;
    if(bit==1&&azpv_auto_chain.stages==0&&input==azdbg_scene_identity&&
       azrp_match_render(azrp_profile,t->desc.Width,t->desc.Height)){
        azpv_auto_chain.postfx=(uintptr_t)t->res;azpv_auto_chain.stages=1;
    }else if(bit==2&&azpv_auto_chain.stages==1&&input==azpv_auto_chain.postfx&&
       azrp_match_render(azrp_profile,t->desc.Width,t->desc.Height)){
        azpv_auto_chain.composite=(uintptr_t)t->res;azpv_auto_chain.stages=3;
    }else if(bit==4&&azpv_auto_chain.stages==3&&input==azpv_auto_chain.composite&&
       (!dlss_sr_play_mode()||hash==(azvis_hdr?0x577d52ef:0xa93721c4))&&
       azrp_match_output(azrp_profile,t->desc.Width,t->desc.Height)){
        azpv_auto_chain.stages=7;
        azdebug_auto_scene_note(&azpv_auto_scene,resolve,GetTickCount64());
        if(azpv_auto_scene.count==1)log_msg("PREVIEW AUTO_SCENE resolve=%llu finalPS=%08lx output=%ux%u freshSR=1 nativeChain=1 qpc=%lld", (unsigned long long)resolve,(unsigned long)hash,azrp_profile.ow,azrp_profile.oh,azvis_qpc());
    }
}
/* Opt-in snapshots of exact observed fullscreen passes only. Require
   matching live RTV metadata and a known RT state in this recording. */
static void preview_after_draw(ID3D12GraphicsCommandList *cl,ListState *s,int start_zero) {
    if(!azpv_enabled||!azrp_ready()||!azpv_postfx||!s)return;
    ID3D12Resource *target=NULL,*input=NULL;
    AzHdrContract hdr={0};
    uintptr_t identity=0;DWORD hash=0;unsigned long long resolve=0;
    unsigned missing=0,found=0;int known=0,recording_match=0,state_value=-1;
    UINT nr=s->num_rtv,nv=s->num_vp,ns=s->num_sc;
    /* Failure bits are diagnostic; no weaker resource-state inference. */
    AcquireSRWLockExclusive(&azt_lock);
    if(azpv_auto_test&&!azdbg.id){preview_auto_observe_locked(cl,s,start_zero);ReleaseSRWLockExclusive(&azt_lock);return;}
    if((azdbg.active&&azdbg.id==1&&azdbg.observed<=AZPV_GROUPS)||azvis_window_locked()){
        AztPso *p=azt_pso_slot(s->pso,0);
        if(p&&azfg_known_candidate(p)){
            hash=p->hash[0];resolve=azt_frame;
            AztRoot *r=azt_root_slot(s->trace_graphics_rs,0);
            AztDesc *t=nr==1?azt_slot(azt_heap_for_cpu(s->rtv[0].ptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV),s->rtv[0].ptr,0):NULL;
            const ViewEntry *live=nr==1?view_find(s->rtv[0].ptr):NULL;
            AztState *st=t&&t->res?azt_state(t->res,0):NULL;
            if(azt_incomplete)missing|=1u;
            if(!start_zero||nr!=1||s->has_dsv||nv!=1||ns!=1||s->trace_predicated||s->trace_queries||s->trace_query_unknown||
               s->trace_topology!=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST)missing|=2u;
            if(!r||p->rs!=s->trace_graphics_rs||p->blend_enabled||p->depth_enabled||
               p->color_write_mask!=D3D12_COLOR_WRITE_ENABLE_ALL)missing|=4u;
            if(!t||!t->res||!live||live->resource!=t->res||!t->simple||
               t->format!=DXGI_FORMAT_R10G10B10A2_UNORM)missing|=8u;
            if(t&&t->res&&live&&live->resource==t->res&&t->format==DXGI_FORMAT_R10G10B10A2_UNORM)
                identity=(uintptr_t)t->res;
            known=st&&st->known;
            if(st)state_value=(int)st->state;
            recording_match=st&&st->list==cl&&s->trace_recording_id&&st->recording==s->trace_recording_id;
            if(!known||state_value!=(int)D3D12_RESOURCE_STATE_RENDER_TARGET||!recording_match)missing|=16u;
            if(!t||nv!=1||ns!=1||s->vp[0].TopLeftX!=0||s->vp[0].TopLeftY!=0||
               s->vp[0].Width!=(float)t->desc.Width||s->vp[0].Height!=(float)t->desc.Height||
               s->sc[0].left!=0||s->sc[0].top!=0||s->sc[0].right<(LONG)t->desc.Width||s->sc[0].bottom<(LONG)t->desc.Height)missing|=32u;
            if(r&&p->rs==s->trace_graphics_rs)for(UINT i=0;i<r->count;++i)
                if(r->ranges[i].reg==0&&r->ranges[i].space==0&&r->ranges[i].count){
                    AztDesc *a=azt_bound_srv(s,&r->ranges[i],0);
                    if(a&&a->res){input=a->res;++found;}
                }
            if(found!=1)missing|=64u;
            if(!missing){target=t->res;ID3D12Resource_AddRef(target);}
            if(hash==0x577d52ef&&(azrp_profile.wide||azvis_hdr)&&azrp_sr_enabled)hdr=azhdr_inspect(cl,s,resolve,identity);
        }
    }
    ReleaseSRWLockExclusive(&azt_lock);
    if(!hash)return;
    int routed=0;
    AcquireSRWLockExclusive(&azpv_lock);
    for(unsigned i=0;i<AZPV_TOTAL;++i){
        AzpvGroup *g=&azpv_groups[i];int fg_capture=i<AZPV_GROUPS&&azvis_fg_probe&&!azpv_done;int capture=i<AZPV_GROUPS&&!azpv_temporal_probe&&!azvis_fg_probe;
        if(!g->resolve||g->resolve!=resolve||(capture&&azpv_done))continue;
        unsigned bit=hash==0x479be24a?1u:hash==0xa7840ad0?2u:4u;
        if(g->postfx_seen&bit)continue;
        g->postfx_seen|=bit;if(capture)g->requested_mask|=bit<<4;
        const char *name=bit==1?"preoverlay":bit==2?"composite":"final";
        uintptr_t expected=bit==1?(uintptr_t)g->scene:bit==2?g->postfx:g->composite;
        int chain=identity&&expected&&found==1&&(uintptr_t)input==expected;
        /* Identity correlation must not depend on the upstream readback
           succeeding. This does not establish a resource lifetime or state. */
        if(chain&&!(missing&(1u|4u|8u|64u))){
            if(bit==1){g->postfx=identity;if(capture)visible_note_hud(identity,resolve);}
            if(bit==2)g->composite=identity;
        }
        log_msg("PREVIEW POSTFX_GATE resolve=%llu stage=%s missing=0x%02x chain=%d rtv=%u vp=%u sc=%u topology=%u state=%d known=%d recording=%d t0matches=%u startZero=%d",
            resolve,name,missing,chain,nr,nv,ns,(unsigned)s->trace_topology,state_value,known,recording_match,found,start_zero);
        if(capture&&chain&&!missing&&target){
            AzpvBatch *batch=preview_batch(i,cl,s->trace_recording_id);
            if(!batch||!preview_add(batch,target,D3D12_RESOURCE_STATE_RENDER_TARGET,name))
                log_msg("PREVIEW SKIP resolve=%llu stage=%s reason=readback_batch_or_allocation",resolve,name);
        }else if(capture)log_msg("PREVIEW SKIP resolve=%llu stage=%s reason=%s",resolve,name,missing?"gate":"identity_chain");
        /* Native replay never touches the original RTV, so an unknown
           original target transition (bit 0x10) is not used as a barrier
           guess. All other original-draw/identity gates still apply. */
        if(bit==1&&azrp_enabled){
            if(capture&&!azguide_enabled)g->requested_mask|=128u;
            if(capture&&!azguide_enabled&&chain&&!(missing&~16u)&&s->trace_recording_id){
                AzpvBatch *batch=preview_batch(i,cl,s->trace_recording_id);
                ID3D12Resource *image=batch?postfx_replay_draw(cl,s,resolve,NULL,NULL):NULL;
                if(!image||!preview_add(batch,image,D3D12_RESOURCE_STATE_RENDER_TARGET,"replay"))
                    log_msg("POSTFX REPLAY SKIP resolve=%llu reason=record_or_readback",resolve);
            }else if(capture&&!azguide_enabled)log_msg("POSTFX REPLAY SKIP resolve=%llu reason=original_draw_gate missing=0x%02x chain=%d",resolve,missing,chain);
            if(azrp_sr_enabled){
                if(capture)g->requested_mask|=256u;
                int draw_ready=chain&&!(missing&~16u)&&s->trace_recording_id;
                int deferred=0;
                int producer_ready=preview_sr_ready(g,cl,s->trace_recording_id,draw_ready,&deferred);
                AzpvBatch *batch=draw_ready&&producer_ready?preview_batch(i,cl,s->trace_recording_id):NULL;
                AzrpBindings bindings;
                /* Independent preflight runs even when the producer or
                   draw gate prevents any replay work. */
                if(azrp_prepare_bindings(s,g->scene,g->sr_snapshot,&bindings,resolve,producer_ready,draw_ready&&batch!=NULL,capture)){
                    ID3D12Resource *image=postfx_replay_draw(cl,s,resolve,&bindings,g->visible.enabled?&g->visible.sr_surface:NULL);
                    if(!image){ID3D12DescriptorHeap_Release(bindings.heap);log_msg("POSTFX SR_REPLAY SKIP resolve=%llu reason=draw path=%u",resolve,bindings.path);}
                    else{
                        if(deferred){
                            batch->producer_dependency=g->core_batch->fence;
                            ID3D12Fence_AddRef(batch->producer_dependency);
                            log_msg("POSTFX SR_DEPENDENCY resolve=%llu producer=%llu consumer=%llu fence=%p submittedProducer=1",resolve,g->core_batch->recording,batch->recording,(void *)batch->producer_dependency);
                        }
                        if(capture&&!preview_add(batch,image,D3D12_RESOURCE_STATE_RENDER_TARGET,"srpostfx"))
                            log_msg("POSTFX SR_REPLAY SKIP resolve=%llu reason=readback path=%u",resolve,bindings.path);
                        if(g->visible.enabled){if(chain&&!missing&&target)visible_begin(&g->visible,cl,s,target,resolve);else log_msg("VISIBLE SKIP resolve=%llu stage=srpostfx reason=original_state",resolve);}
                    }
                }
            }
        }
        if(g->visible.enabled&&bit==2&&chain&&!missing&&target){
            visible_composite(&g->visible,cl,s,target,resolve);
            /* Independent of successful SR preparation, observe the
               native tail so an earlier gate cannot hide the next gap. */
            s->trace_watch_resolve=resolve;s->trace_watch_count=0;s->trace_watch_active=1;
            azvis_tail_begin(&s->trace_tail,s->trace_work_seq,(uintptr_t)target);
            log_msg("VISIBLE WORK_WATCH resolve=%llu list=%p recording=%llu work=%llu composite=%p privateComposite=%d",resolve,(void *)cl,s->trace_recording_id,s->trace_work_seq,(void *)target,g->visible.composite_ready);
        }
        int copy_match=g->hdr_copy_full&&!g->hdr_copy_ambiguous&&hdr.scene&&g->hdr_copy==hdr.scene&&g->hdr_copy_list==(uintptr_t)cl&&g->hdr_copy_recording==s->trace_recording_id;
        if(capture&&hash==0x577d52ef&&(azrp_profile.wide||azvis_hdr)&&azrp_sr_enabled){
            g->requested_mask|=2048u;
            log_msg("POSTFX HDR_FINAL_GATE resolve=%llu t0Chain=%d t2CopyMatch=%d inputBindings=%d t2ReadState=%d outputColorSpace=unknown nativeDrawPreserved=1 routeDecision=separate",resolve,chain,copy_match,hdr.inputs_ok,hdr.scene_readable);
            if(chain&&!missing&&copy_match&&hdr.scene_ref){
                AzpvBatch *batch=preview_batch(i,cl,s->trace_recording_id);
                if(!batch||!preview_add(batch,hdr.scene_ref,hdr.scene_state,"hdrscene"))log_msg("POSTFX HDR_SKIP resolve=%llu reason=t2_readback_allocation",resolve);
            }else log_msg("POSTFX HDR_SKIP resolve=%llu reason=t2_state_or_copy_unproven",resolve);
        }
        if(bit==4)g->closed=1;
        if(g->visible.enabled&&bit==4){
            if(capture)g->requested_mask|=1536u;
            if(hash==0x577d52ef&&azvis_hdr&&chain&&!missing&&target){
                AzpvBatch *batch=preview_batch(i,cl,s->trace_recording_id);
                int result=batch?visible_hdr_finalize(&g->visible,cl,s,&hdr,target,resolve,copy_match,capture||fg_capture):0;
                if(result==1)routed=1;
                if(result==1&&fg_capture){
                    g->requested_mask|=1024u|131072u;
                    int a=preview_add(batch,target,D3D12_RESOURCE_STATE_RENDER_TARGET,"visiblefinal");
                    int b=g->visible.fg_hudless&&preview_add(batch,g->visible.fg_hudless,D3D12_RESOURCE_STATE_RENDER_TARGET,"fghudless");
                    log_msg("FG INPUT_FINAL resolve=%llu final=%d hudless=%d hdr=1 actualPresent=0",resolve,a,b);
                }
                if(result&&capture){
                    g->requested_mask|=16384u;
                    if(!preview_add(batch,g->visible.hdr_scene,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"hdrsrscene"))
                        log_msg("VISIBLE HDR_SKIP resolve=%llu reason=scene_readback",resolve);
                    if(result==2)g->requested_mask|=12288u;
                    if(!preview_add(batch,g->visible.merged,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,result==1?"visiblehud":"hudprobe"))
                        log_msg("VISIBLE HDR_SKIP resolve=%llu reason=composite_readback",resolve);
                    if(!preview_add(batch,result==1?target:g->visible.probe_final,D3D12_RESOURCE_STATE_RENDER_TARGET,result==1?"visiblefinal":"finalprobe"))
                        log_msg("VISIBLE HDR_SKIP resolve=%llu reason=final_readback",resolve);
                }
            }else if(hash==0x577d52ef)log_msg("VISIBLE SKIP resolve=%llu stage=final reason=hdr_mode_or_draw_gate missing=0x%x chain=%d",resolve,missing,chain);
            else if(hash==0xa93721c4&&!azvis_hdr&&chain&&!missing&&target&&g->visible.pre_ready&&g->visible.composite_ready&&
               g->visible.list==cl&&g->visible.recording==s->trace_recording_id){
                AzpvBatch *batch=preview_batch(i,cl,s->trace_recording_id);
                int result=batch?visible_finalize(&g->visible,cl,s,input,target,resolve,capture||fg_capture):0;
                if(result==1){
                    routed=1;
                    if(fg_capture){
                        g->requested_mask|=1024u|131072u;
                        int a=preview_add(batch,target,D3D12_RESOURCE_STATE_RENDER_TARGET,"visiblefinal");
                        int b=g->visible.fg_hudless&&preview_add(batch,g->visible.fg_hudless,D3D12_RESOURCE_STATE_RENDER_TARGET,"fghudless");
                        log_msg("FG INPUT_FINAL resolve=%llu final=%d hudless=%d hdr=0 actualPresent=0",resolve,a,b);
                    }
                    if(capture){
                        if(!preview_add(batch,g->visible.merged,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"visiblehud"))log_msg("VISIBLE SKIP resolve=%llu stage=visiblehud reason=readback",resolve);
                        if(!preview_add(batch,target,D3D12_RESOURCE_STATE_RENDER_TARGET,"visiblefinal"))log_msg("VISIBLE SKIP resolve=%llu stage=visiblefinal reason=readback",resolve);
                    }
                }else if(result==2&&capture){
                    g->requested_mask|=12288u;
                    if(!preview_add(batch,g->visible.merged,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"hudprobe"))log_msg("VISIBLE SKIP resolve=%llu stage=hudprobe reason=readback",resolve);
                    if(!preview_add(batch,g->visible.probe_final,D3D12_RESOURCE_STATE_RENDER_TARGET,"finalprobe"))log_msg("VISIBLE SKIP resolve=%llu stage=finalprobe reason=readback",resolve);
                }
            }else log_msg("VISIBLE SKIP resolve=%llu stage=final reason=chain_gate missing=0x%x chain=%d",resolve,missing,chain);
        }
    }
    if(dlss_sr_play_mode()&&routed)++azpv_play_recorded;
    ReleaseSRWLockExclusive(&azpv_lock);
    if(hash==0xa93721c4||hash==0x577d52ef)
        visible_note_final(identity,(uintptr_t)input,cl,s->trace_recording_id,resolve,hash,routed,missing);
    if(target)ID3D12Resource_Release(target);
    if(hdr.scene_ref)ID3D12Resource_Release(hdr.scene_ref);
    if(hdr.image_ref)ID3D12Resource_Release(hdr.image_ref);
}
static void preview_reset(ID3D12GraphicsCommandList *cl) {
    if(!azpv_enabled)return;
    AcquireSRWLockExclusive(&azpv_lock);
    for(unsigned i=0;i<azpv_batch_count;++i){
        AzpvBatch *b=&azpv_batches[i];
        if(b->list!=cl||!b->life.current)continue;
        if(b->life.state==PV_RECORDED)log_msg("PREVIEW FAILED resolve=%llu batch=%u reason=reset_before_submit",b->resolve,i+1);
        azpv_on_reset(&b->life);
    }
    ReleaseSRWLockExclusive(&azpv_lock);
}
/* A recorded deferred read cannot be undone after Close. Always impose
   its already-enqueued producer signal before forwarding the consumer.
   Even a same-queue wait is valid (the signal was previously submitted).
   On the exceptional failure of queue Wait, preserve ordering by a
   synchronous fence wait, or observe device removal. Never execute an
   unsynchronized read on a live device. Opt-in experimental path only. */
static void preview_submit_dependency(ID3D12CommandQueue *q,AzpvBatch *b){
    ID3D12Fence *f=b->producer_dependency;if(!f)return;
    UINT64 completed=f->lpVtbl->GetCompletedValue(f);
    HRESULT hr=S_OK;const char *mode="already_complete";long long start=azvis_qpc();
    if(completed==UINT64_MAX)mode="device_removed";
    else if(completed<1){
        hr=q->lpVtbl->Wait(q,f,1);
        if(SUCCEEDED(hr))mode="queue_wait";
        else{
            mode="cpu_fallback";
            log_msg("POSTFX SR_DEPENDENCY_WAIT_FAILED resolve=%llu hr=%08lx",b->resolve,(unsigned long)hr);
            /* NULL event waits synchronously per ID3D12Fence contract.
               If even event registration fails, poll only while the
               device remains valid; the native list cannot be rolled back. */
            if(FAILED(f->lpVtbl->SetEventOnCompletion(f,1,NULL))){
                while(f->lpVtbl->GetCompletedValue(f)==0&&SUCCEEDED(g_device->lpVtbl->GetDeviceRemovedReason(g_device)))Sleep(1);
            }
            completed=f->lpVtbl->GetCompletedValue(f);
            if(completed==UINT64_MAX||FAILED(g_device->lpVtbl->GetDeviceRemovedReason(g_device)))mode="device_removed";
        }
    }
    log_msg("POSTFX SR_DEPENDENCY_SUBMIT resolve=%llu recording=%llu queue=%p fence=%p mode=%s hr=%08lx completedBeforeOrFallback=%llu qpcStart=%lld qpcEnd=%lld",b->resolve,b->recording,(void *)q,(void *)f,mode,(unsigned long)hr,(unsigned long long)completed,start,azvis_qpc());
}
typedef void (STDMETHODCALLTYPE *AzpvExecute)(ID3D12CommandQueue *,UINT,ID3D12CommandList *const *);
static void preview_execute(ID3D12CommandQueue *q,UINT n,ID3D12CommandList *const *lists,AzpvExecute original) {
    if(!azpv_enabled){original(q,n,lists);return;}
    AcquireSRWLockExclusive(&azpv_lock);
    unsigned matches[AZPV_BATCHES]={0},any=0;
    for(unsigned i=0;i<azpv_batch_count;++i){
        AzpvBatch *b=&azpv_batches[i];if(!b->life.current)continue;
        for(UINT j=0;lists&&j<n;++j)if((void *)lists[j]==(void *)b->list)++matches[i];
        any|=matches[i];
        if(matches[i]&&(matches[i]!=1||b->life.state!=PV_RECORDED)){
            if(b->life.state!=PV_FAILED)log_msg("PREVIEW FAILED resolve=%llu batch=%u reason=repeated_submission",b->resolve,i+1);
            b->life.state=PV_FAILED;
        }
    }
    if(!any){ReleaseSRWLockExclusive(&azpv_lock);original(q,n,lists);return;}
    for(unsigned i=0;i<azpv_batch_count;++i)if(matches[i])preview_submit_dependency(q,&azpv_batches[i]);
    original(q,n,lists);
    for(unsigned i=0;i<azpv_batch_count;++i){
        AzpvBatch *b=&azpv_batches[i];if(matches[i]!=1||b->life.state!=PV_RECORDED)continue;
        HRESULT hr=q->lpVtbl->Signal(q,b->fence,1);
        azpv_on_submit(&b->life,matches[i],SUCCEEDED(hr));
        if(b->life.state!=PV_SUBMITTED)log_msg("PREVIEW FAILED resolve=%llu batch=%u reason=signal",b->resolve,i+1);
        else{
            b->queue=(uintptr_t)q;
            log_msg("PREVIEW SUBMITTED resolve=%llu queue=%p list=%p batch=%u recording=%llu",b->resolve,(void *)q,(void *)b->list,i+1,b->recording);
            AzvisFrame *v=&azpv_groups[b->group].visible;
            if(v->routed&&v->recording==b->recording&&v->list==b->list)log_msg("VISIBLE FINAL_SUBMITTED resolve=%llu target=%p queue=%p list=%p recording=%llu fence=%p qpc=%lld",b->resolve,(void *)v->target_id,(void *)q,(void *)b->list,b->recording,(void *)b->fence,azvis_qpc());
        }
    }
    ReleaseSRWLockExclusive(&azpv_lock);
}
static int preview_dir(void) {
    if(azpv_dirname[0])return 1;
    DWORD n=GetModuleFileNameW(NULL,azpv_dir,MAX_PATH);
    if(!n||n>=MAX_PATH)return 0;
    while(n&&azpv_dir[n-1]!=L'\\')--n;
    if(!n)return 0;
    char name[80];int m=snprintf(name,sizeof(name),"azimuth_preview_%lu_%llu",
       (unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());
    if(m<=0||(size_t)m>=sizeof(name)||n+(DWORD)m+1>=MAX_PATH)return 0;
    if(!MultiByteToWideChar(CP_UTF8,0,name,-1,azpv_dir+n,(int)(MAX_PATH-n))||!CreateDirectoryW(azpv_dir,NULL))return 0;
    strcpy(azpv_dirname,name);return 1;
}
static int preview_save(AzpvBatch *g,AzpvShot *s) {
    if(!preview_dir())return 0;
    char name[100];int n=snprintf(name,sizeof(name),"f%llu-%s-%ux%u-fmt%u.raw",g->resolve,s->name,s->w,s->h,s->format);
    wchar_t file[MAX_PATH];int m=lstrlenW(azpv_dir);
    if(n<=0||(size_t)n>=sizeof(name)||m+1+n+1>=MAX_PATH)return 0;
    lstrcpyW(file,azpv_dir);file[m++]=L'\\';
    if(!MultiByteToWideChar(CP_UTF8,0,name,-1,file+m,MAX_PATH-m))return 0;
    void *data=NULL;D3D12_RANGE range={0,(SIZE_T)s->total},empty={0,0};
    if(FAILED(s->buffer->lpVtbl->Map(s->buffer,0,&range,&data))||!data)return 0;
    HANDLE out=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    int ok=out!=INVALID_HANDLE_VALUE;
    if(ok){
        for(UINT y=0;y<s->rows;++y){
            DWORD written=0;const BYTE *row=(const BYTE *)data+s->footprint.Offset+(UINT64)y*s->footprint.Footprint.RowPitch;
            if(!WriteFile(out,row,(DWORD)s->row_bytes,&written,NULL)||written!=(DWORD)s->row_bytes){ok=0;break;}
        }
        CloseHandle(out);if(!ok)DeleteFileW(file);
    }
    s->buffer->lpVtbl->Unmap(s->buffer,0,&empty);
    if(ok)log_msg("PREVIEW FILE directory=%s file=%s bytes=%llu",azpv_dirname,name,(unsigned long long)(s->row_bytes*s->rows));
    return ok;
}
static void preview_poll(void) {
    if(!dlss_sr_preview_mode())return;
    AcquireSRWLockExclusive(&azt_lock);
    int ended=azdbg.id!=0&&!azdbg.active;
    if(dlss_sr_play_mode()&&azt_incomplete)dlss_sr_play_disable("metadata_capacity");
    unsigned long long retire_before=azt_frame;
    ReleaseSRWLockExclusive(&azt_lock);
    visible_poll_trial();
    AcquireSRWLockExclusive(&azpv_lock);
    preview_reap_live(retire_before);
    if(dlss_sr_play_mode()){
        uint64_t now=GetTickCount64();
        if(now-azpv_play_stats_ms>=5000u){
            azpv_play_stats_ms=now;unsigned used=0,failed=0;
            for(unsigned i=AZPV_GROUPS;i<AZPV_TOTAL;++i)used+=azpv_groups[i].resolve!=0;
            for(unsigned i=0;i<azpv_batch_count;++i)failed+=azpv_batches[i].life.state==PV_FAILED;
            log_msg("SR PLAY_STATS attempts=%llu recorded=%llu gpuComplete=%llu nativeRetired=%llu slotBusy=%llu snapshotReuse=%llu liveSlots=%u failedBatches=%u resolve=%llu available=%d",
                (unsigned long long)azpv_play_attempts,(unsigned long long)azpv_play_recorded,(unsigned long long)azpv_play_completed,(unsigned long long)azpv_play_native,(unsigned long long)azpv_play_busy,(unsigned long long)azpv_play_reuse,used,failed,retire_before,dlss_sr_play_available());
        }
        goto done;
    }
    if(azpv_done)goto done;
    unsigned pending=0,files=0,good_groups=0,failed_groups=0;
    unsigned saved_mask[AZPV_GROUPS]={0};
    uint64_t now=GetTickCount64();
    static const char *names[]={"color","depth","mv","sr","preoverlay","composite","final","replay","srpostfx","visiblehud","visiblefinal","hdrscene","hudprobe","finalprobe","hdrsrscene","stencil","guidecb","fghudless"};
    for(unsigned i=0;i<azpv_batch_count;++i){
        AzpvBatch *b=&azpv_batches[i];
        if(!b->life.state||b->group>=AZPV_GROUPS)continue;
        if(b->life.state==PV_SUBMITTED){
            UINT64 value=b->fence->lpVtbl->GetCompletedValue(b->fence);
            if(value==UINT64_MAX){b->life.state=PV_FAILED;log_msg("PREVIEW FAILED resolve=%llu batch=%u reason=device_removed",b->resolve,i+1);}
            /* Wait for the successful reset too, so a repeated submission
               cannot race mapping or silently overwrite the captured data. */
            else if(azpv_can_map(b->life,value)){
                if(b->ready_event){CloseHandle(b->ready_event);b->ready_event=NULL;}
                for(unsigned k=0;k<b->count;++k){
                    if(preview_save(b,&b->shots[k]))++b->good;
                    else b->shots[k].name=NULL;
                    /* Successful reset + completed fence: these readback
                       references need not pin a game buffer until exit. */
                    if(b->shots[k].source){ID3D12Resource_Release(b->shots[k].source);b->shots[k].source=NULL;}
                    if(b->shots[k].buffer){ID3D12Resource_Release(b->shots[k].buffer);b->shots[k].buffer=NULL;}
                }
                b->life.state=b->good?PV_SAVED:PV_FAILED;
                log_msg("PREVIEW SAVED resolve=%llu files=%u requested=%u fenceComplete=1 batch=%u recording=%llu",b->resolve,b->good,b->count,i+1,b->recording);
                AzvisFrame *v=&azpv_groups[b->group].visible;
                if(v->routed&&v->recording==b->recording&&v->list==b->list)log_msg("VISIBLE FINAL_GPU_COMPLETE resolve=%llu target=%p batch=%u",b->resolve,(void *)v->target_id,i+1);
            }
        }
        if((b->life.state==PV_RECORDED||b->life.state==PV_SUBMITTED)&&now-b->started>=20000){
            b->life.state=PV_FAILED;
            log_msg("PREVIEW FAILED resolve=%llu batch=%u reason=completion_or_reset_timeout",b->resolve,i+1);
        }
        if(b->life.state==PV_SAVED){
            files+=b->good;
            for(unsigned k=0;k<b->count;++k)if(b->shots[k].name)
                for(unsigned j=0;j<AZPV_SHOTS;++j)if(!strcmp(b->shots[k].name,names[j]))saved_mask[b->group]|=1u<<j;
        }else if(b->life.state!=PV_FAILED)++pending;
    }
    if(ended&&!pending){
        for(unsigned i=0;i<azpv_count;++i){
            AzpvGroup *g=&azpv_groups[i];
            unsigned expected=(azpv_postfx?127u:15u)|(azrp_enabled&&!azguide_enabled?128u:0u)|(azrp_sr_enabled?256u:0u)|(g->visible.enabled?(azvis_hdr&&!azvis_hdr_live?12288u:1536u):0u)|((azrp_profile.wide||azvis_hdr)&&azrp_sr_enabled?2048u:0u)|(g->visible.enabled&&azvis_hdr?16384u:0u)|(azguide_enabled?(32768u|65536u):0u);
            if(azpv_temporal_probe)expected=15u|32768u|65536u;
            if(azvis_fg_probe)expected=6u|1024u|131072u|(azguide_enabled?(32768u|65536u):0u);
            int core=azvis_fg_probe?(saved_mask[i]&expected)==expected:(saved_mask[i]&15u)==15u;
            if(core)++good_groups;else ++failed_groups;
            log_msg("PREVIEW GROUP_RESULT resolve=%llu expected=0x%02x attempted=0x%02x saved=0x%02x postfxSeen=0x%x coreComplete=%d",
                g->resolve,expected,g->requested_mask,saved_mask[i],g->postfx_seen,core);
        }
        if(!azpv_count)log_msg("PREVIEW NO_GROUPS: no successful selected probe, queue unavailable, or external capture runtime");
        azpv_done=1;log_msg("PREVIEW COMPLETE groups=%u saved=%u failed=%u recordedFiles=%u batches=%u",azpv_count,good_groups,failed_groups,files,azpv_batch_count);
    }
done: ReleaseSRWLockExclusive(&azpv_lock);
}
