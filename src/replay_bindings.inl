/* Opt-in CPU shadow copies. Never read a shader-visible heap as a copy
   source, never overwrite a game descriptor, and never dereference a stale
   resource pointer to recreate a view during replay. */
#include "replay_binding_policy.h"
#define AZRP_ROOTS 64
static int azrp_sr_enabled;
static ID3D12DescriptorHeap *azrp_shadow;
static D3D12_CPU_DESCRIPTOR_HANDLE azrp_shadow_base;
static UINT azrp_shadow_inc,azrp_shadow_used;
static struct {ID3D12RootSignature *rs;AzrbLayout layout;} azrp_roots[AZRP_ROOTS];
static unsigned azrp_root_count;

typedef struct {
    ID3D12DescriptorHeap *heap,*heaps[2];
    UINT num_heaps,mask;
    D3D12_GPU_DESCRIPTOR_HANDLE tables[32];
} AzrpBindings;
static D3D12_CPU_DESCRIPTOR_HANDLE azrp_shadow_handle(unsigned slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h={azrp_shadow_base.ptr+(SIZE_T)(slot-1)*azrp_shadow_inc};return h;
}
static unsigned azrp_shadow_alloc(unsigned slot) {
    if(slot)return slot;
    return azrp_shadow_used<AZT_DESCS?++azrp_shadow_used:0;
}
static unsigned azrp_copy_shadow(unsigned dst,unsigned src) {
    if(!azrp_shadow||!src||!real_CopyDescriptorsSimple)return 0;
    dst=azrp_shadow_alloc(dst);
    if(dst&&dst!=src)real_CopyDescriptorsSimple(g_device,1,azrp_shadow_handle(dst),azrp_shadow_handle(src),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return dst;
}
static void azrp_shadow_init(void) {
    char b[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_SR_REPLAY",b,sizeof(b));
    if(n!=1||b[0]!='1')return;
    D3D12_FEATURE_DATA_D3D12_OPTIONS opts={0};
    if(FAILED(ID3D12Device_CheckFeatureSupport(g_device,D3D12_FEATURE_D3D12_OPTIONS,&opts,sizeof(opts)))||
       opts.ResourceBindingTier!=D3D12_RESOURCE_BINDING_TIER_3){
        log_msg("POSTFX SR_REPLAY unavailable: resource binding tier 3 required");return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=AZT_DESCS;
    if(FAILED(ID3D12Device_CreateDescriptorHeap(g_device,&hd,&IID_ID3D12DescriptorHeap,(void **)&azrp_shadow))||!azrp_shadow){
        log_msg("POSTFX SR_REPLAY unavailable: CPU descriptor shadow allocation");return;
    }
    azrp_shadow->lpVtbl->GetCPUDescriptorHandleForHeapStart(azrp_shadow,&azrp_shadow_base);
    azrp_shadow_inc=ID3D12Device_GetDescriptorHandleIncrementSize(g_device,hd.Type);
    if(!azrp_shadow_inc)return;
    azt_shadow_copy=azrp_copy_shadow;azrp_sr_enabled=1;
    log_msg("POSTFX SR_REPLAY CONFIG private descriptor mirror, exact PS t0 substitution, offscreen only");
}
static AztDesc *azrp_shadow_begin(D3D12_CPU_DESCRIPTOR_HANDLE h) {
    AztDesc *d=azt_slot(azt_heap_for_cpu(h.ptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),h.ptr,0);
    if(!d)return NULL;
    d->shadow=azrp_shadow_alloc(d->shadow);d->shadow_valid=0;return d->shadow?d:NULL;
}
static void azrp_shadow_srv(ID3D12Device *dev,ID3D12Resource *res,const D3D12_SHADER_RESOURCE_VIEW_DESC *view,D3D12_CPU_DESCRIPTOR_HANDLE h){
    if(!azrp_sr_enabled)return;
    AcquireSRWLockExclusive(&azt_lock);
    AztDesc *d=azrp_shadow_begin(h);
    if(d){real_CreateShaderResourceView(dev,res,view,azrp_shadow_handle(d->shadow));d->shadow_type=AZRB_SRV;d->shadow_valid=1;}
    ReleaseSRWLockExclusive(&azt_lock);
}
static void azrp_shadow_cbv(ID3D12Device *dev,const D3D12_CONSTANT_BUFFER_VIEW_DESC *view,D3D12_CPU_DESCRIPTOR_HANDLE h){
    if(!azrp_sr_enabled)return;
    AcquireSRWLockExclusive(&azt_lock);
    AztDesc *d=azrp_shadow_begin(h);
    if(d){real_CreateConstantBufferView(dev,view,azrp_shadow_handle(d->shadow));d->shadow_type=AZRB_CBV;d->shadow_valid=1;}
    ReleaseSRWLockExclusive(&azt_lock);
}
static void azrp_shadow_uav(ID3D12Device *dev,ID3D12Resource *res,ID3D12Resource *counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC *view,D3D12_CPU_DESCRIPTOR_HANDLE h){
    if(!azrp_sr_enabled)return;
    AcquireSRWLockExclusive(&azt_lock);
    AztDesc *d=azrp_shadow_begin(h);
    if(d){real_CreateUnorderedAccessView(dev,res,counter,view,azrp_shadow_handle(d->shadow));d->shadow_type=AZRB_UAV;d->shadow_valid=1;}
    ReleaseSRWLockExclusive(&azt_lock);
}
static void azrp_range(AzrbLayout *l,unsigned root,unsigned vis,unsigned type,unsigned reg,unsigned space,UINT count,UINT offset,UINT *next){
    UINT start=offset==D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND?*next:offset;
    azrb_range(l,root,vis,type,reg,space,count,start);
    *next=count==UINT_MAX||start==UINT_MAX||count>UINT_MAX-start?UINT_MAX:start+count;
}
static void azrp_note_root(ID3D12RootSignature *rs,const void *blob,SIZE_T bytes){
    if(!azrp_sr_enabled||!rs||!blob||!bytes)return;
    typedef HRESULT(WINAPI *Deserializer)(LPCVOID,SIZE_T,REFIID,void **);
    HMODULE dll=GetModuleHandleW(L"d3d12.dll");
    Deserializer fn=dll?(Deserializer)(void *)GetProcAddress(dll,"D3D12CreateVersionedRootSignatureDeserializer"):NULL;
    ID3D12VersionedRootSignatureDeserializer *obj=NULL;
    if(!fn||FAILED(fn(blob,bytes,&IID_ID3D12VersionedRootSignatureDeserializer,(void **)&obj))||!obj)return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc=obj->lpVtbl->GetUnconvertedRootSignatureDesc(obj);
    AzrbLayout l;azrb_init(&l);
#define AZRP_PARSE_ROOT(rd,Param,Range) do { \
    const UINT count=(rd)->NumParameters; \
    if(count>32||((UINT)(rd)->Flags&0xc00u))l.valid=0; \
    for(UINT i=0;i<count&&i<32;++i){ \
        const Param *p=&(rd)->pParameters[i]; \
        if(p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){ \
            UINT next=0; \
            for(UINT j=0;j<p->DescriptorTable.NumDescriptorRanges;++j){ \
                const Range *x=&p->DescriptorTable.pDescriptorRanges[j]; \
                azrp_range(&l,i,p->ShaderVisibility,x->RangeType,x->BaseShaderRegister,x->RegisterSpace,x->NumDescriptors,x->OffsetInDescriptorsFromTableStart,&next); \
            } \
        }else if(p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){ \
            azrb_direct(&l,i,p->ShaderVisibility,AZRB_CBV,p->Constants.ShaderRegister,p->Constants.RegisterSpace,AZRB_DIRECT); \
        }else if(p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_CBV||p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_SRV||p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_UAV){ \
            unsigned type=p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_CBV?AZRB_CBV:p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_SRV?AZRB_SRV:AZRB_UAV; \
            azrb_direct(&l,i,p->ShaderVisibility,type,p->Descriptor.ShaderRegister,p->Descriptor.RegisterSpace,AZRB_DIRECT); \
        } \
    } \
    for(UINT i=0;i<(rd)->NumStaticSamplers;++i){ \
        const D3D12_STATIC_SAMPLER_DESC *x=&(rd)->pStaticSamplers[i]; \
        azrb_direct(&l,0,x->ShaderVisibility,AZRB_SAMPLER,x->ShaderRegister,x->RegisterSpace,AZRB_STATIC); \
    } \
} while(0)
    if(desc&&desc->Version==D3D_ROOT_SIGNATURE_VERSION_1_0){
        const D3D12_ROOT_SIGNATURE_DESC *rd=&desc->Desc_1_0;
        AZRP_PARSE_ROOT(rd,D3D12_ROOT_PARAMETER,D3D12_DESCRIPTOR_RANGE);
    }else if(desc&&desc->Version==D3D_ROOT_SIGNATURE_VERSION_1_1){
        const D3D12_ROOT_SIGNATURE_DESC1 *rd=&desc->Desc_1_1;
        AZRP_PARSE_ROOT(rd,D3D12_ROOT_PARAMETER1,D3D12_DESCRIPTOR_RANGE1);
    }else l.valid=0;
#undef AZRP_PARSE_ROOT
    obj->lpVtbl->Release(obj);
    uint32_t base[AZRB_TABLES],total,mask;
    if(!azrb_finish(&l,base,&total,&mask))return;
    AcquireSRWLockExclusive(&azt_lock);
    if(azrp_root_count<AZRP_ROOTS){
        ID3D12RootSignature_AddRef(rs);
        unsigned i=azrp_root_count++;azrp_roots[i].rs=rs;azrp_roots[i].layout=l;
        log_msg("POSTFX SR_LAYOUT root=%p entries=%u psT0Root=%u psT0Offset=%u vsT0Root=%u vsT0Offset=%u",(void *)rs,total,l.req[0].root,l.req[0].offset,l.req[16].root,l.req[16].offset);
    }
    ReleaseSRWLockExclusive(&azt_lock);
}
/* Same original root signature. Only descriptor tables change; root CBV
   and root constants remain in their original registers. Rebind sampler
   tables from the unchanged sampler heap. Clone the exact declared
   shader registers, with Tier 3 unreferenced slots never accessed. */
static int azrp_prepare_bindings(ListState *s,ID3D12Resource *scene,ID3D12Resource *snapshot,AzrpBindings *out,unsigned long long resolve){
    if(!azrp_sr_enabled||!s||!scene||!snapshot||!real_SetComputeRootDescriptorTable||
       !s->trace_graphics_rs||!s->num_heaps||s->num_heaps>2)return 0;
    memset(out,0,sizeof(*out));
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors=AZRB_HEAP_LIMIT;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(ID3D12Device_CreateDescriptorHeap(g_device,&hd,&IID_ID3D12DescriptorHeap,(void **)&out->heap))||!out->heap)return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    out->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(out->heap,&cpu);
    out->heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(out->heap,&gpu);
    int ok=0;const char *reason="root_layout";
    AcquireSRWLockExclusive(&azt_lock);
    const AzrbLayout *l=NULL;
    for(unsigned i=0;i<azrp_root_count;++i)if(azrp_roots[i].rs==s->trace_graphics_rs){l=&azrp_roots[i].layout;break;}
    uint32_t base[AZRB_TABLES],total,mask;
    if(!l||!azrb_finish(l,base,&total,&mask)||azt_incomplete)goto done;
    AztHeap *orig=NULL;
    for(UINT i=0;i<s->num_heaps;++i){
        AztHeap *h=azt_heap_for_ptr(s->heaps[i]);
        if(h&&h->type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV){
            if(orig){reason="multiple_resource_heaps";goto done;}
            orig=h;out->heaps[i]=out->heap;
        }else out->heaps[i]=s->heaps[i];
    }
    out->num_heaps=s->num_heaps;
    if(!orig||!orig->gpu){reason="resource_heap";goto done;}
    for(unsigned i=0;i<AZRB_REQS;++i){
        const AzrbReq *r=&l->req[i];
        if(r->mode!=AZRB_TABLE)continue;
        if(!(s->trace_tables_mask&(1u<<r->root))){reason="unbound_table";goto done;}
        if(l->kind[r->root]==2){out->tables[r->root]=s->tables[r->root];out->mask|=1u<<r->root;continue;}
        uintptr_t handle=0;
        if(!azroute_descriptor_cpu(s->tables[r->root].ptr,orig->gpu,orig->cpu,orig->inc,orig->count,r->offset,&handle)){
            reason="descriptor_bounds";goto done;
        }
        AztDesc *d=azt_slot(orig,(SIZE_T)handle,0);
        if(!d||!d->shadow_valid||d->shadow_type!=r->type){reason="missing_descriptor_shadow";goto done;}
        D3D12_CPU_DESCRIPTOR_HANDLE dst={cpu.ptr+(SIZE_T)(base[r->root]+r->offset)*azrp_shadow_inc};
        if(i==0){
            if(d->res!=scene||!d->simple||d->format!=DXGI_FORMAT_R11G11B10_FLOAT){reason="scene_t0_identity";goto done;}
            D3D12_SHADER_RESOURCE_VIEW_DESC view={0};view.Format=DXGI_FORMAT_R11G11B10_FLOAT;
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
            real_CreateShaderResourceView(g_device,snapshot,&view,dst);
        }else real_CopyDescriptorsSimple(g_device,1,dst,azrp_shadow_handle(d->shadow),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        out->tables[r->root].ptr=gpu.ptr+(UINT64)base[r->root]*azrp_shadow_inc;
        out->mask|=1u<<r->root;
    }
    ok=1;
    log_msg("POSTFX SR_BINDINGS resolve=%llu entries=%u root=%p tables=0x%08x shadowUsed=%u",resolve,total,(void *)s->trace_graphics_rs,out->mask,azrp_shadow_used);
done:
    ReleaseSRWLockExclusive(&azt_lock);
    if(!ok){ID3D12DescriptorHeap_Release(out->heap);memset(out,0,sizeof(*out));log_msg("POSTFX SR_REPLAY SKIP resolve=%llu reason=%s",resolve,reason);}
    return ok;
}
