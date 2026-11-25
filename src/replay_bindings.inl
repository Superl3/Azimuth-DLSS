/* Opt-in CPU shadow copies. Never read a shader-visible heap as a copy
   source, never overwrite a game descriptor, and never dereference a stale
   resource pointer to recreate a view during replay. */
#include "replay_binding_policy.h"
#define AZRP_ROOTS 64
#define AZRP_SCHEMA_ROWS 128
/* Metadata belongs to the actual COM object, not a reused pointer. */
static const GUID azrp_layout_key={0x5c5901a7,0xf1b5,0x4bce,{0x91,0x15,0x37,0x86,0x7c,0xa3,0x92,0xd4}};
typedef AzrbSchemaRow AzrpSchemaRow;
typedef struct {AzrbLayout layout;UINT version,flags,parameters,rows,overflow;AzrpSchemaRow row[AZRP_SCHEMA_ROWS];} AzrpRootEvidence;
static void azrp_schema_add(AzrpRootEvidence *e,UINT root,UINT vis,UINT type,UINT reg,UINT space,UINT count,UINT offset,UINT kind){
    if(e->rows>=AZRP_SCHEMA_ROWS){e->overflow=1;return;}
    e->row[e->rows++]=(AzrpSchemaRow){root,vis,type,reg,space,count,offset,kind};
}
static int azrp_sr_enabled;
static ID3D12DescriptorHeap *azrp_shadow;
static D3D12_CPU_DESCRIPTOR_HANDLE azrp_shadow_base;
static UINT azrp_shadow_inc,azrp_shadow_used;
static struct {ID3D12RootSignature *rs;AzrbLayout layout;} azrp_roots[AZRP_ROOTS];
static unsigned azrp_root_count;

typedef struct {
    ID3D12DescriptorHeap *heap,*heaps[2];
    UINT num_heaps,path;
    uint64_t mask;
    D3D12_GPU_DESCRIPTOR_HANDLE tables[AZRB_TABLES];
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
/* Registration rejects invalid layouts before inserting them. Log the
   actual rejection with the root identity so a later Resolve's missing
   entry can be correlated without registering an unsafe layout. */
static void azrp_log_layout_failure(ID3D12RootSignature *rs,const AzrbLayout *l,AzrbFinish status,unsigned req,unsigned long long resolve,const char *phase){
    unsigned missing=0;
    for(unsigned i=0;i<AZRB_REQS;++i)if(!l->req[i].mode)missing|=1u<<i;
    const AzrbReq *bad=req<AZRB_REQS?&l->req[req]:NULL;
    log_msg("POSTFX SR_LAYOUT_REJECT phase=%s resolve=%llu root=%p detail=%s reqIndex=%u stage=%u type=%u reg=%u mode=%u tableRoot=%u offset=%u valid=%d missingMask=0x%05x psT0Mode=%u psT0Root=%u psT0Offset=%u vsT0Mode=%u vsT0Root=%u vsT0Offset=%u",
        phase,resolve,(void *)rs,azrb_finish_name(status),req,bad?bad->stage:UINT_MAX,bad?bad->type:UINT_MAX,bad?bad->reg:UINT_MAX,bad?bad->mode:UINT_MAX,bad?bad->root:UINT_MAX,bad?bad->offset:UINT_MAX,
        l->valid,missing,l->req[0].mode,l->req[0].root,l->req[0].offset,l->req[16].mode,l->req[16].root,l->req[16].offset);
}
static void azrp_note_root(ID3D12RootSignature *rs,const void *blob,SIZE_T bytes){
    if(!azrp_sr_enabled||!rs)return;
    if(!blob||!bytes){log_msg("POSTFX SR_LAYOUT_REJECT phase=registration resolve=0 root=%p detail=missing_serialized_root",(void *)rs);return;}
    typedef HRESULT(WINAPI *Deserializer)(LPCVOID,SIZE_T,REFIID,void **);
    HMODULE dll=GetModuleHandleW(L"d3d12.dll");
    Deserializer fn=dll?(Deserializer)(void *)GetProcAddress(dll,"D3D12CreateVersionedRootSignatureDeserializer"):NULL;
    ID3D12VersionedRootSignatureDeserializer *obj=NULL;
    if(!fn){log_msg("POSTFX SR_LAYOUT_REJECT phase=registration resolve=0 root=%p detail=deserializer_unavailable",(void *)rs);return;}
    HRESULT hr=fn(blob,bytes,&IID_ID3D12VersionedRootSignatureDeserializer,(void **)&obj);
    if(FAILED(hr)||!obj){log_msg("POSTFX SR_LAYOUT_REJECT phase=registration resolve=0 root=%p detail=deserialize_failed hr=%08lx",(void *)rs,(unsigned long)hr);return;}
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc=obj->lpVtbl->GetUnconvertedRootSignatureDesc(obj);
    AzrpRootEvidence evidence;memset(&evidence,0,sizeof(evidence));
    AzrbLayout l;azrb_init(&l);
    evidence.version=desc?(UINT)desc->Version:0;
#define AZRP_PARSE_ROOT(rd,Param,Range) do { \
    const UINT count=(rd)->NumParameters; \
    evidence.parameters=count;evidence.flags=(UINT)(rd)->Flags; \
    if(count>AZRB_TABLES){l.valid=0;evidence.overflow=1;} \
    if((UINT)(rd)->Flags&0xc00u)l.valid=0; \
    for(UINT i=0;i<count&&i<AZRB_TABLES;++i){ \
        const Param *p=&(rd)->pParameters[i]; \
        if(p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){ \
            UINT next=0; \
            for(UINT j=0;j<p->DescriptorTable.NumDescriptorRanges;++j){ \
                const Range *x=&p->DescriptorTable.pDescriptorRanges[j]; \
                azrp_schema_add(&evidence,i,p->ShaderVisibility,x->RangeType,x->BaseShaderRegister,x->RegisterSpace,x->NumDescriptors,x->OffsetInDescriptorsFromTableStart==D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND?next:x->OffsetInDescriptorsFromTableStart,AZRB_TABLE); \
                azrp_range(&l,i,p->ShaderVisibility,x->RangeType,x->BaseShaderRegister,x->RegisterSpace,x->NumDescriptors,x->OffsetInDescriptorsFromTableStart,&next); \
            } \
        }else if(p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){ \
            azrp_schema_add(&evidence,i,p->ShaderVisibility,AZRB_CBV,p->Constants.ShaderRegister,p->Constants.RegisterSpace,p->Constants.Num32BitValues,0,AZRB_CONSTANT); \
            azrb_direct(&l,i,p->ShaderVisibility,AZRB_CBV,p->Constants.ShaderRegister,p->Constants.RegisterSpace,AZRB_CONSTANT); \
        }else if(p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_CBV||p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_SRV||p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_UAV){ \
            unsigned type=p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_CBV?AZRB_CBV:p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_SRV?AZRB_SRV:AZRB_UAV; \
            azrp_schema_add(&evidence,i,p->ShaderVisibility,type,p->Descriptor.ShaderRegister,p->Descriptor.RegisterSpace,1,0,AZRB_DIRECT); \
            azrb_direct(&l,i,p->ShaderVisibility,type,p->Descriptor.ShaderRegister,p->Descriptor.RegisterSpace,AZRB_DIRECT); \
        } \
    } \
    for(UINT i=0;i<(rd)->NumStaticSamplers;++i){ \
        const D3D12_STATIC_SAMPLER_DESC *x=&(rd)->pStaticSamplers[i]; \
        azrp_schema_add(&evidence,i,x->ShaderVisibility,AZRB_SAMPLER,x->ShaderRegister,x->RegisterSpace,1,0,AZRB_STATIC); \
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
    evidence.layout=l;
    HRESULT stored=rs->lpVtbl->SetPrivateData(rs,&azrp_layout_key,(UINT)sizeof(evidence),&evidence);
    if(FAILED(stored))log_msg("POSTFX SR_LAYOUT_STORE_FAILED root=%p hr=%08lx",(void *)rs,(unsigned long)stored);
    uint32_t base[AZRB_TABLES],total;uint64_t mask;
    unsigned req;
    AzrbFinish status=azrb_finish_detail(&l,base,&total,&mask,&req);
    if(status!=AZRB_FINISH_OK){azrp_log_layout_failure(rs,&l,status,req,0,"registration");return;}
    AcquireSRWLockExclusive(&azt_lock);
    for(unsigned i=0;i<azrp_root_count;++i)if(azrp_roots[i].rs==rs){ReleaseSRWLockExclusive(&azt_lock);return;}
    if(azrp_root_count<AZRP_ROOTS){
        ID3D12RootSignature_AddRef(rs);
        unsigned i=azrp_root_count++;azrp_roots[i].rs=rs;azrp_roots[i].layout=l;
        log_msg("POSTFX SR_LAYOUT root=%p entries=%u psT0Root=%u psT0Offset=%u vsT0Root=%u vsT0Offset=%u",(void *)rs,total,l.req[0].root,l.req[0].offset,l.req[16].root,l.req[16].offset);
    }else log_msg("POSTFX SR_LAYOUT_REJECT phase=registration resolve=0 root=%p detail=registry_full registered=%u capacity=%u",(void *)rs,azrp_root_count,(unsigned)AZRP_ROOTS);
    ReleaseSRWLockExclusive(&azt_lock);
}
/* Validate original table handles against the actual currently-bound
   heap of the declared kind. The native sampler heap stays unchanged.
   A table never assigned in a complete epoch is undefined, not lost;
   after replay, restore every actually assigned table and no others. */
static uint64_t azrp_valid_tables(const ListState *s,const AztRoot *root,uint64_t bound,const D3D12_GPU_DESCRIPTOR_HANDLE *handles){
    uint64_t valid=0;
    if(!root||!root->table_layout_known)return 0;
    for(unsigned i=0;i<AZRB_TABLES;++i){
        uint64_t bit=azrb_bit(i);
        if(!(bound&bit)||!(root->tables_all&bit))continue;
        UINT type=(root->sampler_tables&bit)?D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        unsigned matches=0;
        for(UINT j=0;j<s->num_heaps&&j<2;++j){
            AztHeap *heap=azt_heap_for_ptr(s->heaps[j]);uintptr_t cpu;
            if(heap&&heap->type==type&&azroute_descriptor_cpu(handles[i].ptr,heap->gpu,heap->cpu,heap->inc,heap->count,0,&cpu))++matches;
        }
        if(matches==1)valid|=bit;
    }
    return valid;
}
static int azrp_restore_ok(const ListState *s,const AztRoot *root,uint64_t bound,uint64_t valid){
    return root&&root->table_layout_known&&azrb_restore_mask_ok(s->trace_bindings_known,s->trace_bindings_uncertain,root->tables_all,bound,valid);
}
/* Plan A: legacy compact table packing. B: same-capacity physical heap
   rebase. C: extended packing. All share exact shader requirements,
   descriptor identity, producer and complete restoration gates. No path
   switches the game's root signature or writes a game descriptor.
   Preflight logs independent failures even when execution is blocked. */
static int azrp_prepare_bindings(ListState *s,ID3D12Resource *scene,ID3D12Resource *snapshot,AzrpBindings *out,unsigned long long resolve,int producer_ready,int draw_ready){
    memset(out,0,sizeof(*out));
    if(!azrp_sr_enabled||!s)return 0;
    AzrpRootEvidence evidence;memset(&evidence,0,sizeof(evidence));
    int object_known=0;
    if(s->trace_graphics_rs){
        UINT size=(UINT)sizeof(evidence);
        HRESULT hr=s->trace_graphics_rs->lpVtbl->GetPrivateData(s->trace_graphics_rs,&azrp_layout_key,&size,&evidence);
        object_known=SUCCEEDED(hr)&&size==sizeof(evidence)&&evidence.rows<=AZRP_SCHEMA_ROWS;
    }
    AcquireSRWLockExclusive(&azt_lock);
    const AzrbLayout *cached=NULL,*l=NULL;
    for(unsigned i=0;i<azrp_root_count;++i)if(azrp_roots[i].rs==s->trace_graphics_rs){cached=&azrp_roots[i].layout;break;}
    if(object_known)l=&evidence.layout;else l=cached;
    AztRoot *gr=azt_root_slot(s->trace_graphics_rs,0);
    AztRoot *cr=s->compute_rs?azt_root_slot(s->compute_rs,0):NULL;
    uint64_t gv=azrp_valid_tables(s,gr,s->trace_tables_mask,s->tables);
    uint64_t cv=azrp_valid_tables(s,cr,s->trace_compute_mask,s->trace_compute_tables);
    int graphics_ok=azrp_restore_ok(s,gr,s->trace_tables_mask,gv);
    int compute_ok=s->compute_rs?azrp_restore_ok(s,cr,s->trace_compute_mask,cv):s->trace_bindings_known&&!s->trace_bindings_uncertain&&!s->trace_compute_mask;
    log_msg("POSTFX SR_RESTORE resolve=%llu epochKnown=%d uncertain=%d graphicsDeclared=0x%016llx graphicsBound=0x%016llx graphicsValid=0x%016llx graphicsUnbound=0x%016llx computeDeclared=0x%016llx computeBound=0x%016llx computeValid=0x%016llx computeUnbound=0x%016llx",
        resolve,s->trace_bindings_known,s->trace_bindings_uncertain,(unsigned long long)(gr?gr->tables_all:0),(unsigned long long)s->trace_tables_mask,(unsigned long long)gv,
        (unsigned long long)(gr?gr->tables_all&~s->trace_tables_mask:0),(unsigned long long)(cr?cr->tables_all:0),(unsigned long long)s->trace_compute_mask,(unsigned long long)cv,
        (unsigned long long)(cr?cr->tables_all&~s->trace_compute_mask:0));
    int bindings_ok=real_SetComputeRootDescriptorTable&&real_CopyDescriptorsSimple&&real_CreateShaderResourceView&&real_CreateDescriptorHeap&&
        azrp_shadow&&azrp_shadow_inc&&s->trace_graphics_rs&&s->num_heaps&&s->num_heaps<=2&&!s->trace_bindings_uncertain;
    AztHeap *orig=NULL;int resource_heap_index=-1,duplicate=0;
    for(UINT i=0;i<s->num_heaps&&i<2;++i){
        AztHeap *heap=azt_heap_for_ptr(s->heaps[i]);
        if(heap&&heap->type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV){
            if(orig)duplicate=1;
            orig=heap;resource_heap_index=(int)i;
        }
    }
    int heap_ok=orig&&orig->gpu&&orig->inc==azrp_shadow_inc&&!duplicate;
    log_msg("POSTFX SR_PREFLIGHT resolve=%llu root=%p registered=%d objectLayout=%d registryCount=%u registryCapacity=%u traceIncomplete=%d descriptorUsed=%u graphicsRestore=%d graphicsMask=0x%016llx graphicsRequired=0x%016llx computeRestore=%d computeMask=0x%016llx computeRequired=0x%016llx bindings=%d heap=%d heapCount=%u producerReady=%d snapshot=%d drawReady=%d",
        resolve,(void *)s->trace_graphics_rs,cached!=NULL,object_known,azrp_root_count,(unsigned)AZRP_ROOTS,azt_incomplete,azt_desc_used,
        graphics_ok,(unsigned long long)s->trace_tables_mask,(unsigned long long)(gr?gr->tables_all:0),compute_ok,(unsigned long long)s->trace_compute_mask,(unsigned long long)(cr?cr->tables_all:0),
        bindings_ok,heap_ok,orig?orig->count:0,producer_ready,snapshot!=NULL,draw_ready);
    if(object_known){
        log_msg("POSTFX SR_SCHEMA resolve=%llu root=%p version=%u flags=0x%x parameters=%u rows=%u truncated=%u",
            resolve,(void *)s->trace_graphics_rs,evidence.version,evidence.flags,evidence.parameters,evidence.rows,evidence.overflow);
        for(UINT i=0;i<evidence.rows;++i){
            const AzrpSchemaRow *x=&evidence.row[i];
            log_msg("POSTFX SR_SCHEMA_ROW resolve=%llu root=%p index=%u vis=%u type=%u reg=%u space=%u count=%u offset=%u kind=%u",
                resolve,(void *)s->trace_graphics_rs,x->root,x->visibility,x->type,x->reg,x->space,x->count,x->offset,x->kind);
        }
    }
    if(!l){
        log_msg("POSTFX SR_REPLAY SKIP resolve=%llu reason=root_layout_unregistered root=%p detail=no_cache_or_object_metadata",resolve,(void *)s->trace_graphics_rs);
        ReleaseSRWLockExclusive(&azt_lock);return 0;
    }
    uint32_t base_a[AZRB_TABLES]={0},base_b[AZRB_TABLES],base_c[AZRB_TABLES]={0},total_a=0,total_c=0;
    uint64_t mask_a=0,mask_b=0,mask_c=0;
    unsigned bad_a,bad_b,bad_c;
    AzrbFinish a=azrb_finish_detail(l,base_a,&total_a,&mask_a,&bad_a);
    AzrbFinish c=azrb_finish_limit(l,base_c,&total_c,&mask_c,&bad_c,AZRB_FALLBACK_LIMIT);
    if(a!=AZRB_FINISH_OK)azrp_log_layout_failure(s->trace_graphics_rs,l,a,bad_a,resolve,"replay");
    AzrbLayout physical_layout;unsigned project_bad=UINT32_MAX;int projected=0;
    /* If the full-layout parser rejected the root, independently resolve
       the shader requirements against the saved original ranges. Only the
       same-capacity physical plan may use this projected layout. */
    uint32_t unused_base[AZRB_TABLES],unused_total=0;uint64_t unused_mask=0;unsigned unused_req;
    AzrbFinish semantic=azrb_finish_limit(l,unused_base,&unused_total,&unused_mask,&unused_req,0);
    if(semantic!=AZRB_FINISH_OK&&object_known){
        projected=azrb_project_schema(&physical_layout,evidence.row,evidence.rows,evidence.parameters,evidence.flags,evidence.overflow,&project_bad);
        log_msg("POSTFX SR_SCHEMA_PROJECT resolve=%llu original=%s projected=%d req=%u",resolve,azrb_finish_name(semantic),projected,project_bad);
        if(projected)l=&physical_layout;
    }
    for(unsigned i=0;i<AZRB_TABLES;++i){
        base_b[i]=UINT32_MAX;
        if(heap_ok&&(s->trace_tables_mask&azrb_bit(i))){
            uintptr_t handle=0;
            if(azroute_descriptor_cpu(s->tables[i].ptr,orig->gpu,orig->cpu,orig->inc,orig->count,0,&handle))
                base_b[i]=(uint32_t)((handle-orig->cpu)/orig->inc);
        }
    }
    AzrbRebase b=azrb_rebase(l,base_b,heap_ok?orig->count:0,&mask_b,&bad_b);
    AztDesc *source[AZRB_REQS]={0};
    unsigned errors=0,strict_errors=0;
    for(unsigned i=0;i<AZRB_REQS;++i){
        const AzrbReq *q=&l->req[i];
        const char *reason="native_root_arg_preserved";
        int ok=0;uint32_t physical=UINT32_MAX;
        if(!q->mode)reason="required_binding_missing";
        else if(q->mode==AZRB_STATIC){ok=q->type==AZRB_SAMPLER;reason=ok?"static_sampler":"invalid_static";}
        else if(q->mode==AZRB_DIRECT){
            ok=q->root<AZRB_TABLES&&q->type==AZRB_CBV&&(s->trace_graphics_cbv_mask&azrb_bit(q->root));
            reason=ok?"native_root_cbv_observed":q->type==AZRB_CBV?"root_cbv_unobserved":"unsupported_direct";
        }else if(q->mode==AZRB_CONSTANT){
            /* Exact PS/VS pair only reads cb13[0].x. No root signature
               change or constant write occurs during replay. */
            ok=q->root<AZRB_TABLES&&q->type==AZRB_CBV&&q->reg==13&&(s->trace_graphics_constant_words[q->root]&1);
            reason=ok?"native_root_constant_observed":"root_constant_unobserved_or_unsupported";
        }
        else if(q->mode!=AZRB_TABLE||q->root>=AZRB_TABLES||q->offset>=l->span[q->root]||!l->kind[q->root])reason="invalid_table";
        else if(!(s->trace_tables_mask&azrb_bit(q->root))||!s->tables[q->root].ptr)reason="unbound_table";
        else if(l->kind[q->root]==2){
            ok=q->type==AZRB_SAMPLER&&(gv&azrb_bit(q->root));reason=ok?"native_sampler_table":"invalid_sampler_table";
        }
        else if(!heap_ok||base_b[q->root]>=orig->count||q->offset>=orig->count-base_b[q->root])reason="descriptor_bounds";
        else{
            physical=base_b[q->root]+q->offset;
            AztDesc *d=azt_slot(orig,orig->cpu+(SIZE_T)physical*orig->inc,0);
            source[i]=d;
            if(!d)reason="missing_descriptor_metadata";
            else if(i==0){
                /* PS t0 is overwritten only in a new heap. A shadow of
                   the old view is not needed by B/C, but live exact
                   source identity, simple view, and format still are. */
                ok=d->res==scene&&d->simple&&d->format==DXGI_FORMAT_R11G11B10_FLOAT&&
                    d->desc.Width==1280&&d->desc.Height==720&&(!d->shadow_valid||d->shadow_type==AZRB_SRV);
                if(!ok)reason="scene_t0_identity";
                else if(!d->shadow_valid){reason="scene_verified_recreate";strict_errors|=1u<<i;}
                else reason="scene_verified";
            }else{
                ok=d->shadow_valid&&d->shadow&&d->shadow_type==q->type;
                reason=ok?"shadow_ok":!d->shadow_valid||!d->shadow?"missing_descriptor_shadow":"shadow_type_mismatch";
            }
        }
        if(!ok)errors|=1u<<i;
        if(q->mode==AZRB_DIRECT||q->mode==AZRB_CONSTANT)log_msg("POSTFX SR_ROOT_ARG resolve=%llu req=%u index=%u mode=%u cbv=0x%016llx words=0x%016llx observed=%d",resolve,i,q->root,q->mode,(unsigned long long)(q->root<AZRB_TABLES&&q->mode==AZRB_DIRECT?s->trace_graphics_cbv[q->root]:0),(unsigned long long)(q->root<AZRB_TABLES?s->trace_graphics_constant_words[q->root]:0),ok);
        log_msg("POSTFX SR_REQ resolve=%llu root=%p req=%u stage=%u type=%u reg=%u mode=%u tableRoot=%u offset=%u span=%u physical=%u shadow=%u shadowType=%u resource=%p ok=%d reason=%s",
            resolve,(void *)s->trace_graphics_rs,i,q->stage,q->type,q->reg,q->mode,q->root,q->offset,q->root<AZRB_TABLES?l->span[q->root]:0,
            physical,source[i]?source[i]->shadow:0,source[i]?source[i]->shadow_type:UINT_MAX,source[i]?(void *)source[i]->res:NULL,ok,reason);
    }
    int common=bindings_ok&&graphics_ok&&compute_ok&&heap_ok&&!azt_incomplete&&!errors&&producer_ready&&snapshot&&scene&&draw_ready;
    int available[3]={!projected&&a==AZRB_FINISH_OK&&!strict_errors,b==AZRB_REBASE_OK,!projected&&c==AZRB_FINISH_OK};
    log_msg("POSTFX SR_PLAN resolve=%llu path=A_compact layout=%s req=%u entries=%u descriptorErrors=0x%05x common=%d candidate=%d",
        resolve,azrb_finish_name(a),bad_a,total_a,errors|strict_errors,common,available[0]);
    log_msg("POSTFX SR_PLAN resolve=%llu path=B_physical layout=%s req=%u entries=%u descriptorErrors=0x%05x common=%d candidate=%d",
        resolve,azrb_rebase_name(b),bad_b,orig?orig->count:0,errors,common,available[1]);
    log_msg("POSTFX SR_PLAN resolve=%llu path=C_extended layout=%s req=%u entries=%u descriptorErrors=0x%05x common=%d candidate=%d",
        resolve,azrb_finish_name(c),bad_c,total_c,errors,common,available[2]);
    if(!common){
        const char *reason=azt_incomplete?"trace_incomplete":!draw_ready?"original_draw_gate":!producer_ready||!snapshot?"producer_not_ready":
            !graphics_ok?"graphics_restore_unproven":!compute_ok?"compute_restore_unproven":!bindings_ok?"unknown_original_bindings":!heap_ok?"resource_heap":"required_binding_unavailable";
        log_msg("POSTFX SR_REPLAY SKIP resolve=%llu reason=%s root=%p descriptorErrors=0x%05x",resolve,reason,(void *)s->trace_graphics_rs,errors);
        ReleaseSRWLockExclusive(&azt_lock);return 0;
    }
    /* Allocation is the only remaining fallible preparation step. Try the
       next independent plan before recording ANY GPU work. */
    for(unsigned path=0;path<3;++path){
        if(!available[path])continue;
        const uint32_t *base=path==0?base_a:path==1?base_b:base_c;
        UINT total=path==0?total_a:path==1?orig->count:total_c;
        if(!total||total>AZRB_FALLBACK_LIMIT)continue;
        D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors=total;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap *private_heap=NULL;
        /* The device hook acquires azt_lock. This private heap must bypass
           it while preflight holds the lock, or successful plans deadlock. */
        HRESULT hr=real_CreateDescriptorHeap(g_device,&hd,&IID_ID3D12DescriptorHeap,(void **)&private_heap);
        if(FAILED(hr)||!private_heap){
            if(private_heap)ID3D12DescriptorHeap_Release(private_heap);
            log_msg("POSTFX SR_PLAN_REJECT resolve=%llu path=%u reason=heap_allocation hr=%08lx",resolve,path+1,(unsigned long)hr);continue;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        private_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(private_heap,&cpu);
        private_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(private_heap,&gpu);
        if(!cpu.ptr||!gpu.ptr){ID3D12DescriptorHeap_Release(private_heap);continue;}
        out->heap=private_heap;out->path=path+1;out->num_heaps=s->num_heaps;
        for(UINT i=0;i<s->num_heaps;++i)out->heaps[i]=(int)i==resource_heap_index?private_heap:s->heaps[i];
        for(unsigned i=0;i<AZRB_REQS;++i){
            const AzrbReq *q=&l->req[i];
            if(q->mode!=AZRB_TABLE)continue;
            if(l->kind[q->root]==2)out->tables[q->root]=s->tables[q->root];
            else{
                D3D12_CPU_DESCRIPTOR_HANDLE dst={cpu.ptr+(SIZE_T)(base[q->root]+q->offset)*azrp_shadow_inc};
                if(i==0){
                    D3D12_SHADER_RESOURCE_VIEW_DESC view={0};view.Format=DXGI_FORMAT_R11G11B10_FLOAT;
                    view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
                    real_CreateShaderResourceView(g_device,snapshot,&view,dst);
                }else real_CopyDescriptorsSimple(g_device,1,dst,azrp_shadow_handle(source[i]->shadow),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                out->tables[q->root].ptr=gpu.ptr+(UINT64)base[q->root]*azrp_shadow_inc;
            }
            out->mask|=azrb_bit(q->root);
        }
        log_msg("POSTFX SR_BINDINGS resolve=%llu path=%u entries=%u root=%p tables=0x%016llx shadowUsed=%u layoutSource=%s",
            resolve,out->path,total,(void *)s->trace_graphics_rs,(unsigned long long)out->mask,azrp_shadow_used,projected?"schema_projection":object_known?"object":"cache");
        ReleaseSRWLockExclusive(&azt_lock);return 1;
    }
    log_msg("POSTFX SR_REPLAY SKIP resolve=%llu reason=%s root=%p",resolve,
        a!=AZRB_FINISH_OK&&b!=AZRB_REBASE_OK&&c!=AZRB_FINISH_OK?"root_layout_validation_failed":"all_plans_unavailable",(void *)s->trace_graphics_rs);
    ReleaseSRWLockExclusive(&azt_lock);return 0;
}
