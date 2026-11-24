/* Included once by hooks.c, after ListState and view_find. No Present hook.
   This is a bounded, CPU-side descriptor/PSO trace and a deliberately narrow
   pure-HDR-upscale replacement. It does not change any game descriptor. */
#include "sr_route_policy.h"
#include "debug_capture_policy.h"
#include <stdint.h>
#include <stdlib.h>

#define AZT_HEAPS 512
#define AZT_DESCS 131072
#define AZT_ROOTS 512
#define AZT_PSOS 8192
#define AZT_RANGES 64
#define AZT_SCAN_LIMIT 128
#define AZT_STATES 8192
#define AZT_PROBE 32

static SRWLOCK azt_lock = SRWLOCK_INIT;
static int azt_incomplete, azt_warned, azt_confirmed, azt_disabled;
static unsigned azt_misses;
#define AZT_DIRECT_QUEUES 32
static ID3D12CommandQueue *azt_direct_queues[AZT_DIRECT_QUEUES];
static unsigned azt_direct_count;
static UINT azt_reg, azt_space;
static DWORD azt_hash[4], azt_vs_hash[4];
static unsigned azt_budget = 160, azt_copy_logs = 8, azt_resolve_logs = 12;
static unsigned long long azt_generation, azt_frame;
/* All debug window fields and budgets are guarded by azt_lock. */
static AzDebugWindow azdbg;
static unsigned azdbg_geometry, azdbg_velocity, azdbg_aux;
static int azdbg_aux_warned;
/* Last recognized resolve identity is debug evidence only; never dereference or use for route. */
static uintptr_t azdbg_scene_identity, azdbg_scene_list;

typedef struct {
    ID3D12DescriptorHeap *heap;
    UINT type, count, inc;
    SIZE_T cpu;
    UINT64 gpu;
    unsigned long long generation;
} AztHeap;
typedef struct {
    SIZE_T handle;
    unsigned long long generation;
    ID3D12Resource *res;
    D3D12_RESOURCE_DESC desc;
    DXGI_FORMAT format;
    unsigned simple;
} AztDesc;
typedef struct { UINT root, offset, count, reg, space; } AztRange;
typedef struct {
    ID3D12RootSignature *rs;
    UINT count;
    int complete;
    AztRange ranges[AZT_RANGES];
} AztRoot;
typedef struct {
    ID3D12PipelineState *pso;
    ID3D12RootSignature *rs;
    DWORD hash[4], vs_hash[4];
    int pure_candidate;
    /* Observational FG metadata, not an indication that a draw renders HUD. */
    int blend_enabled, depth_enabled, compute;
    UINT color_write_mask;
} AztPso;
typedef struct { ID3D12Resource *res; ID3D12GraphicsCommandList *list; unsigned long long recording; int known; D3D12_RESOURCE_STATES state; } AztState;
static AztHeap azt_heaps[AZT_HEAPS];
static AztDesc azt_descs[AZT_DESCS];
static AztRoot azt_roots[AZT_ROOTS];
static AztPso azt_psos[AZT_PSOS];
static AztState azt_states[AZT_STATES];
static struct {
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *scene, *output;
    unsigned long long frame;
    int unmodified, copied;
} azt_pending;

static int sr_trace_enabled(void) { return dlss_sr_mode() != AZSR_DLAA; }
static unsigned azt_hashptr(UINT64 p, unsigned mask) {
    return (unsigned)((p ^ (p >> 17) ^ (p >> 37)) * 0x9e3779b97f4a7c15ull >> 32) & mask;
}
static void azt_overflow(void) {
    azt_incomplete = 1;
    if (!azt_warned++) log_msg("SR TRACE incomplete: table capacity or descriptor layout exceeded; routing disabled");
}
static int azt_env_uint(const char *name, UINT *out) {
    char b[32], tail;
    DWORD n = GetEnvironmentVariableA(name,b,sizeof(b));
    return n && n < sizeof(b) && sscanf(b,"%u%c",out,&tail)==1;
}
static int azt_env_hash(const char *name,DWORD out[4]) {
    char b[80],tail;unsigned h[4];DWORD n=GetEnvironmentVariableA(name,b,sizeof(b));
    if(n!=35)return 0;
    for(unsigned i=0;i<35;++i){char c=b[i];if(i==8||i==17||i==26){if(c!='-')return 0;}
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return 0;}
    if(sscanf(b,"%8x-%8x-%8x-%8x%c",&h[0],&h[1],&h[2],&h[3],&tail)!=4 || !(h[0]|h[1]|h[2]|h[3]))return 0;
    for(unsigned i=0;i<4;++i)out[i]=(DWORD)h[i];
    return 1;
}
static void azdbg_finish_locked(const char *reason) {
    if(!azdbg.active)return;
    log_msg("DEBUG END id=%u reason=%s resolves=%u lastResolve=%llu",azdbg.id,reason,azdbg.observed,azt_frame);
    azdebug_end(&azdbg);
    azt_budget=azt_resolve_logs=azt_copy_logs=0;
}
static int azdbg_sample(int velocity) {
    int ok=0;
    AcquireSRWLockExclusive(&azt_lock);
    unsigned *left=velocity?&azdbg_velocity:&azdbg_geometry;
    if(azdbg.active && *left){--*left;ok=1;}
    ReleaseSRWLockExclusive(&azt_lock);
    return ok;
}
/* Caller holds azt_lock. Separate budget from SR and FG so a busy
   copy/dispatch stream cannot consume their capture quotas. */
static int azdbg_aux_locked(void) {
    if(!azdbg.active)return 0;
    if(azdbg_aux){--azdbg_aux;return 1;}
    if(!azdbg_aux_warned++){log_msg("DEBUG TRUNCATED id=%u section=aux",azdbg.id);}
    return 0;
}
static void sr_trace_init(void) {
    if (dlss_sr_debug_mode()) {
        azt_budget=azt_resolve_logs=azt_copy_logs=0;
        log_msg("DEBUG READY: F8 starts/stops bounded SR+FG trace; no render replacement");
    } else if (dlss_sr_trace_only()) {
        UINT requested;
        if (azt_env_uint("AZIMUTH_SR_TRACE_BUDGET", &requested) &&
            requested >= 160 && requested <= 20000) azt_budget = requested;
        azt_resolve_logs = 64;
        log_msg("SR TRACE budget=%u (consumer records; bounded)", azt_budget);
    }
    if (dlss_sr_mode() != AZSR_ROUTE) return;
    char yes[8];DWORD y=GetEnvironmentVariableA("AZIMUTH_SR_ROUTE_CONFIRMED",yes,sizeof(yes));
    if (!azt_env_hash("AZIMUTH_SR_ROUTE_HASH",azt_hash) || !azt_env_hash("AZIMUTH_SR_ROUTE_VS_HASH",azt_vs_hash) ||
        y != 1 || yes[0] != '1' || !azt_env_uint("AZIMUTH_SR_ROUTE_REGISTER",&azt_reg) ||
        !azt_env_uint("AZIMUTH_SR_ROUTE_SPACE",&azt_space)) {
        log_msg("SR ROUTE not armed: set confirmed=1, exact four-word PS and VS hashes, register and space after GPU capture");
        return;
    }
    azt_confirmed=1;
    log_msg("SR ROUTE armed: hash=%08lx-%08lx-%08lx-%08lx vs=%08lx-%08lx-%08lx-%08lx t%u space%u (same-list pure HDR scale only)",
            (unsigned long)azt_hash[0],(unsigned long)azt_hash[1],(unsigned long)azt_hash[2],(unsigned long)azt_hash[3],
            (unsigned long)azt_vs_hash[0],(unsigned long)azt_vs_hash[1],(unsigned long)azt_vs_hash[2],(unsigned long)azt_vs_hash[3],azt_reg,azt_space);
}
static void sr_trace_queue(ID3D12CommandQueue *q,const D3D12_COMMAND_QUEUE_DESC *desc) {
    if (!q || !desc) return;
    if(dlss_sr_debug_mode()) log_msg("DEBUG QUEUE_CREATE queue=%p type=%d",(void *)q,(int)desc->Type);
    if(desc->Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
    AcquireSRWLockExclusive(&azt_lock);
    int seen=0;
    for(unsigned i=0;i<azt_direct_count && i<AZT_DIRECT_QUEUES;++i) if(azt_direct_queues[i]==q)seen=1;
    if(!seen){if(azt_direct_count<AZT_DIRECT_QUEUES)azt_direct_queues[azt_direct_count++]=q;else azt_overflow();}
    if(!seen)log_msg("SR TRACE observed direct queues=%u",azt_direct_count);
    ReleaseSRWLockExclusive(&azt_lock);
}
static int sr_trace_route_ready(void) {
    AcquireSRWLockExclusive(&azt_lock);
    int ready=azt_confirmed&&!azt_incomplete&&!azt_disabled&&azt_direct_count==1;
    ReleaseSRWLockExclusive(&azt_lock);
    return ready;
}
static void azt_note_miss(void) {
    if(++azt_misses>=16 && !azt_disabled){azt_disabled=1;log_msg("SR ROUTE disabled after 16 unresolved outputs; restart after inspecting trace");}
}
static AztHeap *azt_heap_for_ptr(ID3D12DescriptorHeap *p) {
    for (unsigned i=0;i<AZT_HEAPS;++i) if (azt_heaps[i].heap==p) return &azt_heaps[i];
    return NULL;
}
static AztHeap *azt_heap_for_cpu(SIZE_T h, UINT type) {
    AztHeap *best=NULL;
    for (unsigned i=0;i<AZT_HEAPS;++i) {
        AztHeap *x=&azt_heaps[i];
        if (!x->heap || x->type!=type || !x->inc || h<x->cpu) continue;
        SIZE_T delta=h-x->cpu;
        if (delta%x->inc || delta/x->inc>=x->count) continue;
        if (!best || x->generation>best->generation) best=x;
    }
    return best;
}
static AztDesc *azt_slot(AztHeap *h, SIZE_T cpu, int insert) {
    if (!h) return NULL;
    unsigned i=azt_hashptr((UINT64)cpu ^ h->generation, AZT_DESCS-1);
    for (unsigned k=0;k<AZT_PROBE;++k) {
        AztDesc *d=&azt_descs[(i+k)&(AZT_DESCS-1)];
        if (d->handle==cpu && d->generation==h->generation) return d;
        if (!d->generation) {
            if (!insert) return NULL;
            d->handle=cpu; d->generation=h->generation; return d;
        }
    }
    if (insert) azt_overflow();
    return NULL;
}
static void sr_trace_heap(ID3D12Device *dev, ID3D12DescriptorHeap *heap, const D3D12_DESCRIPTOR_HEAP_DESC *desc) {
    if (!sr_trace_enabled() || !heap || !desc || (desc->Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc->Type!=D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) return;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu={0};
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap,&cpu);
    if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap,&gpu);
    UINT inc=ID3D12Device_GetDescriptorHandleIncrementSize(dev,desc->Type);
    AcquireSRWLockExclusive(&azt_lock);
    AztHeap *h=azt_heap_for_ptr(heap);
    if (!h) for (unsigned i=0;i<AZT_HEAPS;++i) if (!azt_heaps[i].heap) {h=&azt_heaps[i];break;}
    if (h) *h=(AztHeap){heap,(UINT)desc->Type,desc->NumDescriptors,inc,cpu.ptr,gpu.ptr,++azt_generation};
    else azt_overflow();
    ReleaseSRWLockExclusive(&azt_lock);
}
static void azt_write_desc(SIZE_T handle,UINT type,ID3D12Resource *res,DXGI_FORMAT format,int simple) {
    D3D12_RESOURCE_DESC rd;
    memset(&rd,0,sizeof(rd));
    if (res) res->lpVtbl->GetDesc(res,&rd);
    if (format==DXGI_FORMAT_UNKNOWN) format=rd.Format;
    if (rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.DepthOrArraySize!=1 || rd.SampleDesc.Count!=1) simple=0;
    AcquireSRWLockExclusive(&azt_lock);
    AztDesc *d=azt_slot(azt_heap_for_cpu(handle,type),handle,1);
    if (d) {d->res=res;d->desc=rd;d->format=format;d->simple=(unsigned)(simple!=0);}
    ReleaseSRWLockExclusive(&azt_lock);
}
static void sr_trace_rtv(ID3D12Resource *res,const D3D12_RENDER_TARGET_VIEW_DESC *desc,D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (!sr_trace_enabled()) return;
    int simple=!desc || (desc->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2D && desc->Texture2D.MipSlice==0 && desc->Texture2D.PlaneSlice==0);
    azt_write_desc(h.ptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,res,desc?desc->Format:DXGI_FORMAT_UNKNOWN,simple);
}
static void sr_trace_srv(ID3D12Resource *res,const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (!sr_trace_enabled()) return;
    int simple=!desc || (desc->ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2D && desc->Texture2D.MostDetailedMip==0 &&
        desc->Texture2D.PlaneSlice==0 && desc->Texture2D.ResourceMinLODClamp==0.0f && desc->Shader4ComponentMapping==D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    azt_write_desc(h.ptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,res,desc?desc->Format:DXGI_FORMAT_UNKNOWN,simple);
}
static void azt_copy_one(SIZE_T dst,SIZE_T src,UINT type) {
    AztDesc *s=azt_slot(azt_heap_for_cpu(src,type),src,0);
    AztDesc *d=azt_slot(azt_heap_for_cpu(dst,type),dst,1);
    if (!d) return;
    if (s) {d->res=s->res;d->desc=s->desc;d->format=s->format;d->simple=s->simple;}
    else {d->res=NULL;d->simple=0;d->format=DXGI_FORMAT_UNKNOWN;memset(&d->desc,0,sizeof(d->desc));}
}
static void sr_trace_copy_descriptors(ID3D12Device *dev,UINT nd,const D3D12_CPU_DESCRIPTOR_HANDLE *dst,const UINT *ds,
    UINT ns,const D3D12_CPU_DESCRIPTOR_HANDLE *src,const UINT *ss,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (!sr_trace_enabled() || (type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && type!=D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) return;
    UINT inc=ID3D12Device_GetDescriptorHandleIncrementSize(dev,type);
    if (!inc || !dst || !src) return;
    AcquireSRWLockExclusive(&azt_lock);
    UINT di=0,si=0,dj=0,sj=0,total=0;
    while (di<nd && si<ns) {
        UINT dc=ds?ds[di]:1,sc=ss?ss[si]:1;
        if (dj>=dc) {++di;dj=0;continue;}
        if (sj>=sc) {++si;sj=0;continue;}
        if (++total>AZT_DESCS) {azt_overflow();break;}
        azt_copy_one(dst[di].ptr+(SIZE_T)dj*inc,src[si].ptr+(SIZE_T)sj*inc,(UINT)type);
        ++dj;++sj;
    }
    ReleaseSRWLockExclusive(&azt_lock);
}

static AztRoot *azt_root_slot(ID3D12RootSignature *rs,int insert) {
    if(!rs)return NULL;
    unsigned i=azt_hashptr((UINT64)(uintptr_t)rs,AZT_ROOTS-1);
    for (unsigned k=0;k<AZT_PROBE;++k) {
        AztRoot *r=&azt_roots[(i+k)&(AZT_ROOTS-1)];
        if (r->rs==rs) return r;
        if (!r->rs) {if(insert){r->rs=rs;return r;}return NULL;}
    }
    if(insert)azt_overflow();
    return NULL;
}
static void azt_add_range(AztRoot *r,UINT root,UINT type,UINT n,UINT reg,UINT space,UINT start,UINT *next) {
    UINT offset=start==D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND?*next:start;
    if (offset==UINT_MAX || n==UINT_MAX || n>UINT_MAX-offset) {r->complete=0;*next=UINT_MAX;return;}
    *next=offset+n;
    if (type!=D3D12_DESCRIPTOR_RANGE_TYPE_SRV || !n) return;
    if (r->count>=AZT_RANGES || reg>UINT_MAX-n) {r->complete=0;return;}
    if(n>AZT_SCAN_LIMIT)r->complete=0;
    r->ranges[r->count++]=(AztRange){root,offset,n>AZT_SCAN_LIMIT?AZT_SCAN_LIMIT:n,reg,space};
}
static void sr_trace_root(ID3D12RootSignature *rs,const void *blob,SIZE_T bytes) {
    if (!sr_trace_enabled() || !rs || !blob || !bytes) return;
    typedef HRESULT(WINAPI *Deserializer)(LPCVOID,SIZE_T,REFIID,void **);
    HMODULE dll=GetModuleHandleW(L"d3d12.dll");
    Deserializer fn=dll?(Deserializer)(void *)GetProcAddress(dll,"D3D12CreateVersionedRootSignatureDeserializer"):NULL;
    ID3D12VersionedRootSignatureDeserializer *obj=NULL;
    if(!fn || FAILED(fn(blob,bytes,&IID_ID3D12VersionedRootSignatureDeserializer,(void **)&obj)) || !obj) return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *d=obj->lpVtbl->GetUnconvertedRootSignatureDesc(obj);
    if (!d) {obj->lpVtbl->Release(obj);return;}
    AztRoot v;memset(&v,0,sizeof(v));v.rs=rs;v.complete=1;
    if(d->Version==D3D_ROOT_SIGNATURE_VERSION_1_0) {
        const D3D12_ROOT_SIGNATURE_DESC *rd=&d->Desc_1_0;
        if(rd->NumParameters>32 || ((UINT)rd->Flags & 0x400u)) v.complete=0;
        for(UINT i=0;i<rd->NumParameters && i<32;++i) {
            const D3D12_ROOT_PARAMETER *p=&rd->pParameters[i];
            if(p->ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || (p->ShaderVisibility!=D3D12_SHADER_VISIBILITY_PIXEL && p->ShaderVisibility!=D3D12_SHADER_VISIBILITY_ALL)) continue;
            UINT next=0;
            for(UINT j=0;j<p->DescriptorTable.NumDescriptorRanges;++j) {
                const D3D12_DESCRIPTOR_RANGE *x=&p->DescriptorTable.pDescriptorRanges[j];
                azt_add_range(&v,i,x->RangeType,x->NumDescriptors,x->BaseShaderRegister,x->RegisterSpace,x->OffsetInDescriptorsFromTableStart,&next);
            }
        }
    } else if(d->Version==D3D_ROOT_SIGNATURE_VERSION_1_1) {
        const D3D12_ROOT_SIGNATURE_DESC1 *rd=&d->Desc_1_1;
        if(rd->NumParameters>32 || ((UINT)rd->Flags & 0x400u)) v.complete=0;
        for(UINT i=0;i<rd->NumParameters && i<32;++i) {
            const D3D12_ROOT_PARAMETER1 *p=&rd->pParameters[i];
            if(p->ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || (p->ShaderVisibility!=D3D12_SHADER_VISIBILITY_PIXEL && p->ShaderVisibility!=D3D12_SHADER_VISIBILITY_ALL)) continue;
            UINT next=0;
            for(UINT j=0;j<p->DescriptorTable.NumDescriptorRanges;++j) {
                const D3D12_DESCRIPTOR_RANGE1 *x=&p->DescriptorTable.pDescriptorRanges[j];
                azt_add_range(&v,i,x->RangeType,x->NumDescriptors,x->BaseShaderRegister,x->RegisterSpace,x->OffsetInDescriptorsFromTableStart,&next);
            }
        }
    } else v.complete=0;
    obj->lpVtbl->Release(obj);
    AcquireSRWLockExclusive(&azt_lock);
    AztRoot *r=azt_root_slot(rs,1);if(r)*r=v;
    ReleaseSRWLockExclusive(&azt_lock);
}
static AztPso *azt_pso_slot(ID3D12PipelineState *pso,int insert) {
    if(!pso)return NULL;
    unsigned i=azt_hashptr((UINT64)(uintptr_t)pso,AZT_PSOS-1);
    for(unsigned k=0;k<AZT_PROBE;++k){AztPso *p=&azt_psos[(i+k)&(AZT_PSOS-1)];if(p->pso==pso)return p;if(!p->pso){if(insert){p->pso=pso;return p;}return NULL;}}
    if(insert)azt_overflow();
    return NULL;
}
static void sr_trace_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d,ID3D12PipelineState *pso) {
    if(!sr_trace_enabled() || !d || !pso)return;
    AztPso v;memset(&v,0,sizeof(v));v.pso=pso;v.rs=d->pRootSignature;
    if(d->PS.BytecodeLength>=20 && d->PS.pShaderBytecode && !memcmp(d->PS.pShaderBytecode,"DXBC",4)) memcpy(v.hash,(const BYTE *)d->PS.pShaderBytecode+4,16);
    if(d->VS.BytecodeLength>=20 && d->VS.pShaderBytecode && !memcmp(d->VS.pShaderBytecode,"DXBC",4)) memcpy(v.vs_hash,(const BYTE *)d->VS.pShaderBytecode+4,16);
    const D3D12_RENDER_TARGET_BLEND_DESC *b=&d->BlendState.RenderTarget[0];
    v.blend_enabled=b->BlendEnable;
    v.depth_enabled=d->DepthStencilState.DepthEnable;
    v.color_write_mask=b->RenderTargetWriteMask;
    v.pure_candidate=d->NumRenderTargets==1 && d->RTVFormats[0]==DXGI_FORMAT_R11G11B10_FLOAT && d->DSVFormat==DXGI_FORMAT_UNKNOWN &&
        !d->DepthStencilState.DepthEnable && !d->DepthStencilState.StencilEnable && !b->BlendEnable && !b->LogicOpEnable &&
        b->RenderTargetWriteMask==D3D12_COLOR_WRITE_ENABLE_ALL && !d->BlendState.AlphaToCoverageEnable && d->SampleMask==UINT_MAX &&
        d->SampleDesc.Count==1 && d->SampleDesc.Quality==0 && d->PrimitiveTopologyType==D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
        d->RasterizerState.FillMode==D3D12_FILL_MODE_SOLID && !d->StreamOutput.NumEntries &&
        !d->GS.BytecodeLength && !d->HS.BytecodeLength && !d->DS.BytecodeLength;
    AcquireSRWLockExclusive(&azt_lock);AztPso *p=azt_pso_slot(pso,1);if(p)*p=v;ReleaseSRWLockExclusive(&azt_lock);
}
static AztState *azt_state(ID3D12Resource *res,int insert) {
    if(!res)return NULL;
    unsigned i=azt_hashptr((UINT64)(uintptr_t)res,AZT_STATES-1);
    for(unsigned k=0;k<AZT_PROBE;++k){AztState *p=&azt_states[(i+k)&(AZT_STATES-1)];if(p->res==res)return p;if(!p->res){if(insert){p->res=res;return p;}return NULL;}}
    if(insert)azt_overflow();
    return NULL;
}
static void sr_trace_barriers(ID3D12GraphicsCommandList *cl,ListState *s,UINT n,const D3D12_RESOURCE_BARRIER *b) {
    if(!sr_trace_enabled() || !b || !s)return;
    AcquireSRWLockExclusive(&azt_lock);
    if(!s->trace_recording_id) s->trace_recording_id=++azt_generation;
    for(UINT i=0;i<n;++i){
        if(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){
            if(azt_pending.scene && (!b[i].Aliasing.pResourceAfter || b[i].Aliasing.pResourceAfter==azt_pending.scene || b[i].Aliasing.pResourceBefore==azt_pending.scene))azt_pending.unmodified=0;
            if(b[i].Aliasing.pResourceAfter){AztState *a=azt_state(b[i].Aliasing.pResourceAfter,0);if(a)a->known=0;}
            else for(unsigned j=0;j<AZT_STATES;++j)azt_states[j].known=0;
            continue;
        }
        if(b[i].Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b[i].Transition.pResource)continue;
        ID3D12Resource *res=b[i].Transition.pResource;
        AztState *a=azt_state(res,1);
        if(a){a->known=b[i].Flags==D3D12_RESOURCE_BARRIER_FLAG_NONE && (b[i].Transition.Subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || b[i].Transition.Subresource==0);a->state=b[i].Transition.StateAfter;a->list=cl;a->recording=s->trace_recording_id;}
        if(res==azt_pending.scene && (b[i].Transition.StateAfter & (D3D12_RESOURCE_STATE_RENDER_TARGET|D3D12_RESOURCE_STATE_UNORDERED_ACCESS|D3D12_RESOURCE_STATE_COPY_DEST|D3D12_RESOURCE_STATE_RESOLVE_DEST)))azt_pending.unmodified=0;
    }
    ReleaseSRWLockExclusive(&azt_lock);
}
static void sr_trace_write(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst) {
    if(!sr_trace_enabled())return;
    AcquireSRWLockExclusive(&azt_lock);
    if(azt_pending.scene && ((dst && dst==azt_pending.scene) || (!dst && cl==azt_pending.list)))azt_pending.unmodified=0;
    ReleaseSRWLockExclusive(&azt_lock);
}
static void sr_trace_reset(ID3D12GraphicsCommandList *cl) {
    if(!sr_trace_enabled())return;
    AcquireSRWLockExclusive(&azt_lock);
    int missed=azt_pending.list==cl && azt_pending.output && !azt_pending.copied && dlss_sr_mode()==AZSR_ROUTE;
    if(missed)azt_note_miss();
    if(azt_pending.list==cl){memset(&azt_pending,0,sizeof(azt_pending));}
    ReleaseSRWLockExclusive(&azt_lock);
    if(missed)dlss_sr_route_result(0);
}
static void sr_trace_begin_resolve(void) {
    if(!sr_trace_enabled())return;
    int missed=0;
    AcquireSRWLockExclusive(&azt_lock);
    missed=azt_pending.output && !azt_pending.copied && dlss_sr_mode()==AZSR_ROUTE;
    if(missed)azt_note_miss();
    if(missed && azt_budget){--azt_budget;log_msg("SR ROUTE MISS frame=%llu: no verified consumer; original draw retained",azt_pending.frame);}
    memset(&azt_pending,0,sizeof(azt_pending));
    ReleaseSRWLockExclusive(&azt_lock);
    if(missed)dlss_sr_route_result(0);
}
static void sr_trace_resolved(ID3D12GraphicsCommandList *cl,ID3D12Resource *scene,ID3D12Resource *output) {
    if(!sr_trace_enabled() || !scene)return;
    AcquireSRWLockExclusive(&azt_lock);
    azt_pending.list=cl;azt_pending.scene=scene;azt_pending.output=output;
    azt_pending.frame=++azt_frame;azt_pending.unmodified=1;azt_pending.copied=0;
    if(dlss_sr_debug_mode()) {
        azdbg_scene_identity=(uintptr_t)scene; azdbg_scene_list=(uintptr_t)cl;
        if(azdbg.active && !azdebug_resolve(&azdbg)) azdbg_finish_locked("resolve_limit");
        if(azdbg.active)log_msg("DEBUG RESOLVE_RESOURCE id=%u resolve=%llu scene=%p list=%p identityOnly=1",azdbg.id,azt_frame,(void *)scene,(void *)cl);
    }
    if(azt_resolve_logs){--azt_resolve_logs;D3D12_RESOURCE_DESC sd;scene->lpVtbl->GetDesc(scene,&sd);
        log_msg("SR TRACE RESOLVE frame=%llu list=%p scene=%ux%u format=%d evaluated=%d",azt_pending.frame,(void *)cl,(UINT)sd.Width,sd.Height,(int)sd.Format,output!=NULL);}
    ReleaseSRWLockExclusive(&azt_lock);
}
static int azt_full_texture(const D3D12_RESOURCE_DESC *d) {
    return d->Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && d->DepthOrArraySize==1 && d->MipLevels==1 &&
        d->SampleDesc.Count==1 && !d->SampleDesc.Quality && d->Format==DXGI_FORMAT_R11G11B10_FLOAT;
}
static AztDesc *azt_bound_srv(ListState *s,const AztRange *r,UINT k) {
    if(!(s->trace_tables_mask&(1u<<r->root)))return NULL;
    for(UINT i=0;i<s->num_heaps;++i){
        AztHeap *h=azt_heap_for_ptr(s->heaps[i]);uintptr_t cpu;
        if(!h || h->type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !azroute_descriptor_cpu(s->tables[r->root].ptr,h->gpu,h->cpu,h->inc,h->count,r->offset+k,&cpu))continue;
        return azt_slot(h,(SIZE_T)cpu,0);
    }
    return NULL;
}
/* Returns 1 only if this exact original draw is replaced with a same-size copy.
   A recorded bound SRV is evidence of binding, not evidence the shader samples it. */
static int sr_trace_draw(ID3D12GraphicsCommandList *cl,ListState *s,UINT count,UINT instances,int role,int start_zero) {
    if(!sr_trace_enabled() || !s)return 0;
    AcquireSRWLockExclusive(&azt_lock);
    if((dlss_sr_debug_mode() && !azdbg.active) || !azt_pending.scene || (!azt_budget && dlss_sr_mode()!=AZSR_ROUTE)){ReleaseSRWLockExclusive(&azt_lock);return 0;}
    AztDesc *target=NULL;
    if(s->num_rtv==1)target=azt_slot(azt_heap_for_cpu(s->rtv[0].ptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV),s->rtv[0].ptr,0);
    /* Invalidate if any bound target writes into the source. Multi-RTV draws are
       conservatively disqualified when their target identity cannot be proven. */
    if(s->num_rtv>1 && s->rtv_single)azt_pending.unmodified=0;
    else for(UINT i=0;i<s->num_rtv;++i){
        SIZE_T h=s->rtv[i].ptr;AztDesc *t=azt_slot(azt_heap_for_cpu(h,D3D12_DESCRIPTOR_HEAP_TYPE_RTV),h,0);
        if(!t || !t->res || t->res==azt_pending.scene)azt_pending.unmodified=0;
    }
    if(role!=ROLE_NONE || !s->trace_graphics_rs || !s->pso){ReleaseSRWLockExclusive(&azt_lock);return 0;}
    AztPso *p=azt_pso_slot(s->pso,0);AztRoot *r=azt_root_slot(s->trace_graphics_rs,0);
    if(!p || !r){ReleaseSRWLockExclusive(&azt_lock);return 0;}
    int matched=0;UINT reg=0,space=0,root=0;AztDesc *source=NULL;
    for(UINT i=0;i<r->count;++i){const AztRange *x=&r->ranges[i];for(UINT k=0;k<x->count;++k){AztDesc *d=azt_bound_srv(s,x,k);if(!d || d->res!=azt_pending.scene)continue;
        if(!matched || (x->reg+k==azt_reg && x->space==azt_space)){reg=x->reg+k;space=x->space;root=x->root;source=d;}
        ++matched;
    }}
    if(!matched){ReleaseSRWLockExclusive(&azt_lock);return 0;}
    AzSRSize size={target?(UINT)target->desc.Width:0,target?target->desc.Height:0};
    AzSRSize vp={0,0};int full=0;
    if(s->num_vp==1 && s->num_sc==1 && s->vp[0].Width>0 && s->vp[0].Height>0 && s->vp[0].Width<=16384 && s->vp[0].Height<=16384){
        vp=(AzSRSize){(UINT)s->vp[0].Width,(UINT)s->vp[0].Height};
        full=s->vp[0].TopLeftX==0 && s->vp[0].TopLeftY==0 && s->vp[0].Width==(float)vp.w && s->vp[0].Height==(float)vp.h &&
            s->sc[0].left==0 && s->sc[0].top==0 && s->sc[0].right==(LONG)vp.w && s->sc[0].bottom==(LONG)vp.h;
    }
    AztState *ts=target&&target->res?azt_state(target->res,0):NULL;
    int known_rt=ts&&ts->known&&ts->state==D3D12_RESOURCE_STATE_RENDER_TARGET&&ts->list==cl&&s->trace_recording_id&&ts->recording==s->trace_recording_id;
    if(azt_budget){--azt_budget;log_msg("SR CONSUMER frame=%llu list=%p same=%d hash=%08lx-%08lx-%08lx-%08lx vs=%08lx-%08lx-%08lx-%08lx t%u space%u root=%u matches=%d target=%ux%u fmt=%d vp=%ux%u pureCandidate=%d stateRT=%d unchanged=%d layout=%d draw=%u/%u",
        azt_pending.frame,(void *)cl,cl==azt_pending.list,(unsigned long)p->hash[0],(unsigned long)p->hash[1],(unsigned long)p->hash[2],(unsigned long)p->hash[3],(unsigned long)p->vs_hash[0],(unsigned long)p->vs_hash[1],(unsigned long)p->vs_hash[2],(unsigned long)p->vs_hash[3],reg,space,root,matched,size.w,size.h,target?(int)target->format:0,vp.w,vp.h,p->pure_candidate,known_rt,azt_pending.unmodified,r->complete&&!azt_incomplete,count,instances);}
    if(dlss_sr_mode()!=AZSR_ROUTE || !azt_pending.output || azt_pending.copied || azt_disabled || azt_direct_count!=1 || !real_CopyResource){ReleaseSRWLockExclusive(&azt_lock);return 0;}
    D3D12_RESOURCE_DESC out;azt_pending.output->lpVtbl->GetDesc(azt_pending.output,&out);
    AzSRSize os={(UINT)out.Width,out.Height};
    AzRouteGate gate={os,size,vp,azt_confirmed,1,cl==azt_pending.list,
        matched==1 && reg==azt_reg && space==azt_space && source && source->simple && source->format==DXGI_FORMAT_R11G11B10_FLOAT && azt_full_texture(&source->desc),
        !memcmp(p->hash,azt_hash,16)&&!memcmp(p->vs_hash,azt_vs_hash,16),r->complete && !azt_incomplete && p->rs==s->trace_graphics_rs,
        s->num_rtv==1&&!s->has_dsv,full,p->pure_candidate,target&&target->res&&target->simple&&target->res!=azt_pending.scene&&target->res!=azt_pending.output&&target->format==DXGI_FORMAT_R11G11B10_FLOAT&&azt_full_texture(&target->desc)&&azt_full_texture(&out)&&target->desc.Layout==out.Layout,
        known_rt,instances==1&&start_zero&&(count==3||count==6)&&s->trace_topology==D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST&&!s->trace_predicated&&!s->trace_queries&&!s->trace_query_unknown,azt_pending.unmodified,dlss_enabled()};
    if(!azroute_allow(gate)){ReleaseSRWLockExclusive(&azt_lock);return 0;}
    /* Only a verified pure scale draw can be replaced this way. The original
       command list, bindings and PSO are untouched; transitions are restored. */
    ID3D12Resource *dst=target->res,*src=azt_pending.output;
    hooks_transition(cl,dst,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
    hooks_transition(cl,src,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    real_CopyResource(cl,dst,src);
    hooks_transition(cl,src,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    hooks_transition(cl,dst,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
    azt_pending.copied=1;azt_misses=0;
    dlss_sr_route_result(1);
    if(azt_copy_logs){--azt_copy_logs;log_msg("SR ROUTE COPIED frame=%llu %ux%u (verify final output and postprocessing)",azt_pending.frame,os.w,os.h);}
    ReleaseSRWLockExclusive(&azt_lock);
    return 1;
}
