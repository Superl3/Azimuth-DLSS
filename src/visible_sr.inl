/* Opt-in FH2 SDR output trial. Native draws always run first. Only the
   exact final PS/VS pair may be replayed into its original live RTV.
   The scene is full-resolution except where native HUD composition is
   retained. No guessed alpha, no game heap writes, no Present takeover. */
#include "visible_merge_shader.h"
#define AZVIS_PSO_MAX 8
#define AZVIS_MS 10000u
#define AZPV_CAPTURE_COUNT 2u
static int azvis_enabled;
static SRWLOCK azvis_pso_lock=SRWLOCK_INIT;
static struct {ID3D12PipelineState *original,*merge;} azvis_psos[AZVIS_PSO_MAX];
static unsigned azvis_pso_count;
typedef struct {
    int enabled,pre_ready,composite_ready,routed;
    ID3D12GraphicsCommandList *list;
    unsigned long long recording,composite_work;
    uintptr_t target_id;
    AzrpSurface sr_surface;
    ID3D12Resource *layers,*merged;
    ID3D12DescriptorHeap *rtv,*heap;
} AzvisFrame;
static int azvis_window_locked(void){
    return azvis_enabled&&azdbg.id==1&&GetTickCount64()-azdbg.started_ms<AZVIS_MS;
}
static void visible_init(void){
    char b[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_VISIBLE_SR",b,sizeof(b));
    azvis_enabled=azrp_enabled&&azrp_sr_enabled&&n==1&&b[0]=='1';
    if(n==1&&b[0]=='1')log_msg("VISIBLE CONFIG enabled=%d durationMs=%u mode=native_hud_region_retention sdr=1 presentVerified=0",azvis_enabled,AZVIS_MS);
}
static void visible_release(AzvisFrame *v){
    if(v->heap)ID3D12DescriptorHeap_Release(v->heap);
    if(v->rtv)ID3D12DescriptorHeap_Release(v->rtv);
    if(v->merged)ID3D12Resource_Release(v->merged);
    if(v->layers)ID3D12Resource_Release(v->layers);
    if(v->sr_surface.srv)ID3D12DescriptorHeap_Release(v->sr_surface.srv);
    if(v->sr_surface.rtv)ID3D12DescriptorHeap_Release(v->sr_surface.rtv);
    if(v->sr_surface.image)ID3D12Resource_Release(v->sr_surface.image);
    memset(v,0,sizeof(*v));
}
static int azvis_final_pair(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d){
    static const DWORD ps[]={0xa93721c4,0x5975a343,0x749f00dc,0xe74366c0};
    static const DWORD vs[]={0x79d0ca68,0x15da8827,0x24fc6096,0x35dd7508};
    return d&&d->PS.pShaderBytecode&&d->VS.pShaderBytecode&&d->PS.BytecodeLength>=20&&d->VS.BytecodeLength>=20&&
        !memcmp((const BYTE *)d->PS.pShaderBytecode+4,ps,16)&&!memcmp((const BYTE *)d->VS.pShaderBytecode+4,vs,16);
}
static void visible_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d,ID3D12PipelineState *pso){
    if(!azvis_enabled||!pso||!d)return;
    AztPso candidate;memset(&candidate,0,sizeof(candidate));
    if(azsd_hash(&d->PS,candidate.hash)&&azsd_hash(&d->VS,candidate.vs_hash)&&azsd_hud_candidate(&candidate)){
        const D3D12_RENDER_TARGET_BLEND_DESC *b=&d->BlendState.RenderTarget[0];
        const D3D12_DEPTH_STENCIL_DESC *ds=&d->DepthStencilState;
        log_msg("VISIBLE HUD_PSO pso=%p ps=%08lx-%08lx-%08lx-%08lx vs=%08lx-%08lx-%08lx-%08lx blend=%d src=%d dst=%d op=%d srcAlpha=%d dstAlpha=%d opAlpha=%d writeMask=%u depth=%d depthWrite=%d depthFunc=%d stencil=%d stencilReadMask=%u stencilWriteMask=%u front=%d,%d,%d,%d back=%d,%d,%d,%d dsvFmt=%d samples=%u inputElements=%u",
            (void *)pso,(unsigned long)candidate.hash[0],(unsigned long)candidate.hash[1],(unsigned long)candidate.hash[2],(unsigned long)candidate.hash[3],
            (unsigned long)candidate.vs_hash[0],(unsigned long)candidate.vs_hash[1],(unsigned long)candidate.vs_hash[2],(unsigned long)candidate.vs_hash[3],
            b->BlendEnable,(int)b->SrcBlend,(int)b->DestBlend,(int)b->BlendOp,(int)b->SrcBlendAlpha,(int)b->DestBlendAlpha,(int)b->BlendOpAlpha,(unsigned)b->RenderTargetWriteMask,
            ds->DepthEnable,(int)ds->DepthWriteMask,(int)ds->DepthFunc,ds->StencilEnable,(unsigned)ds->StencilReadMask,(unsigned)ds->StencilWriteMask,
            (int)ds->FrontFace.StencilFailOp,(int)ds->FrontFace.StencilDepthFailOp,(int)ds->FrontFace.StencilPassOp,(int)ds->FrontFace.StencilFunc,
            (int)ds->BackFace.StencilFailOp,(int)ds->BackFace.StencilDepthFailOp,(int)ds->BackFace.StencilPassOp,(int)ds->BackFace.StencilFunc,
            (int)d->DSVFormat,d->SampleDesc.Count,d->InputLayout.NumElements);
    }
    if(!azvis_final_pair(d))return;
    const D3D12_RENDER_TARGET_BLEND_DESC *b=&d->BlendState.RenderTarget[0];
    if(!d->pRootSignature||d->NumRenderTargets!=1||d->RTVFormats[0]!=DXGI_FORMAT_R10G10B10A2_UNORM||
       d->DSVFormat!=DXGI_FORMAT_UNKNOWN||d->DepthStencilState.DepthEnable||d->DepthStencilState.StencilEnable||
       b->BlendEnable||b->LogicOpEnable||b->RenderTargetWriteMask!=D3D12_COLOR_WRITE_ENABLE_ALL||
       d->BlendState.AlphaToCoverageEnable||d->SampleMask!=UINT_MAX||d->SampleDesc.Count!=1||d->SampleDesc.Quality||
       d->PrimitiveTopologyType!=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE||d->StreamOutput.NumEntries||
       d->GS.BytecodeLength||d->HS.BytecodeLength||d->DS.BytecodeLength||d->InputLayout.NumElements||
       d->RasterizerState.FillMode!=D3D12_FILL_MODE_SOLID){
        log_msg("VISIBLE PSO SKIP reason=unsupported_final_state");return;
    }
    AcquireSRWLockExclusive(&azvis_pso_lock);
    for(unsigned i=0;i<azvis_pso_count;++i)if(azvis_psos[i].original==pso)goto done;
    if(azvis_pso_count>=AZVIS_PSO_MAX)goto done;
    ID3DBlob *shader=compile(azvis_merge_hlsl,"main","ps_5_0");
    if(!shader){log_msg("VISIBLE PSO SKIP reason=merge_compile");goto done;}
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc=*d;
    desc.PS.pShaderBytecode=ID3D10Blob_GetBufferPointer(shader);desc.PS.BytecodeLength=ID3D10Blob_GetBufferSize(shader);
    desc.CachedPSO.pCachedBlob=NULL;desc.CachedPSO.CachedBlobSizeInBytes=0;
    ID3D12PipelineState *merge=NULL;
    HRESULT hr=real_CreateGraphicsPipelineState(g_device,&desc,&IID_ID3D12PipelineState,(void **)&merge);
    ID3D10Blob_Release(shader);
    if(FAILED(hr)||!merge){log_msg("VISIBLE PSO SKIP reason=merge_pso hr=%08lx",(unsigned long)hr);goto done;}
    ID3D12PipelineState_AddRef(pso);unsigned i=azvis_pso_count++;
    azvis_psos[i].original=pso;azvis_psos[i].merge=merge;
    log_msg("VISIBLE PSO READY original=%p merge=%p native_final_lut=1",(void *)pso,(void *)merge);
done: ReleaseSRWLockExclusive(&azvis_pso_lock);
}
static int azvis_texture(ID3D12Resource *r,UINT w,UINT h){
    if(!r)return 0;
    D3D12_RESOURCE_DESC d;r->lpVtbl->GetDesc(r,&d);
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==w&&d.Height==h&&
        d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1&&!d.SampleDesc.Quality&&
        (d.Format==DXGI_FORMAT_R10G10B10A2_UNORM||d.Format==DXGI_FORMAT_R10G10B10A2_TYPELESS);
}
static ID3D12Resource *azvis_new_texture(UINT layers,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC d={0};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=1920;d.Height=1080;
    d.DepthOrArraySize=(UINT16)layers;d.MipLevels=1;d.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
    d.SampleDesc.Count=1;d.Flags=flags;
    ID3D12Resource *r=NULL;
    if(FAILED(ID3D12Device_CreateCommittedResource(g_device,&hp,D3D12_HEAP_FLAG_NONE,&d,state,NULL,&IID_ID3D12Resource,(void **)&r)))return NULL;
    return r;
}
static void azvis_copy_layer(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst,UINT layer,ID3D12Resource *src){
    D3D12_TEXTURE_COPY_LOCATION a={0},b={0};
    a.pResource=dst;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;a.SubresourceIndex=layer;
    b.pResource=src;b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.SubresourceIndex=0;
    hooks_transition(cl,src,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    real_CopyTextureRegion(cl,&a,0,0,0,&b,NULL);
    hooks_transition(cl,src,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
}
static void visible_begin(AzvisFrame *v,ID3D12GraphicsCommandList *cl,ListState *s,ID3D12Resource *native,unsigned long long resolve){
    if(!v->enabled||!real_CopyTextureRegion||!azvis_texture(native,1280,720)||!azvis_texture(v->sr_surface.image,1920,1080)){
        log_msg("VISIBLE SKIP resolve=%llu stage=srpostfx reason=native_or_sr_surface",resolve);return;
    }
    v->layers=azvis_new_texture(3,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    if(!v->layers){log_msg("VISIBLE SKIP resolve=%llu stage=srpostfx reason=layer_allocation",resolve);return;}
    v->list=cl;v->recording=s->trace_recording_id;
    azvis_copy_layer(cl,v->layers,0,v->sr_surface.image);
    azvis_copy_layer(cl,v->layers,1,native);
    v->pre_ready=1;
    log_msg("VISIBLE SRPOSTFX_READY resolve=%llu list=%p recording=%llu scene=%p native=%p",resolve,(void *)cl,v->recording,(void *)v->sr_surface.image,(void *)native);
}
static void visible_composite(AzvisFrame *v,ID3D12GraphicsCommandList *cl,ListState *s,ID3D12Resource *native,unsigned long long resolve){
    if(!v->enabled||!v->pre_ready||v->list!=cl||v->recording!=s->trace_recording_id||!azvis_texture(native,1280,720)){
        log_msg("VISIBLE SKIP resolve=%llu stage=composite reason=recording_or_native_surface",resolve);return;
    }
    azvis_copy_layer(cl,v->layers,2,native);
    hooks_transition(cl,v->layers,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    v->composite_ready=1;v->composite_work=s->trace_work_seq;
    log_msg("VISIBLE HUD_INPUT_READY resolve=%llu list=%p recording=%llu native=%p",resolve,(void *)cl,v->recording,(void *)native);
}
/* Resolve only the original final shader's three declared bindings:
   PS t0 2D source, t1 1D curve, s0 linear clamp; no VS resources.
   Root private data also exists for layouts rejected by the 20-binding
   postfx policy. The original root signature and sampler stay native. */
typedef struct {UINT root,offset,mode;} AzvisReg;
static int azvis_reg(const AzrpRootEvidence *e,UINT type,UINT reg,AzvisReg *out){
    unsigned found=0;
    for(UINT i=0;i<e->rows;++i){
        const AzrpSchemaRow *x=&e->row[i];
        if(x->space||x->type!=type||!azrb_vis(x->visibility,AZRB_PS))continue;
        if(x->kind==AZRB_TABLE&&reg>=x->reg&&reg-x->reg<x->count){
            uint64_t offset=(uint64_t)x->offset+reg-x->reg;
            if(x->root>=e->parameters||offset>=UINT32_MAX)return 0;
            *out=(AzvisReg){x->root,(UINT)offset,AZRB_TABLE};++found;
        }else if(x->kind==AZRB_STATIC&&type==AZRB_SAMPLER&&x->reg==reg){
            *out=(AzvisReg){0,0,AZRB_STATIC};++found;
        }
    }
    return found==1;
}
static int azvis_table_span(const AzrpRootEvidence *e,UINT root,UINT *span){
    uint64_t n=0;
    for(UINT i=0;i<e->rows;++i){
        const AzrpSchemaRow *x=&e->row[i];
        if(x->kind!=AZRB_TABLE||x->root!=root)continue;
        if(x->type==AZRB_SAMPLER||!x->count||x->count==UINT_MAX||x->offset==UINT_MAX)return 0;
        uint64_t end=(uint64_t)x->offset+x->count;if(end>n)n=end;
    }
    if(!n||n>AZRB_HEAP_LIMIT)return 0;
    *span=(UINT)n;return 1;
}
static int azvis_final_bindings(ListState *s,ID3D12Resource *native,AzvisFrame *v,AzrpBindings *a,AzrpBindings *b,unsigned long long resolve){
    memset(a,0,sizeof(*a));memset(b,0,sizeof(*b));
    AzrpRootEvidence e;memset(&e,0,sizeof(e));UINT size=(UINT)sizeof(e);
    if(!s->trace_graphics_rs||FAILED(s->trace_graphics_rs->lpVtbl->GetPrivateData(s->trace_graphics_rs,&azrp_layout_key,&size,&e))||
       size!=sizeof(e)||e.overflow||e.rows>AZRP_SCHEMA_ROWS||e.parameters>AZRB_TABLES||(e.flags&0xc00u)){
        log_msg("VISIBLE SKIP resolve=%llu stage=final reason=root_schema",resolve);return 0;
    }
    AzvisReg t0={0},t1={0},s0={0};UINT n0=0,n1=0;
    if(azdbg.active&&azdbg.observed<=AZPV_CAPTURE_COUNT){
        log_msg("VISIBLE FINAL_SCHEMA resolve=%llu root=%p parameters=%u rows=%u flags=0x%x",resolve,(void *)s->trace_graphics_rs,e.parameters,e.rows,e.flags);
        for(UINT i=0;i<e.rows;++i){const AzrpSchemaRow *x=&e.row[i];log_msg("VISIBLE FINAL_SCHEMA_ROW resolve=%llu index=%u vis=%u type=%u reg=%u space=%u count=%u offset=%u kind=%u",resolve,x->root,x->visibility,x->type,x->reg,x->space,x->count,x->offset,x->kind);}
    }
    if(!azvis_reg(&e,AZRB_SRV,0,&t0)||!azvis_reg(&e,AZRB_SRV,1,&t1)||!azvis_reg(&e,AZRB_SAMPLER,0,&s0)||
       t0.mode!=AZRB_TABLE||t1.mode!=AZRB_TABLE||!azvis_table_span(&e,t0.root,&n0)||
       (t0.root!=t1.root&&!azvis_table_span(&e,t1.root,&n1))||
       t0.offset>=n0||t1.offset>=(t0.root==t1.root?n0:n1)||(t0.root==t1.root&&t0.offset==t1.offset)||
       n0+n1>AZRB_HEAP_LIMIT/2){
        log_msg("VISIBLE SKIP resolve=%llu stage=final reason=final_required_layout",resolve);return 0;
    }
    int ok=0;const char *reason="restore";
    AcquireSRWLockExclusive(&azt_lock);
    AztRoot *gr=azt_root_slot(s->trace_graphics_rs,0),*cr=s->compute_rs?azt_root_slot(s->compute_rs,0):NULL;
    uint64_t gv=azrp_valid_tables(s,gr,s->trace_tables_mask,s->tables),cv=azrp_valid_tables(s,cr,s->trace_compute_mask,s->trace_compute_tables);
    int graphics=azrp_restore_ok(s,gr,s->trace_tables_mask,gv);
    int compute=s->compute_rs?azrp_restore_ok(s,cr,s->trace_compute_mask,cv):s->trace_bindings_known&&!s->trace_bindings_uncertain&&!s->trace_compute_mask;
    if(azt_incomplete||!graphics||!compute||!real_CreateDescriptorHeap||!real_SetComputeRootDescriptorTable||
       !real_CopyDescriptorsSimple||!real_CreateShaderResourceView||!azrp_shadow||!azrp_shadow_inc||s->num_heaps>2)goto done;
    if(!(gv&azrb_bit(t0.root))||!(gv&azrb_bit(t1.root))||(s0.mode==AZRB_TABLE&&!(gv&azrb_bit(s0.root)))){reason="unbound_final_tables";goto done;}
    AztHeap *original=NULL;int hi=-1;
    for(UINT i=0;i<s->num_heaps;++i){
        AztHeap *h=azt_heap_for_ptr(s->heaps[i]);
        if(h&&h->type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV){
            if(original){reason="multiple_heaps";goto done;}original=h;hi=(int)i;
        }
    }
    if(!original||original->inc!=azrp_shadow_inc){reason="resource_heap";goto done;}
    uintptr_t h0=0,h1=0;
    if(!azroute_descriptor_cpu(s->tables[t0.root].ptr,original->gpu,original->cpu,original->inc,original->count,t0.offset,&h0)||
       !azroute_descriptor_cpu(s->tables[t1.root].ptr,original->gpu,original->cpu,original->inc,original->count,t1.offset,&h1)){
        reason="final_descriptor_bounds";goto done;
    }
    AztDesc *scene=azt_slot(original,(SIZE_T)h0,0),*curve=azt_slot(original,(SIZE_T)h1,0);
    log_msg("VISIBLE FINAL_REQ resolve=%llu t0=%p expected=%p t0Fmt=%u t0Simple=%u curve=%p curveDim=%u curveWidth=%llu curveShadow=%d curveType=%u",resolve,scene?(void *)scene->res:NULL,(void *)native,scene?(unsigned)scene->format:0,scene?scene->simple:0,curve?(void *)curve->res:NULL,curve?(unsigned)curve->desc.Dimension:0,curve?(unsigned long long)curve->desc.Width:0,curve?curve->shadow_valid:0,curve?curve->shadow_type:0);
    if(!scene||scene->res!=native||!scene->simple||scene->format!=DXGI_FORMAT_R10G10B10A2_UNORM||
       scene->desc.Width!=1280||scene->desc.Height!=720){reason="final_t0_identity";goto done;}
    if(!curve||!curve->res||!curve->shadow_valid||!curve->shadow||curve->shadow_type!=AZRB_SRV||
       curve->desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE1D||!curve->desc.Width||curve->desc.DepthOrArraySize!=1){
        reason="native_curve_shadow";goto done;
    }
    UINT total=n0+n1,base1=t0.root==t1.root?0:n0;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2*total;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(real_CreateDescriptorHeap(g_device,&hd,&IID_ID3D12DescriptorHeap,(void **)&v->heap))||!v->heap){reason="final_heap_allocation";goto done;}
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    v->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(v->heap,&cpu);
    v->heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(v->heap,&gpu);
    for(unsigned pass=0;pass<2;++pass){
        AzrpBindings *o=pass?b:a;UINT base=pass*total;
        o->heap=v->heap;o->num_heaps=s->num_heaps;
        for(UINT i=0;i<s->num_heaps;++i)o->heaps[i]=(int)i==hi?v->heap:s->heaps[i];
        o->mask=azrb_bit(t0.root)|azrb_bit(t1.root);
        o->tables[t0.root].ptr=gpu.ptr+(UINT64)base*azrp_shadow_inc;
        o->tables[t1.root].ptr=gpu.ptr+(UINT64)(base+base1)*azrp_shadow_inc;
        if(s0.mode==AZRB_TABLE){o->mask|=azrb_bit(s0.root);o->tables[s0.root]=s->tables[s0.root];}
        D3D12_CPU_DESCRIPTOR_HANDLE dst0={cpu.ptr+(SIZE_T)(base+t0.offset)*azrp_shadow_inc};
        D3D12_CPU_DESCRIPTOR_HANDLE dst1={cpu.ptr+(SIZE_T)(base+base1+t1.offset)*azrp_shadow_inc};
        D3D12_SHADER_RESOURCE_VIEW_DESC view={0};view.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(pass){view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;}
        else{view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.MipLevels=1;view.Texture2DArray.ArraySize=3;}
        real_CreateShaderResourceView(g_device,pass?v->merged:v->layers,&view,dst0);
        real_CopyDescriptorsSimple(g_device,1,dst1,azrp_shadow_handle(curve->shadow),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    log_msg("VISIBLE FINAL_BINDINGS resolve=%llu root=%p t0Root=%u t0Offset=%u t1Root=%u t1Offset=%u curve=%p s0Mode=%u graphicsRestore=%d computeRestore=%d entries=%u",resolve,(void *)s->trace_graphics_rs,t0.root,t0.offset,t1.root,t1.offset,(void *)curve->res,s0.mode,graphics,compute,2*total);
    ok=1;
done:
    ReleaseSRWLockExclusive(&azt_lock);
    if(!ok)log_msg("VISIBLE SKIP resolve=%llu stage=final reason=%s graphicsRestore=%d computeRestore=%d",resolve,reason,graphics,compute);
    return ok;
}
static void azvis_set_tables(ID3D12GraphicsCommandList *cl,const AzrpBindings *b){
    for(UINT i=0;i<AZRB_TABLES;++i)if(b->mask&azrb_bit(i))real_SetGraphicsRootDescriptorTable(cl,i,b->tables[i]);
}
static int visible_finalize(AzvisFrame *v,ID3D12GraphicsCommandList *cl,ListState *s,ID3D12Resource *native,ID3D12Resource *target,unsigned long long resolve){
    if(!v->enabled||!v->pre_ready||!v->composite_ready||v->list!=cl||v->recording!=s->trace_recording_id||
       !azvis_texture(native,1280,720)||!azvis_texture(target,1920,1080)||s->vp_jittered||
       !azpv_adjacent_work(v->composite_work,s->trace_work_seq)){
        log_msg("VISIBLE SKIP resolve=%llu stage=final reason=chain_recording_or_nonadjacent_work",resolve);return 0;
    }
    ID3D12PipelineState *merge=NULL;
    AcquireSRWLockShared(&azvis_pso_lock);
    for(unsigned i=0;i<azvis_pso_count;++i)if(azvis_psos[i].original==s->pso){merge=azvis_psos[i].merge;break;}
    ReleaseSRWLockShared(&azvis_pso_lock);
    if(!merge){log_msg("VISIBLE SKIP resolve=%llu stage=final reason=no_exact_sdr_pso",resolve);return 0;}
    v->merged=azvis_new_texture(1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    if(!v->merged||!real_CreateDescriptorHeap||FAILED(real_CreateDescriptorHeap(g_device,&hd,&IID_ID3D12DescriptorHeap,(void **)&v->rtv))||!v->rtv){
        log_msg("VISIBLE SKIP resolve=%llu stage=final reason=merge_allocation",resolve);return 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;v->rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(v->rtv,&rtv);
    real_CreateRenderTargetView(g_device,v->merged,NULL,rtv);
    AzrpBindings a,b;
    if(!azvis_final_bindings(s,native,v,&a,&b,resolve))return 0;
    /* All gates and allocations passed BEFORE touching the native target.
       The existing 1080p viewport/scissor and native fullscreen VS are
       unchanged. Merge in the pre-SDR domain, then run the actual original
       final shader once on the new image using the original curve/sampler.
       The original final draw already ran, so skipped paths are native. */
    real_SetDescriptorHeaps(cl,a.num_heaps,a.heaps);azvis_set_tables(cl,&a);
    real_SetPipelineState(cl,merge);real_OMSetRenderTargets(cl,1,&rtv,FALSE,NULL);
    real_DrawInstanced(cl,3,1,0,0);
    hooks_transition(cl,v->merged,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    log_msg("VISIBLE HUD_COMPOSITE_RECORDED resolve=%llu size=1920x1080 policy=native_region alphaRecovered=0",resolve);
    azvis_set_tables(cl,&b);real_SetPipelineState(cl,s->pso);
    real_OMSetRenderTargets(cl,s->num_rtv,s->rtv,s->rtv_single,NULL);
    real_DrawInstanced(cl,3,1,0,0);
    real_SetDescriptorHeaps(cl,s->num_heaps,s->heaps);
    for(UINT i=0;i<AZRB_TABLES;++i){
        if(s->trace_tables_mask&azrb_bit(i))real_SetGraphicsRootDescriptorTable(cl,i,s->tables[i]);
        if(s->trace_compute_mask&azrb_bit(i))real_SetComputeRootDescriptorTable(cl,i,s->trace_compute_tables[i]);
    }
    LARGE_INTEGER qpc;QueryPerformanceCounter(&qpc);
    v->routed=1;v->target_id=(uintptr_t)target;
    log_msg("VISIBLE FINAL_RECORDED resolve=%llu list=%p recording=%llu target=%p qpc=%lld nativeFinalShader=1 presentVerified=0",resolve,(void *)cl,v->recording,(void *)target,(long long)qpc.QuadPart);
    return 1;
}
