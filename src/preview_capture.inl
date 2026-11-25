/* A resolve group correlates observations; each physical command-list
   recording has its own batch and completion fence. A reused COM pointer
   is not a recording identity. Readback never guesses queue/state. */
#include "preview_batch_policy.h"
#define AZPV_GROUPS AZPV_CAPTURE_COUNT
#define AZPV_SHOTS 11
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
    ID3D12Fence *fence;
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
    uintptr_t postfx,composite;
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
static wchar_t azpv_dir[MAX_PATH];
static char azpv_dirname[80];

static void preview_init(void) {
    if(!dlss_sr_preview_mode())return;
    char b[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_PREVIEW_POSTFX",b,sizeof(b));
    azpv_postfx=n==1&&b[0]=='1';
    postfx_replay_init();
    visible_init();
    azpv_enabled=GetModuleHandleW(L"renderdoc.dll")==NULL;
    log_msg("PREVIEW CONFIG readback=%d postfx=%d groups=%u maxBytes=%llu",
        azpv_enabled,azpv_postfx,(unsigned)AZPV_GROUPS,(unsigned long long)AZPV_LIMIT);
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
static ID3D12Resource *preview_sr_snapshot(ID3D12GraphicsCommandList *cl,ID3D12Resource *out) {
    if(!azrp_sr_enabled||!real_CopyResource||!out)return NULL;
    D3D12_RESOURCE_DESC rd;out->lpVtbl->GetDesc(out,&rd);
    if(rd.Width!=1920||rd.Height!=1080||rd.Format!=DXGI_FORMAT_R11G11B10_FLOAT||
       rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||rd.DepthOrArraySize!=1||rd.MipLevels!=1||rd.SampleDesc.Count!=1)return NULL;
    rd.Flags=D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    ID3D12Resource *copy=NULL;
    if(FAILED(ID3D12Device_CreateCommittedResource(g_device,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void **)&copy))||!copy)return NULL;
    hooks_transition(cl,out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    real_CopyResource(cl,copy,out);
    hooks_transition(cl,out,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    hooks_transition(cl,copy,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return copy;
}
static int preview_sr_ready(const AzpvGroup *g,ID3D12GraphicsCommandList *cl,unsigned long long recording) {
    if(!g->sr_snapshot||!g->core_batch)return 0;
    const AzpvBatch *b=g->core_batch;
    UINT64 value=0;
    if(b->life.state==PV_SUBMITTED||b->life.state==PV_SAVED)value=b->fence->lpVtbl->GetCompletedValue(b->fence);
    return azpv_source_ready(b->life,b->recording,recording,b->list==cl,value);
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
            ID3D12Fence_Release(b->fence);ID3D12GraphicsCommandList_Release(b->list);memset(b,0,sizeof(*b));
        }
        if(p->visible.routed)log_msg("VISIBLE FINAL_GPU_COMPLETE resolve=%llu target=%p liveSlot=%u",p->resolve,(void *)p->visible.target_id,g);
        if(p->scene)ID3D12Resource_Release(p->scene);
        if(p->sr_snapshot)ID3D12Resource_Release(p->sr_snapshot);
        visible_release(&p->visible);
        memset(p,0,sizeof(*p));
    }
}
void hooks_preview_record(ID3D12GraphicsCommandList *cl,ID3D12Resource *color,ID3D12Resource *depth,
                          ID3D12Resource *motion,ID3D12Resource *out,D3D12_RESOURCE_STATES read_state,int reset,int jitter_used,float jx,float jy) {
    if(!azpv_enabled||GetModuleHandleW(L"renderdoc.dll")||!cl||!g_device||
       cl->lpVtbl->GetType(cl)!=D3D12_COMMAND_LIST_TYPE_DIRECT||!InterlockedCompareExchange(&azpv_queue_ready,0,0))return;
    AcquireSRWLockExclusive(&azt_lock);
    int wanted=azdbg.active&&azdbg.id==1&&azdbg.observed<AZPV_GROUPS;
    int live=azvis_window_locked();
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
    if(wanted&&azpv_count<AZPV_GROUPS&&!azpv_done)group=azpv_count++;
    else if(live)for(unsigned i=AZPV_GROUPS;i<AZPV_TOTAL;++i)if(!azpv_groups[i].resolve){group=i;break;}
    if(group==AZPV_TOTAL){if(live)log_msg("VISIBLE SKIP resolve=%llu stage=producer reason=slot_busy",resolve);goto done;}
    int capture=group<AZPV_GROUPS;
    AzpvGroup *g=&azpv_groups[group];g->resolve=resolve;g->visible.enabled=live;
    ID3D12Resource_AddRef(color);g->scene=color;g->requested_mask=15u;
    AzpvBatch *batch=preview_batch(group,cl,recording);
    if(!batch){log_msg("PREVIEW FAILED resolve=%llu reason=core_batch",resolve);goto done;}
    log_msg("PREVIEW GROUP resolve=%llu reset=%d jitter=%d list=%p jitterX=%.7f jitterY=%.7f",resolve,reset,jitter_used,(void *)cl,(double)jx,(double)jy);
    if(capture){
    if(!preview_add(batch,color,read_state,"color"))log_msg("PREVIEW SKIP resolve=%llu stage=color",resolve);
    if(!preview_add(batch,depth,read_state,"depth"))log_msg("PREVIEW SKIP resolve=%llu stage=depth",resolve);
    if(!preview_add(batch,motion,read_state,"mv"))log_msg("PREVIEW SKIP resolve=%llu stage=mv",resolve);
    if(!preview_add(batch,out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"sr"))log_msg("PREVIEW SKIP resolve=%llu stage=sr",resolve);
    }
    g->core_batch=batch;g->sr_snapshot=preview_sr_snapshot(cl,out);
    if(capture&&!batch->count)batch->life.state=PV_FAILED;
done: ReleaseSRWLockExclusive(&azpv_lock);
}
/* Opt-in snapshots of exact observed fullscreen passes only. Require
   matching live RTV metadata and a known RT state in this recording. */
static void preview_after_draw(ID3D12GraphicsCommandList *cl,ListState *s,int start_zero) {
    if(!azpv_enabled||!azpv_postfx||!s)return;
    ID3D12Resource *target=NULL,*input=NULL;
    uintptr_t identity=0;DWORD hash=0;unsigned long long resolve=0;
    unsigned missing=0,found=0;int known=0,recording_match=0,state_value=-1;
    UINT nr=s->num_rtv,nv=s->num_vp,ns=s->num_sc;
    /* Failure bits are diagnostic; no weaker resource-state inference. */
    AcquireSRWLockExclusive(&azt_lock);
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
        }
    }
    ReleaseSRWLockExclusive(&azt_lock);
    if(!hash)return;
    int routed=0;
    AcquireSRWLockExclusive(&azpv_lock);
    for(unsigned i=0;i<AZPV_TOTAL;++i){
        AzpvGroup *g=&azpv_groups[i];int capture=i<AZPV_GROUPS;
        if(!g->resolve||g->resolve!=resolve||(capture&&azpv_done))continue;
        unsigned bit=hash==0x479be24a?1u:hash==0xa7840ad0?2u:4u;
        if(g->postfx_seen&bit)continue;
        g->postfx_seen|=bit;g->requested_mask|=bit<<4;
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
            g->requested_mask|=128u;
            if(capture&&chain&&!(missing&~16u)&&s->trace_recording_id){
                AzpvBatch *batch=preview_batch(i,cl,s->trace_recording_id);
                ID3D12Resource *image=batch?postfx_replay_draw(cl,s,resolve,NULL,NULL):NULL;
                if(!image||!preview_add(batch,image,D3D12_RESOURCE_STATE_RENDER_TARGET,"replay"))
                    log_msg("POSTFX REPLAY SKIP resolve=%llu reason=record_or_readback",resolve);
            }else if(capture)log_msg("POSTFX REPLAY SKIP resolve=%llu reason=original_draw_gate missing=0x%02x chain=%d",resolve,missing,chain);
            if(azrp_sr_enabled){
                g->requested_mask|=256u;
                int draw_ready=chain&&!(missing&~16u)&&s->trace_recording_id;
                int producer_ready=preview_sr_ready(g,cl,s->trace_recording_id);
                AzpvBatch *batch=draw_ready&&producer_ready?preview_batch(i,cl,s->trace_recording_id):NULL;
                AzrpBindings bindings;
                /* Independent preflight runs even when the producer or
                   draw gate prevents any replay work. */
                if(azrp_prepare_bindings(s,g->scene,g->sr_snapshot,&bindings,resolve,producer_ready,draw_ready&&batch!=NULL,capture)){
                    ID3D12Resource *image=postfx_replay_draw(cl,s,resolve,&bindings,g->visible.enabled?&g->visible.sr_surface:NULL);
                    if(!image){ID3D12DescriptorHeap_Release(bindings.heap);log_msg("POSTFX SR_REPLAY SKIP resolve=%llu reason=draw path=%u",resolve,bindings.path);}
                    else{
                        if(capture&&!preview_add(batch,image,D3D12_RESOURCE_STATE_RENDER_TARGET,"srpostfx"))
                            log_msg("POSTFX SR_REPLAY SKIP resolve=%llu reason=readback path=%u",resolve,bindings.path);
                        if(g->visible.enabled){if(chain&&!missing&&target)visible_begin(&g->visible,cl,s,target,resolve);else log_msg("VISIBLE SKIP resolve=%llu stage=srpostfx reason=original_state",resolve);}
                    }
                }
            }
        }
        if(g->visible.enabled&&bit==2&&chain&&!missing&&target)visible_composite(&g->visible,cl,s,target,resolve);
        if(bit==4)g->closed=1;
        if(g->visible.enabled&&bit==4){
            g->requested_mask|=1536u;
            if(hash==0x577d52ef)log_msg("VISIBLE SKIP resolve=%llu stage=final reason=hdr_unsupported switch_game_HDR_off=1",resolve);
            else if(hash==0xa93721c4&&chain&&!missing&&target&&g->visible.pre_ready&&g->visible.composite_ready&&
               g->visible.list==cl&&g->visible.recording==s->trace_recording_id){
                AzpvBatch *batch=preview_batch(i,cl,s->trace_recording_id);
                if(batch&&visible_finalize(&g->visible,cl,s,input,target,resolve)){
                    routed=1;
                    if(capture){
                        if(!preview_add(batch,g->visible.merged,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"visiblehud"))log_msg("VISIBLE SKIP resolve=%llu stage=visiblehud reason=readback",resolve);
                        if(!preview_add(batch,target,D3D12_RESOURCE_STATE_RENDER_TARGET,"visiblefinal"))log_msg("VISIBLE SKIP resolve=%llu stage=visiblefinal reason=readback",resolve);
                    }
                }
            }else log_msg("VISIBLE SKIP resolve=%llu stage=final reason=chain_gate missing=0x%x chain=%d",resolve,missing,chain);
        }
    }
    ReleaseSRWLockExclusive(&azpv_lock);
    if(hash==0xa93721c4||hash==0x577d52ef)
        visible_note_final(identity,(uintptr_t)input,cl,s->trace_recording_id,resolve,hash,routed,missing);
    if(target)ID3D12Resource_Release(target);
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
    unsigned long long retire_before=azt_frame;
    ReleaseSRWLockExclusive(&azt_lock);
    visible_poll_trial();
    AcquireSRWLockExclusive(&azpv_lock);
    preview_reap_live(retire_before);
    if(azpv_done)goto done;
    unsigned pending=0,files=0,good_groups=0,failed_groups=0;
    unsigned saved_mask[AZPV_GROUPS]={0};
    uint64_t now=GetTickCount64();
    static const char *names[]={"color","depth","mv","sr","preoverlay","composite","final","replay","srpostfx","visiblehud","visiblefinal"};
    for(unsigned i=0;i<azpv_batch_count;++i){
        AzpvBatch *b=&azpv_batches[i];
        if(!b->life.state||b->group>=AZPV_GROUPS)continue;
        if(b->life.state==PV_SUBMITTED){
            UINT64 value=b->fence->lpVtbl->GetCompletedValue(b->fence);
            if(value==UINT64_MAX){b->life.state=PV_FAILED;log_msg("PREVIEW FAILED resolve=%llu batch=%u reason=device_removed",b->resolve,i+1);}
            /* Wait for the successful reset too, so a repeated submission
               cannot race mapping or silently overwrite the captured data. */
            else if(azpv_can_map(b->life,value)){
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
                for(unsigned j=0;j<11;++j)if(!strcmp(b->shots[k].name,names[j]))saved_mask[b->group]|=1u<<j;
        }else if(b->life.state!=PV_FAILED)++pending;
    }
    if(ended&&!pending){
        for(unsigned i=0;i<azpv_count;++i){
            AzpvGroup *g=&azpv_groups[i];
            unsigned expected=(azpv_postfx?127u:15u)|(azrp_enabled?128u:0u)|(azrp_sr_enabled?256u:0u)|(g->visible.enabled?1536u:0u);
            int core=(saved_mask[i]&15u)==15u;
            if(core)++good_groups;else ++failed_groups;
            log_msg("PREVIEW GROUP_RESULT resolve=%llu expected=0x%02x attempted=0x%02x saved=0x%02x postfxSeen=0x%x coreComplete=%d",
                g->resolve,expected,g->requested_mask,saved_mask[i],g->postfx_seen,core);
        }
        if(!azpv_count)log_msg("PREVIEW NO_GROUPS: no successful selected probe, queue unavailable, or external capture runtime");
        azpv_done=1;log_msg("PREVIEW COMPLETE groups=%u saved=%u failed=%u recordedFiles=%u batches=%u",azpv_count,good_groups,failed_groups,files,azpv_batch_count);
    }
done: ReleaseSRWLockExclusive(&azpv_lock);
}
