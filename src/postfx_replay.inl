/* First native high-resolution postprocess replay: keep all original
   descriptor bindings and root/IA state; patch only render-domain position
   loads. This is a baseline, not yet SR t0 substitution or visible routing.
   Opt-in, fixed 1280x720 -> 1920x1080, at most two isolated RTVs. */
#include "postfx_dxbc.h"
#include "replay_bindings.inl"
#define AZRP_MAX 8
static int azrp_enabled;
static SRWLOCK azrp_lock=SRWLOCK_INIT;
static struct {ID3D12PipelineState *original,*variant;} azrp_psos[AZRP_MAX];
static unsigned azrp_attempts;
static struct {ID3D12Resource *image;ID3D12DescriptorHeap *rtv,*srv;} azrp_surfaces[4];
static unsigned azrp_surface_count;

static void postfx_replay_init(void) {
    char b[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_POSTFX_REPLAY",b,sizeof(b));
    azrp_enabled=dlss_sr_preview_mode()&&n==1&&b[0]=='1'&&!GetModuleHandleW(L"renderdoc.dll");
    if(azrp_enabled){log_msg("POSTFX REPLAY CONFIG native t0, 1280x720 -> 1920x1080, offscreen only");azrp_shadow_init();}
}
static void postfx_replay_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d,ID3D12PipelineState *pso) {
    if(!azrp_enabled||!d||!pso||!d->PS.pShaderBytecode||!d->VS.pShaderBytecode||
       d->PS.BytecodeLength<20||d->PS.BytecodeLength>1048576||d->VS.BytecodeLength<20)return;
    static const DWORD ps[]={0x479be24a,0x12d3a1cc,0x5ae78b8d,0xe91827a4};
    static const DWORD vs[]={0x2098e1f9,0x8e0d8b8e,0x7bdf6434,0x4ec0d391};
    if(memcmp((const BYTE *)d->PS.pShaderBytecode+4,ps,16)||memcmp((const BYTE *)d->VS.pShaderBytecode+4,vs,16))return;
    const D3D12_RENDER_TARGET_BLEND_DESC *blend=&d->BlendState.RenderTarget[0];
    if(!d->pRootSignature||d->NumRenderTargets!=1||d->RTVFormats[0]!=DXGI_FORMAT_R10G10B10A2_UNORM||
       d->DSVFormat!=DXGI_FORMAT_UNKNOWN||d->DepthStencilState.DepthEnable||d->DepthStencilState.StencilEnable||
       blend->BlendEnable||blend->LogicOpEnable||blend->RenderTargetWriteMask!=D3D12_COLOR_WRITE_ENABLE_ALL||
       d->BlendState.AlphaToCoverageEnable||d->SampleMask!=UINT_MAX||d->SampleDesc.Count!=1||d->SampleDesc.Quality||
       d->PrimitiveTopologyType!=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE||d->StreamOutput.NumEntries||
       d->GS.BytecodeLength||d->HS.BytecodeLength||d->DS.BytecodeLength){
        log_msg("POSTFX REPLAY PSO skipped: unsupported original state");return;
    }
    AcquireSRWLockExclusive(&azrp_lock);
    for(unsigned i=0;i<AZRP_MAX;++i)if(azrp_psos[i].original==pso)goto done;
    if(azrp_attempts>=AZRP_MAX)goto done;
    unsigned slot=azrp_attempts++;
    size_t cap=d->PS.BytecodeLength+80;
    void *patched=HeapAlloc(GetProcessHeap(),0,cap);
    if(!patched){log_msg("POSTFX REPLAY PSO allocation failed");goto done;}
    size_t size=azdx_patch_positions(d->PS.pShaderBytecode,d->PS.BytecodeLength,patched,cap,2.0f/3.0f,2.0f/3.0f,2);
    if(!size){log_msg("POSTFX REPLAY PSO rejected: container checksum/token match");HeapFree(GetProcessHeap(),0,patched);goto done;}
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc=*d;
    desc.PS.pShaderBytecode=patched;desc.PS.BytecodeLength=size;
    desc.CachedPSO.pCachedBlob=NULL;desc.CachedPSO.CachedBlobSizeInBytes=0;
    ID3D12PipelineState *variant=NULL;
    HRESULT hr=real_CreateGraphicsPipelineState(g_device,&desc,&IID_ID3D12PipelineState,(void **)&variant);
    HeapFree(GetProcessHeap(),0,patched);
    if(FAILED(hr)||!variant){log_msg("POSTFX REPLAY PSO create failed hr=%08lx",(unsigned long)hr);goto done;}
    ID3D12PipelineState_AddRef(pso);azrp_psos[slot].original=pso;azrp_psos[slot].variant=variant;
    log_msg("POSTFX REPLAY PSO ready original=%p variant=%p remappedPositionLoads=2",(void *)pso,(void *)variant);
done: ReleaseSRWLockExclusive(&azrp_lock);
}
static ID3D12Resource *postfx_replay_draw(ID3D12GraphicsCommandList *cl,ListState *s,unsigned long long resolve,const AzrpBindings *bindings) {
    if(!azrp_enabled||!s||!cl||s->num_rtv!=1||s->has_dsv||s->num_vp!=1||s->num_sc!=1||
       s->vp_jittered||s->vp[0].Width!=1280.0f||s->vp[0].Height!=720.0f||
       s->vp[0].TopLeftX!=0||s->vp[0].TopLeftY!=0||s->trace_predicated||s->trace_queries||
       s->trace_query_unknown||s->trace_topology!=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST||
       azrp_surface_count>=4)return NULL;
    ID3D12PipelineState *variant=NULL;
    AcquireSRWLockShared(&azrp_lock);
    for(unsigned i=0;i<AZRP_MAX;++i)if(azrp_psos[i].original==s->pso){variant=azrp_psos[i].variant;break;}
    ReleaseSRWLockShared(&azrp_lock);
    if(!variant){log_msg("POSTFX REPLAY SKIP resolve=%llu reason=no_patched_pso",resolve);return NULL;}
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC rd={0};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=1920;rd.Height=1080;
    rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R10G10B10A2_UNORM;rd.SampleDesc.Count=1;
    rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *image=NULL;
    if(FAILED(ID3D12Device_CreateCommittedResource(g_device,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void **)&image))||!image)return NULL;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    ID3D12DescriptorHeap *heap=NULL;
    if(FAILED(ID3D12Device_CreateDescriptorHeap(g_device,&hd,&IID_ID3D12DescriptorHeap,(void **)&heap))||!heap){ID3D12Resource_Release(image);return NULL;}
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap,&rtv);
    real_CreateRenderTargetView(g_device,image,NULL,rtv);
    unsigned idx=azrp_surface_count++;
    azrp_surfaces[idx].image=image;azrp_surfaces[idx].rtv=heap;
    if(bindings)azrp_surfaces[idx].srv=bindings->heap;
    /* Same root signature, descriptor heaps, root constants, VB/IB, topology.
       Only the four states changed here are restored. All surfaces/PSOs
       remain alive until process exit. Never write the original target. */
    D3D12_VIEWPORT vp={0,0,1920,1080,s->vp[0].MinDepth,s->vp[0].MaxDepth};
    D3D12_RECT sc={0,0,1920,1080};
    if(bindings){
        real_SetDescriptorHeaps(cl,bindings->num_heaps,bindings->heaps);
        for(UINT i=0;i<32;++i)if(bindings->mask&(1u<<i))real_SetGraphicsRootDescriptorTable(cl,i,bindings->tables[i]);
    }
    real_SetPipelineState(cl,variant);
    real_OMSetRenderTargets(cl,1,&rtv,FALSE,NULL);
    real_RSSetViewports(cl,1,&vp);real_RSSetScissorRects(cl,1,&sc);
    real_DrawInstanced(cl,3,1,0,0);
    real_SetPipelineState(cl,s->pso);
    real_OMSetRenderTargets(cl,s->num_rtv,s->rtv,s->rtv_single,NULL);
    real_RSSetViewports(cl,s->num_vp,s->vp);real_RSSetScissorRects(cl,s->num_sc,s->sc);
    if(bindings){
        real_SetDescriptorHeaps(cl,s->num_heaps,s->heaps);
        for(UINT i=0;i<32;++i){
            if(s->trace_tables_mask&(1u<<i))real_SetGraphicsRootDescriptorTable(cl,i,s->tables[i]);
            if(s->trace_compute_mask&(1u<<i))real_SetComputeRootDescriptorTable(cl,i,s->trace_compute_tables[i]);
        }
    }
    log_msg("POSTFX REPLAY RECORDED resolve=%llu size=1920x1080 native_t0=%d offscreen=1",resolve,bindings?0:1);
    return image;
}
