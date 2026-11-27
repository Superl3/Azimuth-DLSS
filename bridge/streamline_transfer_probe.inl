// D4 observation only: independent resource IDs for descriptor matching.
// No new lineage edge, queue wait, tag or FG activation is authorized here.
constexpr GUID probe_view_guid={0xe2b2cdd0,0xad89,0x4c13,{0x85,0x7f,0x91,0x6d,0x4e,0x32,0x9c,0xf1}};
constexpr GUID probe_fence_guid={0x27cab516,0x1114,0x4678,{0x90,0x6a,0x54,0x2a,0x60,0x93,0xbd,0xa4}};
uint64_t probe_view_serial=0,probe_fence_serial=0,probe_attempt=0,probe_execute_serial=0;
std::array<uint64_t,3> probe_view_bb{};
std::unordered_map<SIZE_T,ProbeView> probe_views;
std::unordered_map<uintptr_t,ProbeRoot> probe_roots;
std::vector<uint64_t> probe_final_cookies;
unsigned probe_submit_logs=0,probe_sync_logs=0;
bool probe_view_overflow=false;
thread_local bool probe_internal_signal=false;
uint64_t probe_private_id(ID3D12Object *o,const GUID &key,uint64_t &serial){
    if(!o)return 0;uint64_t id=0;UINT size=sizeof(id);
    if(SUCCEEDED(o->GetPrivateData(key,&size,&id))&&size==sizeof(id)&&id)return id;
    id=++serial;return SUCCEEDED(o->SetPrivateData(key,sizeof(id),&id))?id:0;
}
uint64_t probe_view_identity(ID3D12Resource *r){
    return r&&rgb10_geometry(r->GetDesc())?probe_private_id(r,probe_view_guid,probe_view_serial):0;
}
ProbeView probe_get_view(SIZE_T cpu){
    auto it=probe_views.find(cpu);return it==probe_views.end()?ProbeView{}:it->second;
}
void probe_set_view(SIZE_T cpu,ProbeView v){
    if(!cpu)return;
    if(!v.identity){probe_views.erase(cpu);return;}
    if(probe_views.size()>=32768&&!probe_views.count(cpu)){probe_view_overflow=true;return;}
    probe_views[cpu]=v;
}
void probe_note_final(uint64_t cookie,uint64_t view,uint64_t resolve,uint64_t packet){
    if(cookie&&std::find(probe_final_cookies.begin(),probe_final_cookies.end(),cookie)==probe_final_cookies.end()&&probe_final_cookies.size()<64)
        probe_final_cookies.push_back(cookie);
    log("FG TRANSFER_FINAL resolve=%llu packet=%llu generation=%llu cookie=%llu viewIdentity=%llu qpc=%lld",resolve,packet,generation,cookie,view,qpc());
}
bool probe_is_final(uint64_t c){return c&&std::find(probe_final_cookies.begin(),probe_final_cookies.end(),c)!=probe_final_cookies.end();}
bool probe_is_view_bb(uint64_t c){return c&&std::find(probe_view_bb.begin(),probe_view_bb.end(),c)!=probe_view_bb.end();}
void probe_copy_views(ID3D12Device *d,UINT nd,const D3D12_CPU_DESCRIPTOR_HANDLE *ds,const UINT *dn,UINT ns,const D3D12_CPU_DESCRIPTOR_HANDLE *ss,const UINT *sn){
    // Snapshot source descriptors before updating the destination map.
    if(!ds||!ss)return;UINT inc=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    uint64_t dest_count=0,source_count=0;
    for(UINT i=0;i<nd;++i)dest_count+=dn?dn[i]:1;
    for(UINT i=0;i<ns;++i)source_count+=sn?sn[i]:1;
    if(dest_count>65536||source_count>65536){probe_views.clear();probe_view_overflow=true;return;}
    std::vector<ProbeView> values;values.reserve((size_t)source_count);
    for(UINT i=0;i<ns;++i)for(UINT j=0;j<(sn?sn[i]:1);++j)values.push_back(probe_get_view(ss[i].ptr+SIZE_T(j)*inc));
    size_t k=0;for(UINT i=0;i<nd;++i)for(UINT j=0;j<(dn?dn[i]:1);++j,++k)
        probe_set_view(ds[i].ptr+SIZE_T(j)*inc,k<values.size()?values[k]:ProbeView{});
}
void STDMETHODCALLTYPE probe_srv(ID3D12Device *d,ID3D12Resource *r,const D3D12_SHADER_RESOURCE_VIEW_DESC *v,D3D12_CPU_DESCRIPTOR_HANDLE h){
    auto fn=original<decltype(&probe_srv)>(d,18);fn(d,r,v,h);
    std::lock_guard<std::recursive_mutex> g(lock);
    auto id=probe_view_identity(r);
    probe_set_view(h.ptr,{id,1,v?(unsigned)v->Format:(id?(unsigned)r->GetDesc().Format:0)});
}
void STDMETHODCALLTYPE probe_uav(ID3D12Device *d,ID3D12Resource *r,ID3D12Resource *counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC *v,D3D12_CPU_DESCRIPTOR_HANDLE h){
    auto fn=original<decltype(&probe_uav)>(d,19);fn(d,r,counter,v,h);
    std::lock_guard<std::recursive_mutex> g(lock);
    auto id=probe_view_identity(r);
    probe_set_view(h.ptr,{id,2,v?(unsigned)v->Format:(id?(unsigned)r->GetDesc().Format:0)});
}
void STDMETHODCALLTYPE probe_cbv(ID3D12Device *d,const D3D12_CONSTANT_BUFFER_VIEW_DESC *v,D3D12_CPU_DESCRIPTOR_HANDLE h){
    auto fn=original<decltype(&probe_cbv)>(d,17);fn(d,v,h);
    std::lock_guard<std::recursive_mutex> g(lock);probe_views.erase(h.ptr);
}
HRESULT STDMETHODCALLTYPE probe_heap(ID3D12Device *d,const D3D12_DESCRIPTOR_HEAP_DESC *desc,REFIID iid,void **out){
    auto fn=original<decltype(&probe_heap)>(d,14);auto hr=fn(d,desc,iid,out);
    if(SUCCEEDED(hr)&&desc&&desc->Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV&&out&&*out){
        ComPtr<ID3D12DescriptorHeap> h;if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&h)))&&h){
            auto start=h->GetCPUDescriptorHandleForHeapStart().ptr;auto inc=d->GetDescriptorHandleIncrementSize(desc->Type);
            std::lock_guard<std::recursive_mutex> g(lock);
            // Drop tracked slots in a newly allocated/reused heap address range.
            uint64_t bytes=uint64_t(desc->NumDescriptors)*inc;
            for(auto it=probe_views.begin();it!=probe_views.end();){
                if(it->first>=start&&uint64_t(it->first-start)<bytes)it=probe_views.erase(it);else ++it;
            }
        }
    }return hr;
}
template<class Desc> void probe_parse_root(ProbeRoot &r,const Desc &d){
    r.complete=true;
    if(d.NumParameters>64)r.complete=false;
    for(UINT i=0;i<d.NumParameters&&i<64;++i){
        auto &p=d.pParameters[i];if(p.ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)continue;
        uint64_t offset=0;
        for(UINT j=0;j<p.DescriptorTable.NumDescriptorRanges;++j){
            auto &v=p.DescriptorTable.pDescriptorRanges[j];
            if(v.OffsetInDescriptorsFromTableStart!=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)offset=v.OffsetInDescriptorsFromTableStart;
            if(v.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_SRV||v.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_UAV){
                if(r.ranges.size()>=128||offset>UINT_MAX){r.complete=false;break;}
                r.ranges.push_back({i,v.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_SRV?1u:2u,v.BaseShaderRegister,v.RegisterSpace,(unsigned)offset,v.NumDescriptors});
            }
            if(v.NumDescriptors==UINT_MAX){r.complete=false;break;}offset+=v.NumDescriptors;
        }
    }
}
HRESULT STDMETHODCALLTYPE probe_root(ID3D12Device *d,UINT node,const void *blob,SIZE_T size,REFIID iid,void **out){
    auto fn=original<decltype(&probe_root)>(d,16);auto hr=fn(d,node,blob,size,iid,out);
    if(SUCCEEDED(hr)&&out&&*out&&blob&&size){
        ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12VersionedRootSignatureDeserializer> parser;
        if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&root)))&&root){
            ProbeRoot info;
            if(SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob,size,IID_PPV_ARGS(&parser)))&&parser){
                auto v=parser->GetUnconvertedRootSignatureDesc();
                if(v&&v->Version==D3D_ROOT_SIGNATURE_VERSION_1_0)probe_parse_root(info,v->Desc_1_0);
                else if(v&&v->Version==D3D_ROOT_SIGNATURE_VERSION_1_1)probe_parse_root(info,v->Desc_1_1);
            }
            std::lock_guard<std::recursive_mutex> g(lock);
            if(probe_roots.size()<4096||probe_roots.count(reinterpret_cast<uintptr_t>(root.Get())))
                probe_roots[reinterpret_cast<uintptr_t>(root.Get())]=std::move(info);
        }
    }return hr;
}
void STDMETHODCALLTYPE probe_pso(ID3D12GraphicsCommandList *cl,ID3D12PipelineState *p){
    auto fn=original<decltype(&probe_pso)>(cl,25);
    {std::lock_guard<std::recursive_mutex> g(lock);record(cl).probe.pso=reinterpret_cast<uintptr_t>(p);}fn(cl,p);
}
void STDMETHODCALLTYPE probe_heaps(ID3D12GraphicsCommandList *cl,UINT n,ID3D12DescriptorHeap *const *hs){
    auto fn=original<decltype(&probe_heaps)>(cl,28);
    ProbeHeap heap;
    for(UINT i=0;hs&&i<n;++i)if(hs[i]){
        auto d=hs[i]->GetDesc();
        if(d.Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV&&(d.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)&&device){
            heap={hs[i]->GetCPUDescriptorHandleForHeapStart().ptr,hs[i]->GetGPUDescriptorHandleForHeapStart().ptr,d.NumDescriptors,device->GetDescriptorHandleIncrementSize(d.Type)};
        }
    }
    {std::lock_guard<std::recursive_mutex> g(lock);auto &p=record(cl).probe;
     if(p.heap.cpu!=heap.cpu||p.heap.gpu!=heap.gpu||p.heap.count!=heap.count||p.heap.inc!=heap.inc){p.graphics={};p.compute={};}p.heap=heap;}
    fn(cl,n,hs);
}
void STDMETHODCALLTYPE probe_groot(ID3D12GraphicsCommandList *cl,ID3D12RootSignature *root){
    auto fn=original<decltype(&probe_groot)>(cl,30);
    {std::lock_guard<std::recursive_mutex> g(lock);auto &p=record(cl).probe;auto id=reinterpret_cast<uintptr_t>(root);if(p.graphics_root!=id)p.graphics={};p.graphics_root=id;}fn(cl,root);
}
void STDMETHODCALLTYPE probe_croot(ID3D12GraphicsCommandList *cl,ID3D12RootSignature *root){
    auto fn=original<decltype(&probe_croot)>(cl,29);
    {std::lock_guard<std::recursive_mutex> g(lock);auto &p=record(cl).probe;auto id=reinterpret_cast<uintptr_t>(root);if(p.compute_root!=id)p.compute={};p.compute_root=id;}fn(cl,root);
}
void STDMETHODCALLTYPE probe_gtable(ID3D12GraphicsCommandList *cl,UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){
    auto fn=original<decltype(&probe_gtable)>(cl,32);
    {std::lock_guard<std::recursive_mutex> g(lock);auto &p=record(cl).probe;if(i<p.graphics.size())p.graphics[i]=h.ptr;}fn(cl,i,h);
}
void STDMETHODCALLTYPE probe_ctable(ID3D12GraphicsCommandList *cl,UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){
    auto fn=original<decltype(&probe_ctable)>(cl,31);
    {std::lock_guard<std::recursive_mutex> g(lock);auto &p=record(cl).probe;if(i<p.compute.size())p.compute[i]=h.ptr;}fn(cl,i,h);
}
void probe_work(Record &r,const char *method,uint64_t dst,bool compute,unsigned a,unsigned b,unsigned c){
    if(!uav_probe||r.work.size()>=64)return;
    const auto &p=r.probe;const auto &h=p.heap;
    const auto &tables=compute?p.compute:p.graphics;
    ProbeWork w;w.method=method;w.dst=dst;w.pso=p.pso;w.root=compute?p.compute_root:p.graphics_root;w.a=a;w.b=b;w.c=c;w.ordinal=(unsigned)r.ops.size();
    auto root=probe_roots.find(w.root);w.root_known=root!=probe_roots.end()&&root->second.complete;
    unsigned scanned=0;
    auto scan=[&](unsigned slot,unsigned kind,unsigned reg,unsigned space,unsigned offset,unsigned count){
        if(slot>=tables.size()||!tables[slot]||!h.inc||!h.gpu||tables[slot]<h.gpu)return;
        auto delta=tables[slot]-h.gpu;if(delta%h.inc)return;
        uint64_t index=delta/h.inc+offset;
        if(index>=h.count)return;
        unsigned limit=(unsigned)std::min<uint64_t>(std::min<uint64_t>(count,64),h.count-index);
        if(limit<count)w.truncated=true;
        for(unsigned j=0;j<limit;++j){
            if(scanned>=256){w.truncated=true;break;}++scanned;
            SIZE_T cpu=h.cpu+SIZE_T(index+j)*h.inc;uint64_t gpu=h.gpu+(index+j)*h.inc;
            auto v=probe_get_view(cpu);
            // Keep untracked slots too: reused lists can see descriptor updates.
            if(w.bindings.size()<256)w.bindings.push_back({cpu,gpu,slot,kind,reg+j,space,v});
        }
    };
    if(root!=probe_roots.end()){
        for(auto &v:root->second.ranges)scan(v.root,v.kind,v.reg,v.space,v.offset,v.count);
        if(!root->second.complete)w.truncated=true;
    }else{
        // No guessed register layout: report table base only.
        for(unsigned i=0;i<tables.size();++i)if(tables[i])scan(i,0,0,0,0,1);
        w.truncated=true;
    }
    if(compute){
        bool candidate=false;
        for(auto &v:w.bindings)if(v.at_record.kind==2&&probe_is_view_bb(v.at_record.identity)){candidate=true;break;}
        if(!candidate)return;
    }
    r.work.push_back(std::move(w));
}
void STDMETHODCALLTYPE probe_dispatch(ID3D12GraphicsCommandList *cl,UINT a,UINT b,UINT c){
    auto fn=original<decltype(&probe_dispatch)>(cl,14);
    {std::lock_guard<std::recursive_mutex> g(lock);if(uav_probe)probe_work(record(cl),"Dispatch",0,true,a,b,c);}fn(cl,a,b,c);
}
void probe_log_record(ID3D12CommandQueue *q,const Record &r,UINT list_index,uint64_t planned_submit,bool repeat,bool strict_record,uint64_t exec){
    if(probe_submit_logs>=4096)return;
    bool relevant=!r.work.empty();
    for(auto &o:r.ops)if(is_bb(o.dst)||probe_is_final(o.dst)||probe_is_final(o.src)){relevant=true;break;}
    if(!relevant)return;
    ++probe_submit_logs;uint64_t attempt=++probe_attempt;
    log("FG TRANSFER_RECORD attempt=%llu execute=%llu recording=%llu plannedSubmit=%llu queue=%p listIndex=%u generation=%llu known=%d repeatedOrUnknown=%d strictRecord=%d ops=%zu work=%zu descriptorOverflow=%d qpc=%lld",
        attempt,exec,r.id,strict_record?planned_submit:0,q,list_index,generation,r.known,repeat,strict_record,r.ops.size(),r.work.size(),probe_view_overflow,qpc());
    unsigned ordinal=0;
    for(auto &o:r.ops){
        ++ordinal;
        if(!is_bb(o.dst)&&!probe_is_final(o.dst)&&!probe_is_final(o.src))continue;
        log("FG TRANSFER_OP attempt=%llu recording=%llu ordinal=%u kind=%u method=%s dstCookie=%llu srcCookie=%llu packet=%llu a=%u b=%u c=%u qpc=%lld",
            attempt,r.id,ordinal,(unsigned)o.kind,o.reason?o.reason:"unknown",o.dst,o.src,o.packet,o.a,o.b,o.c,qpc());
    }
    for(auto &w:r.work){
        log("FG TRANSFER_WORK attempt=%llu recording=%llu ordinal=%u method=%s dstCookie=%llu pso=%p root=%p rootKnown=%d truncated=%d slots=%zu argc=%u,%u,%u shaderAccessProven=0 qpc=%lld",
            attempt,r.id,w.ordinal,w.method,w.dst,reinterpret_cast<void*>(w.pso),reinterpret_cast<void*>(w.root),w.root_known,w.truncated,w.bindings.size(),w.a,w.b,w.c,qpc());
        for(auto &b:w.bindings){
            auto current=probe_get_view(b.cpu);
            if(!b.at_record.identity&&!current.identity)continue;
            log("FG TRANSFER_BIND attempt=%llu recording=%llu ordinal=%u rootParam=%u rangeKind=%u reg=%u space=%u cpu=%p gpu=%llu recordIdentity=%llu submitIdentity=%llu recordView=%u submitView=%u submitFormat=%u boundOnly=1",
                attempt,r.id,w.ordinal,b.root,b.kind,b.reg,b.space,reinterpret_cast<void*>(b.cpu),b.gpu,b.at_record.identity,current.identity,b.at_record.kind,current.kind,current.format);
        }
    }
}
HRESULT STDMETHODCALLTYPE probe_signal(ID3D12CommandQueue *q,ID3D12Fence *f,UINT64 value){
    auto fn=original<decltype(&probe_signal)>(q,14);
    auto begin=qpc();auto hr=fn(q,f,value);auto end=qpc();
    std::lock_guard<std::recursive_mutex> g(lock);
    if(uav_probe&&!probe_internal_signal&&probe_sync_logs<12000&&gameplay_gate()){
        ++probe_sync_logs;auto id=probe_private_id(f,probe_fence_guid,probe_fence_serial);
        log("FG TRANSFER_SYNC method=Signal queue=%p fence=%p fenceId=%llu value=%llu hr=%08lx generation=%llu qpcBegin=%lld qpcEnd=%lld",q,f,id,value,(unsigned long)hr,generation,begin,end);
    }return hr;
}
HRESULT STDMETHODCALLTYPE probe_wait(ID3D12CommandQueue *q,ID3D12Fence *f,UINT64 value){
    auto fn=original<decltype(&probe_wait)>(q,15);
    auto begin=qpc();auto hr=fn(q,f,value);auto end=qpc();
    std::lock_guard<std::recursive_mutex> g(lock);
    if(uav_probe&&probe_sync_logs<12000&&gameplay_gate()){
        ++probe_sync_logs;auto id=probe_private_id(f,probe_fence_guid,probe_fence_serial);
        log("FG TRANSFER_SYNC method=Wait queue=%p fence=%p fenceId=%llu value=%llu hr=%08lx generation=%llu qpcBegin=%lld qpcEnd=%lld",q,f,id,value,(unsigned long)hr,generation,begin,end);
    }return hr;
}
void probe_list_hooks(ID3D12GraphicsCommandList *cl){
    // Failures affect diagnostic coverage only. Strict hooks remain intact.
    bool ok=true;
    ok=hook(cl,14,probe_dispatch)&&ok;ok=hook(cl,25,probe_pso)&&ok;ok=hook(cl,28,probe_heaps)&&ok;
    ok=hook(cl,29,probe_croot)&&ok;ok=hook(cl,30,probe_groot)&&ok;ok=hook(cl,31,probe_ctable)&&ok;ok=hook(cl,32,probe_gtable)&&ok;
    static unsigned reports=0;if(!ok&&reports++<8)log("FG TRANSFER_COVERAGE list=%p optionalHooks=0",cl);
}
void probe_device_hooks(ID3D12Device *d){
    bool heap=hook(d,14,probe_heap),root=hook(d,16,probe_root),cbv=hook(d,17,probe_cbv),srv=hook(d,18,probe_srv),uav=hook(d,19,probe_uav);
    log("FG TRANSFER_COVERAGE device=%p heap=%d root=%d cbv=%d srv=%d uav=%d lateHook=1",d,heap,root,cbv,srv,uav);
}
void probe_queue_hooks(ID3D12CommandQueue *q){
    bool signal=hook(q,14,probe_signal),wait=hook(q,15,probe_wait);
    static unsigned reports=0;if(reports++<12)log("FG TRANSFER_COVERAGE queue=%p signal=%d wait=%d",q,signal,wait);
}
