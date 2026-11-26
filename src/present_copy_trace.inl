/* Opt-in bounded copy-source fingerprints and shared-handle observation.
   Never read a live swapchain by guessing its state. The sampled source
   is already used by a native full-size copy in this recording. No
   source transition or reference is added; private readback + fence and
   list identity are retained until a successful reset and completion.
   Evidence only, no frame generation or presentation modification. */
#include "present_copy_policy.h"
static int azpc_enabled;
static uint64_t azpc_first;
static SRWLOCK azpc_lock=SRWLOCK_INIT;
typedef struct {
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *readback;
    ID3D12Fence *fence;
    uintptr_t src,dst,device;
    uint64_t recording,resolve;
    AzpvBatchLife life;
    unsigned w,h;
} AzpcShot;
static AzpcShot azpc_shots[AZPC_MAX];
static unsigned azpc_count;
static void azpc_init(void){
    char b[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_PRESENT_COPY",b,sizeof(b));
    azpc_enabled=azvis_enabled&&n==1&&b[0]=='1';
    if(azpc_enabled)log_msg("LINEAGE CONFIG maxSamples=%u resolveWindow=%u crop=%ux%u format=24 sourceOnly=1 noSourceRefs=1",AZPC_MAX,AZPC_WINDOW,AZPC_SIDE,AZPC_SIDE);
}
static int azpc_active_locked(void){
    return azpc_enabled&&azvis_observing()&&azdbg.id==1&&azpc_window(azpc_first,azt_frame);
}
static int azpc_shape(const D3D12_RESOURCE_DESC *d){
    return d->Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&azrp_match_output(azrp_profile,d->Width,d->Height)&&
        d->DepthOrArraySize==1&&d->MipLevels==1&&d->SampleDesc.Count==1&&!d->SampleDesc.Quality&&
        d->Format==DXGI_FORMAT_R10G10B10A2_UNORM&&azpc_crop_ok((unsigned)d->Width,d->Height);
}
static void azpc_copy(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst,ID3D12Resource *src,int full,const char *kind){
    if(!azpc_enabled||!azvis_observing()||!full||!cl||!src||!dst||src==dst||!real_CopyTextureRegion)return;
    D3D12_RESOURCE_DESC a,b;src->lpVtbl->GetDesc(src,&a);dst->lpVtbl->GetDesc(dst,&b);
    if(!azpc_shape(&a)||!azpc_shape(&b)||cl->lpVtbl->GetType(cl)!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
    ListState *s=state_for(cl);
    AcquireSRWLockExclusive(&azt_lock);
    int active=azpc_active_locked();
    uint64_t resolve=azt_frame,recording=0;
    if(active&&s&&s->trace_bindings_known){
        if(!s->trace_recording_id)s->trace_recording_id=++azt_generation;
        recording=s->trace_recording_id;
    }
    ReleaseSRWLockExclusive(&azt_lock);
    if(!active)return;
    AcquireSRWLockExclusive(&azpc_lock);
    if(azpc_count>=AZPC_MAX)goto done;
    unsigned slot=azpc_count++;
    AzpcShot *p=&azpc_shots[slot];
    p->src=(uintptr_t)src;p->dst=(uintptr_t)dst;p->resolve=resolve;p->recording=recording;
    p->w=(unsigned)a.Width;p->h=a.Height;
    log_msg("LINEAGE COPY id=%u resolveObserved=%llu list=%p recording=%llu src=%p dst=%p size=%ux%u fmt=24 kind=%s qpc=%lld",
        slot+1,(unsigned long long)resolve,(void *)cl,(unsigned long long)recording,(void *)src,(void *)dst,p->w,p->h,kind,azvis_qpc());
    if(!recording)goto fail;
    ID3D12Device *dev=NULL;
    if(FAILED(cl->lpVtbl->GetDevice(cl,&IID_ID3D12Device,(void **)&dev))||!dev)goto fail;
    p->device=(uintptr_t)dev;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC bd={0};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=2u*AZPC_BYTES;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr=ID3D12Device_CreateCommittedResource(dev,&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void **)&p->readback);
    if(SUCCEEDED(hr)&&p->readback)hr=ID3D12Device_CreateFence(dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void **)&p->fence);
    ID3D12Device_Release(dev);
    if(FAILED(hr)||!p->readback||!p->fence){
        if(p->readback){ID3D12Resource_Release(p->readback);p->readback=NULL;}
        if(p->fence){ID3D12Fence_Release(p->fence);p->fence=NULL;}
        goto fail;
    }
    ID3D12GraphicsCommandList_AddRef(cl);p->list=cl;p->life=(AzpvBatchLife){PV_RECORDED,1};
    D3D12_TEXTURE_COPY_LOCATION from={0},to={0};
    from.pResource=src;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=p->readback;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
    to.PlacedFootprint.Footprint.Width=AZPC_SIDE;to.PlacedFootprint.Footprint.Height=AZPC_SIDE;
    to.PlacedFootprint.Footprint.Depth=1;to.PlacedFootprint.Footprint.RowPitch=AZPC_PITCH;
    for(unsigned k=0;k<2;++k){
        unsigned x=k?p->w/2-AZPC_SIDE/2:32u,y=k?p->h/2-AZPC_SIDE/2:32u;
        D3D12_BOX box={x,y,0,x+AZPC_SIDE,y+AZPC_SIDE,1};
        to.PlacedFootprint.Offset=(UINT64)k*AZPC_BYTES;
        real_CopyTextureRegion(cl,&to,0,0,0,&from,&box);
    }
    log_msg("LINEAGE SAMPLE_RECORDED id=%u device=%p sourceState=native_copy_source noSourceRefs=1",slot+1,(void *)p->device);
    goto done;
fail:
    p->life.state=PV_FAILED;
    log_msg("LINEAGE SAMPLE_SKIP id=%u reason=recording_or_allocation",slot+1);
done:
    ReleaseSRWLockExclusive(&azpc_lock);
}
static void azpc_reset(ID3D12GraphicsCommandList *cl){
    if(!azpc_enabled)return;
    AcquireSRWLockExclusive(&azpc_lock);
    for(unsigned i=0;i<azpc_count;++i){
        AzpcShot *p=&azpc_shots[i];
        if(p->list==cl&&p->life.current){azpv_on_reset(&p->life);if(p->life.state==PV_FAILED)log_msg("LINEAGE SAMPLE_FAILED id=%u reason=reset_before_signal_or_ambiguous",i+1);}
    }
    ReleaseSRWLockExclusive(&azpc_lock);
}
static void azpc_submit(ID3D12CommandQueue *q,UINT n,ID3D12CommandList *const *lists){
    if(!azpc_enabled)return;
    AcquireSRWLockExclusive(&azpc_lock);
    for(unsigned i=0;i<azpc_count;++i){
        AzpcShot *p=&azpc_shots[i];
        if(!p->life.current||!p->list)continue;
        unsigned matches=0;
        for(UINT j=0;lists&&j<n;++j)if((void *)lists[j]==(void *)p->list)++matches;
        if(!matches)continue;
        HRESULT hr=E_FAIL;
        if(matches==1&&p->life.state==PV_RECORDED)hr=q->lpVtbl->Signal(q,p->fence,1);
        azpv_on_submit(&p->life,matches,SUCCEEDED(hr));
        log_msg("LINEAGE COPY_SUBMITTED id=%u recording=%llu queue=%p fence=%p matches=%u hr=%08lx qpc=%lld",
            i+1,(unsigned long long)p->recording,(void *)q,(void *)p->fence,matches,(unsigned long)hr,azvis_qpc());
    }
    ReleaseSRWLockExclusive(&azpc_lock);
}
static void azpc_poll(void){
    if(!azpc_enabled)return;
    AcquireSRWLockExclusive(&azpc_lock);
    for(unsigned i=0;i<azpc_count;++i){
        AzpcShot *p=&azpc_shots[i];
        if(p->life.state!=PV_SUBMITTED||p->life.current)continue;
        UINT64 value=p->fence->lpVtbl->GetCompletedValue(p->fence);
        if(value==UINT64_MAX){p->life.state=PV_FAILED;log_msg("LINEAGE SAMPLE_FAILED id=%u reason=device_removed",i+1);continue;}
        if(!azpv_can_map(p->life,value))continue;
        void *data=NULL;D3D12_RANGE read={0,2u*AZPC_BYTES},empty={0,0};
        if(SUCCEEDED(p->readback->lpVtbl->Map(p->readback,0,&read,&data))&&data){
            const unsigned char *bytes=(const unsigned char *)data;
            log_msg("LINEAGE SAMPLE id=%u resolveObserved=%llu src=%p dst=%p hashCorner=%016llx hashCenter=%016llx size=%ux%u fenceComplete=1 recordingRetired=1",
                i+1,(unsigned long long)p->resolve,(void *)p->src,(void *)p->dst,(unsigned long long)azpc_hash(bytes,AZPC_PITCH),(unsigned long long)azpc_hash(bytes+AZPC_BYTES,AZPC_PITCH),p->w,p->h);
            p->readback->lpVtbl->Unmap(p->readback,0,&empty);
            p->life.state=PV_SAVED;
        }else{p->life.state=PV_FAILED;log_msg("LINEAGE SAMPLE_FAILED id=%u reason=map",i+1);}
        ID3D12Resource_Release(p->readback);p->readback=NULL;
        ID3D12Fence_Release(p->fence);p->fence=NULL;
        ID3D12GraphicsCommandList_Release(p->list);p->list=NULL;
    }
    ReleaseSRWLockExclusive(&azpc_lock);
}
static void azpc_final_hash(unsigned long long resolve,const void *data,size_t pitch,unsigned w,unsigned h,unsigned format){
    if(!azpc_enabled||!data||format!=DXGI_FORMAT_R10G10B10A2_UNORM||!azpc_crop_ok(w,h)||pitch<(size_t)w*4)return;
    const unsigned char *p=(const unsigned char *)data;
    const unsigned char *corner=p+32u*pitch+32u*4u;
    const unsigned char *center=p+(size_t)(h/2-AZPC_SIDE/2)*pitch+(size_t)(w/2-AZPC_SIDE/2)*4u;
    log_msg("LINEAGE FINAL_SAMPLE resolve=%llu hashCorner=%016llx hashCenter=%016llx size=%ux%u",resolve,(unsigned long long)azpc_hash(corner,pitch),(unsigned long long)azpc_hash(center,pitch),w,h);
}
/* Numeric shared handle observations are candidates only. A handle can
   be duplicated, closed, or reused; do not infer allocation identity
   across missing observations. No handle ownership or QI ref retained. */
static HRESULT (STDMETHODCALLTYPE *azpc_real_share)(ID3D12Device *,ID3D12DeviceChild *,const SECURITY_ATTRIBUTES *,DWORD,LPCWSTR,HANDLE *);
static HRESULT (STDMETHODCALLTYPE *azpc_real_open)(ID3D12Device *,HANDLE,REFIID,void **);
static void azpc_shared(const char *method,ID3D12Device *dev,IUnknown *obj,HANDLE handle){
    ID3D12Resource *r=NULL;ID3D12Fence *f=NULL;
    if(obj){
        (void)IUnknown_QueryInterface(obj,&IID_ID3D12Resource,(void **)&r);
        (void)IUnknown_QueryInterface(obj,&IID_ID3D12Fence,(void **)&f);
    }
    log_msg("LINEAGE SHARED_%s device=%p object=%p handle=%p resource=%p fence=%p qpc=%lld valueOnly=1",
        method,(void *)dev,(void *)obj,(void *)handle,(void *)r,(void *)f,azvis_qpc());
    if(r)ID3D12Resource_Release(r);
    if(f)ID3D12Fence_Release(f);
}
static HRESULT STDMETHODCALLTYPE azpc_share(ID3D12Device *dev,ID3D12DeviceChild *obj,const SECURITY_ATTRIBUTES *attr,DWORD access,LPCWSTR name,HANDLE *out){
    HRESULT hr=azpc_real_share(dev,obj,attr,access,name,out);
    if(azpc_enabled&&SUCCEEDED(hr)&&out&&*out)azpc_shared("CREATE",dev,(IUnknown *)obj,*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE azpc_open(ID3D12Device *dev,HANDLE handle,REFIID riid,void **out){
    HRESULT hr=azpc_real_open(dev,handle,riid,out);
    if(azpc_enabled&&SUCCEEDED(hr)&&out&&*out)azpc_shared("OPEN",dev,(IUnknown *)*out,handle);
    return hr;
}
static void azpc_hook_device(ID3D12Device *dev){
    if(!azpc_enabled)return;
    void **create=VT_SLOT(dev,ID3D12DeviceVtbl,CreateSharedHandle),**open=VT_SLOT(dev,ID3D12DeviceVtbl,OpenSharedHandle);
    if(!azpc_real_share||*create==(void *)azpc_real_share||*create==(void *)azpc_share)patch_slot(create,(void *)azpc_share,(void **)&azpc_real_share);
    if(!azpc_real_open||*open==(void *)azpc_real_open||*open==(void *)azpc_open)patch_slot(open,(void *)azpc_open,(void **)&azpc_real_open);
}
