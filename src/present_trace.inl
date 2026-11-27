/* Optional, passive observation on a LIVE swapchain call. Independent
   from the value-only Visible lineage observer. No swapchain, queue,
   or backbuffer references survive a hook invocation. No present takeover,
   no sync-interval change, no FG. Vtable originals are keyed by slot. */
#define AZDX_SLOTS 64u
static SRWLOCK azdx_lock=SRWLOCK_INIT;
static struct {void **slot;void *original;} azdx_slots[AZDX_SLOTS];
static unsigned azdx_count;
static volatile LONG64 azdx_call_serial;
static void *azdx_original(void **slot){
    void *original=NULL;
    AcquireSRWLockShared(&azdx_lock);
    for(unsigned i=0;i<azdx_count;++i)if(azdx_slots[i].slot==slot){original=azdx_slots[i].original;break;}
    ReleaseSRWLockShared(&azdx_lock);
    return original;
}
static int azdx_patch(void **slot,void *hook){
    int result=0;DWORD protection=0,ignored=0;
    AcquireSRWLockExclusive(&azdx_lock);
    for(unsigned i=0;i<azdx_count;++i)if(azdx_slots[i].slot==slot){
        result=*slot==hook;goto done;
    }
    if(azdx_count>=AZDX_SLOTS||!*slot||*slot==hook)goto done;
    if(!VirtualProtect(slot,sizeof(void *),PAGE_EXECUTE_READWRITE,&protection))goto done;
    azdx_slots[azdx_count].slot=slot;azdx_slots[azdx_count++].original=*slot;
    InterlockedExchangePointer((PVOID volatile *)slot,hook);
    VirtualProtect(slot,sizeof(void *),protection,&ignored);
    result=1;
done:
    ReleaseSRWLockExclusive(&azdx_lock);
    return result;
}
typedef struct {
    uintptr_t resource,hwnd;
    unsigned index,w,h,format,desc_w,desc_h,desc_format,buffer_count;
    int buffer_ok;
} AzdxObservation;
/* Called only while the app owns a live interface and before calling
   the real Present. Release GetBuffer and QI refs BEFORE original Present,
   including on failure. Never use a creation snapshot as a live pointer. */
static AzdxObservation azdx_observe(IDXGISwapChain1 *sc){
    AzdxObservation v={0};v.index=UINT_MAX;
    IDXGISwapChain3 *s3=NULL;
    if(FAILED(IUnknown_QueryInterface((IUnknown *)sc,&IID_IDXGISwapChain3,(void **)&s3))||!s3)return v;
    DXGI_SWAP_CHAIN_DESC1 d={0};HWND hwnd=NULL;
    if(SUCCEEDED(s3->lpVtbl->GetDesc1(s3,&d))){
        v.desc_w=d.Width;v.desc_h=d.Height;v.desc_format=(unsigned)d.Format;v.buffer_count=d.BufferCount;
    }
    if(SUCCEEDED(s3->lpVtbl->GetHwnd(s3,&hwnd)))v.hwnd=(uintptr_t)hwnd;
    v.index=s3->lpVtbl->GetCurrentBackBufferIndex(s3);
    ID3D12Resource *r=NULL;
    if(v.index<v.buffer_count&&SUCCEEDED(s3->lpVtbl->GetBuffer(s3,v.index,&IID_ID3D12Resource,(void **)&r))&&r){
        D3D12_RESOURCE_DESC rd;r->lpVtbl->GetDesc(r,&rd);
        v.resource=(uintptr_t)r;v.w=(unsigned)rd.Width;v.h=rd.Height;v.format=(unsigned)rd.Format;v.buffer_ok=1;
        ID3D12Resource_Release(r);
    }
    s3->lpVtbl->Release(s3);
    return v;
}
static unsigned long long azdx_begin(IDXGISwapChain1 *sc,const char *method,UINT sync,UINT flags){
    if(!azvis_observing()&&!azsl_managed_chain)return 0;
    unsigned long long id=(unsigned long long)InterlockedIncrement64(&azdx_call_serial);
    AzdxObservation v=azdx_observe(sc);
    log_msg("DXGI PRESENT_BEGIN call=%llu method=%s chain=%p hwnd=%p tid=%lu sync=%u flags=%u test=%d index=%u buffer=%p bufferOK=%d size=%ux%u fmt=%u descSize=%ux%u descFmt=%u buffers=%u qpc=%lld noRetainedRefs=1",
        id,method,(void *)sc,(void *)v.hwnd,(unsigned long)GetCurrentThreadId(),sync,flags,(flags&DXGI_PRESENT_TEST)!=0,v.index,(void *)v.resource,v.buffer_ok,v.w,v.h,v.format,v.desc_w,v.desc_h,v.desc_format,v.buffer_count,azvis_qpc());
    return id;
}
static void azdx_end(unsigned long long id,HRESULT hr){
    if(id)log_msg("DXGI PRESENT_END call=%llu hr=%08lx tid=%lu qpc=%lld",id,(unsigned long)hr,(unsigned long)GetCurrentThreadId(),azvis_qpc());
}
static HRESULT STDMETHODCALLTYPE azdx_present(IDXGISwapChain1 *sc,UINT sync,UINT flags){
    typedef HRESULT (STDMETHODCALLTYPE *Fn)(IDXGISwapChain1 *,UINT,UINT);
    Fn original=(Fn)azdx_original(VT_SLOT(sc,IDXGISwapChain1Vtbl,Present));
    if(!original)return E_UNEXPECTED;
    unsigned long long id=azdx_begin(sc,"Present",sync,flags);
    int tagged=azsl_before&&sc==azsl_managed_chain?azsl_before(sc,flags,id):0;
    HRESULT hr=original(sc,sync,flags);
    if(tagged&&azsl_after)azsl_after(hr);
    azdx_end(id,hr);return hr;
}
static HRESULT STDMETHODCALLTYPE azdx_present1(IDXGISwapChain1 *sc,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS *params){
    typedef HRESULT (STDMETHODCALLTYPE *Fn)(IDXGISwapChain1 *,UINT,UINT,const DXGI_PRESENT_PARAMETERS *);
    Fn original=(Fn)azdx_original(VT_SLOT(sc,IDXGISwapChain1Vtbl,Present1));
    if(!original)return E_UNEXPECTED;
    unsigned long long id=azdx_begin(sc,"Present1",sync,flags);
    int tagged=azsl_before&&sc==azsl_managed_chain?azsl_before(sc,flags,id):0;
    HRESULT hr=original(sc,sync,flags,params);
    if(tagged&&azsl_after)azsl_after(hr);
    azdx_end(id,hr);return hr;
}
static void azdx_resize_guard(const char *method,void *sc,UINT count,UINT w,UINT h,DXGI_FORMAT fmt){
    if(azsl_managed_chain){if(sc==(void *)azsl_managed_chain&&azsl_change){InterlockedExchange(&azsl_profile_ready,0);azsl_change(azsl_managed_chain,0,0,S_OK);}}
    else if(azvis_observing()){InterlockedExchange(&azvis_chain_changed,1);dlss_sr_play_disable("swapchain_resize");}
    log_msg("DXGI RESIZE_BEGIN method=%s chain=%p size=%ux%u buffers=%u fmt=%u trialDisabled=%d qpc=%lld",
        method,sc,w,h,count,(unsigned)fmt,azvis_observing(),azvis_qpc());
}
static HRESULT STDMETHODCALLTYPE azdx_resize(IDXGISwapChain1 *sc,UINT count,UINT w,UINT h,DXGI_FORMAT fmt,UINT flags){
    typedef HRESULT (STDMETHODCALLTYPE *Fn)(IDXGISwapChain1 *,UINT,UINT,UINT,DXGI_FORMAT,UINT);
    Fn original=(Fn)azdx_original(VT_SLOT(sc,IDXGISwapChain1Vtbl,ResizeBuffers));
    if(!original)return E_UNEXPECTED;
    azdx_resize_guard("ResizeBuffers",(void *)sc,count,w,h,fmt);
    HRESULT hr=original(sc,count,w,h,fmt,flags);
    if(sc==azsl_managed_chain&&azsl_change){azsl_change(sc,1,0,hr);azsl_update_ready();}
    log_msg("DXGI RESIZE_END method=ResizeBuffers chain=%p hr=%08lx qpc=%lld",(void *)sc,(unsigned long)hr,azvis_qpc());
    return hr;
}
static HRESULT STDMETHODCALLTYPE azdx_resize1(IDXGISwapChain3 *sc,UINT count,UINT w,UINT h,DXGI_FORMAT fmt,UINT flags,const UINT *masks,IUnknown *const *queues){
    typedef HRESULT (STDMETHODCALLTYPE *Fn)(IDXGISwapChain3 *,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT *,IUnknown *const *);
    Fn original=(Fn)azdx_original(VT_SLOT(sc,IDXGISwapChain3Vtbl,ResizeBuffers1));
    if(!original)return E_UNEXPECTED;
    azdx_resize_guard("ResizeBuffers1",(void *)sc,count,w,h,fmt);
    HRESULT hr=original(sc,count,w,h,fmt,flags,masks,queues);
    if((void *)sc==(void *)azsl_managed_chain&&azsl_change){azsl_change(azsl_managed_chain,1,0,hr);azsl_update_ready();}
    log_msg("DXGI RESIZE_END method=ResizeBuffers1 chain=%p hr=%08lx qpc=%lld",(void *)sc,(unsigned long)hr,azvis_qpc());
    return hr;
}
static HRESULT STDMETHODCALLTYPE azdx_color(IDXGISwapChain3 *sc,DXGI_COLOR_SPACE_TYPE color){
    typedef HRESULT (STDMETHODCALLTYPE *Fn)(IDXGISwapChain3 *,DXGI_COLOR_SPACE_TYPE);
    Fn original=(Fn)azdx_original(VT_SLOT(sc,IDXGISwapChain3Vtbl,SetColorSpace1));
    if(!original)return E_UNEXPECTED;
    HRESULT hr=original(sc,color);
    if(azsl_managed_chain){if((void *)sc==(void *)azsl_managed_chain&&azsl_change){azsl_change(azsl_managed_chain,2,(unsigned)color,hr);azsl_update_ready();}}
    else if(SUCCEEDED(hr)&&azvis_observing()){InterlockedExchange(&azvis_chain_changed,1);dlss_sr_play_disable("colorspace_change");}
    log_msg("DXGI COLORSPACE chain=%p value=%u hr=%08lx trialDisabled=%d qpc=%lld",(void *)sc,(unsigned)color,(unsigned long)hr,SUCCEEDED(hr)&&azvis_observing(),azvis_qpc());
    return hr;
}
static void azdx_install(IDXGISwapChain1 *sc){
    char value[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_PRESENT_TRACE",value,sizeof(value));
    int play=dlss_sr_play_mode();
    if(!sc||(!play&&!azsl_requested()&&(n!=1||value[0]!='1')))return;
    int p=play?0:azdx_patch(VT_SLOT(sc,IDXGISwapChain1Vtbl,Present),(void *)azdx_present);
    int p1=play?0:azdx_patch(VT_SLOT(sc,IDXGISwapChain1Vtbl,Present1),(void *)azdx_present1);
    int r=azdx_patch(VT_SLOT(sc,IDXGISwapChain1Vtbl,ResizeBuffers),(void *)azdx_resize);
    int p3=0,p13=0,r3=0,r1=0,color=0;
    uintptr_t id3=0;
    IDXGISwapChain3 *s3=NULL;
    if(SUCCEEDED(IUnknown_QueryInterface((IUnknown *)sc,&IID_IDXGISwapChain3,(void **)&s3))&&s3){
        id3=(uintptr_t)s3;
        p3=play?0:azdx_patch(VT_SLOT(s3,IDXGISwapChain3Vtbl,Present),(void *)azdx_present);
        p13=play?0:azdx_patch(VT_SLOT(s3,IDXGISwapChain3Vtbl,Present1),(void *)azdx_present1);
        r3=azdx_patch(VT_SLOT(s3,IDXGISwapChain3Vtbl,ResizeBuffers),(void *)azdx_resize);
        r1=azdx_patch(VT_SLOT(s3,IDXGISwapChain3Vtbl,ResizeBuffers1),(void *)azdx_resize1);
        color=azdx_patch(VT_SLOT(s3,IDXGISwapChain3Vtbl,SetColorSpace1),(void *)azdx_color);
        s3->lpVtbl->Release(s3);
    }
    if(play&&(!r||!r3||!r1||!color))dlss_sr_play_disable("resize_colorspace_hook_unavailable");
    log_msg("DXGI TRACE_INSTALL chain=%p chain3=%p present=%d present1=%d resize=%d present3=%d present13=%d resize3=%d resize1=%d color=%d noRetainedRefs=1 qpc=%lld",
        (void *)sc,(void *)id3,p,p1,r,p3,p13,r3,r1,color,azvis_qpc());
}
