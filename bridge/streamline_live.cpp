// FH2 Streamline 2.14.1 live HDR alpha. No second Present injection.
// The native queue and list hooks below only prove full texture-copy
// lineage; unknown shader transfers/writes deliberately cannot arm FG.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <array>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
#define AZEXPORT extern "C" __declspec(dllexport)
namespace {
constexpr unsigned slots=12, rw=2592,rh=1080,ow=3840,oh=1600;
constexpr GUID unwrap_guid={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
constexpr GUID cookie_guid={0x5505b313,0x928e,0x4d53,{0x85,0x42,0x01,0x47,0x6a,0xf2,0x73,0x50}};
template<class T> ComPtr<T> native(IUnknown *o){
    ComPtr<T> r;ComPtr<IUnknown> u;
    if(!o)return r;
    HRESULT hr=o->QueryInterface(unwrap_guid,reinterpret_cast<void**>(u.GetAddressOf()));
    if(hr==E_NOINTERFACE)o->QueryInterface(IID_PPV_ARGS(&r));
    else if(SUCCEEDED(hr)&&u)u.As(&r);
    return r;
}
template<class T> T *entry(HMODULE m,const char *n){return reinterpret_cast<T*>(GetProcAddress(m,n));}
template<class T> T *feature(PFun_slGetFeatureFunction *f,sl::Feature id,const char *n){
    void *v=nullptr;return f(id,n,v)==sl::Result::eOk?reinterpret_cast<T*>(v):nullptr;
}
std::recursive_mutex lock;
std::recursive_mutex submit_lock;
FILE *logfile=nullptr;
long long qpc(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
void log(const char *fmt,...){
    if(!logfile)return;
    va_list a;va_start(a,fmt);std::vfprintf(logfile,fmt,a);va_end(a);
    std::fputc('\n',logfile);std::fflush(logfile);
}
bool env(const wchar_t *n){wchar_t s[2];return GetEnvironmentVariableW(n,s,2)==1&&s[0]==L'1';}
bool gameplay_gate(){wchar_t p[1024];DWORD n=GetEnvironmentVariableW(L"FH2_STREAMLINE_ACTIVE_FILE",p,1024);return n&&n<1024&&GetFileAttributesW(p)!=INVALID_FILE_ATTRIBUTES;}
ComPtr<ID3D12Device> device;
ComPtr<ID3D12CommandQueue> present_queue;
IDXGISwapChain1 *managed=nullptr; // borrowed identity only, no permanent chain/buffer ref
PFun_slGetNativeInterface *get_native=nullptr;
PFun_slGetNewFrameToken *new_token=nullptr;
PFun_slSetConstants *set_constants=nullptr;
PFun_slSetTagForFrame *set_tag=nullptr;
PFun_slPCLSetMarker *marker=nullptr;
PFun_slDLSSGSetOptions *set_options=nullptr;
PFun_slDLSSGGetState *get_state=nullptr;
PFun_slReflexSetOptions *reflex_options=nullptr;
sl::DLSSGOptions options;
bool initialized=false, enabled=false, armed=false, hdr=false, shape=false, fatal=false;
uint64_t generation=1, serial=0, last_resolve=0, last_generation=0, last_tick=0;
uint64_t cookie_serial=0, recording_serial=0, submission_serial=0;
uint32_t token_serial=0;
struct Fence {
    ComPtr<ID3D12Fence> f;uint64_t value=0;
    bool done()const{if(!f)return false;auto v=f->GetCompletedValue();return v!=UINT64_MAX&&v>=value;}
    explicit operator bool()const{return f&&value;}
};
struct Stage {
    uint64_t record=0,game_record=0; bool retired=false,submitted=false;
    ID3D12CommandQueue *queue=nullptr;Fence fence;
    bool safe()const{return !record||(retired&&(!submitted||fence.done()));}
};
struct Packet {
    uint64_t id=0,resolve=0,seq=0,gen=0;
    bool reset=false,bad=false,presented=false,tagged=false,quarantined=false;
    bool guide_initialized=false,core_initialized=false,hud_initialized=false;
    Stage camera,core,final;
    Fence consumption;
    ComPtr<ID3D12Resource> camera_tex,camera_rb,depth,mv,hud;
    ComPtr<ID3D12DescriptorHeap> camera_heap;
    sl::FrameToken *token=nullptr;
};
std::array<Packet,slots> ring{};
Packet *find_packet(uint64_t id){
    if(!id)return nullptr;for(auto &p:ring)if(p.id==id)return &p;return nullptr;
}
void off(const char *reason){
    if(armed&&set_options){options.mode=sl::DLSSGMode::eOff;
        auto r=set_options(sl::ViewportHandle(0),options);
        log("FG OFF reason=%s result=%u",reason,(unsigned)r);
        if(r!=sl::Result::eOk)fatal=true;
    }
    armed=false;last_resolve=last_generation=last_tick=0;
}
void reset_metadata(Packet &p){
    p.id=p.resolve=p.seq=p.gen=0;p.reset=p.bad=p.presented=p.tagged=p.quarantined=false;
    p.camera={};p.core={};p.final={};p.consumption={};p.token=nullptr;
}
Packet *allocate(uint64_t resolve,uint64_t seq){
    for(auto &p:ring){
        if(p.id&&!p.quarantined&&p.camera.safe()&&p.core.safe()&&p.final.safe()&&
          (!p.tagged||(!p.quarantined&&p.consumption.done()))&&
          (p.bad||p.presented||p.gen!=generation||resolve>p.resolve+slots)){
            reset_metadata(p);
        }
        if(!p.id){p.id=++serial;p.resolve=resolve;p.seq=seq;p.gen=generation;return &p;}
    }
    log("FG SKIP reason=ring_busy resolve=%llu",(unsigned long long)resolve);return nullptr;
}
Packet *packet_for(uint64_t resolve,uint64_t seq){
    Packet *match=nullptr;
    for(auto &p:ring)if(p.id&&!p.bad&&!p.presented&&p.resolve==resolve&&p.seq==seq&&p.gen==generation){
        if(match)return nullptr;match=&p;
    }
    return match;
}
Packet *final_packet(uint64_t resolve){
    Packet *match=nullptr;
    for(auto &p:ring)if(p.id&&!p.bad&&!p.presented&&p.resolve==resolve&&p.gen==generation&&p.core.record){
        if(match)return nullptr;match=&p;
    }
    return match;
}
bool rgb10(ID3D12Resource *r){
    if(!r)return false;auto d=r->GetDesc();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==ow&&d.Height==oh&&
      d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1&&d.Format==DXGI_FORMAT_R10G10B10A2_UNORM&&!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
}
uint64_t cookie(ID3D12Resource *r){
    if(!rgb10(r))return 0;uint64_t v=0;UINT n=sizeof(v);
    if(SUCCEEDED(r->GetPrivateData(cookie_guid,&n,&v))&&n==sizeof(v)&&v)return v;
    v=++cookie_serial;return SUCCEEDED(r->SetPrivateData(cookie_guid,sizeof(v),&v))?v:0;
}
enum Kind { invalid,copy,cam,core,final };
struct Op {Kind kind;uint64_t dst=0,src=0,packet=0;};
struct Record {uint64_t id=0;bool known=false,submitted=false;std::vector<Op> ops;std::array<uint64_t,8> rt{};};
struct Provenance {uint64_t packet=0,record=0,submit=0,gen=0;ID3D12CommandQueue *queue=nullptr;Fence fence;};
std::unordered_map<ID3D12GraphicsCommandList*,Record> records;
std::unordered_map<uint64_t,Provenance> lineage;
std::unordered_map<SIZE_T,uint64_t> rtv_map;
std::array<uint64_t,3> bb{};
bool is_bb(uint64_t c){return c&&std::find(bb.begin(),bb.end(),c)!=bb.end();}
Record &record(ID3D12GraphicsCommandList *cl){
    auto &r=records[cl];if(!r.id)r.id=++recording_serial;return r;
}
void add(ID3D12GraphicsCommandList *cl,Op op){
    if(!cl||cl->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
    auto &r=record(cl);
    if(r.ops.size()>=4096){fatal=true;return;}
    r.ops.push_back(op);
}
void invalidate(ID3D12GraphicsCommandList *cl,uint64_t dst){if(dst)add(cl,{invalid,dst,0,0});}
void retire(ID3D12GraphicsCommandList *cl){
    auto &r=record(cl);
    for(auto &p:ring)for(auto *s:{&p.camera,&p.core,&p.final})
        if(s->record==r.id){s->retired=true;if(!s->submitted){p.bad=true;if(s==&p.core){p.core_initialized=false;p.depth.Reset();p.mv.Reset();}if(s==&p.final){p.hud_initialized=false;p.hud.Reset();}}}
    r={};r.id=++recording_serial;r.known=true;
}
struct Patch{void **slot;void *original;};
std::vector<Patch> patches;
void **vslot(void *o,unsigned n){return &(*reinterpret_cast<void***>(o))[n];}
template<class F> F original(void *o,unsigned n){
    std::lock_guard<std::recursive_mutex> g(lock);
    auto *s=vslot(o,n);for(auto &p:patches)if(p.slot==s)return reinterpret_cast<F>(p.original);
    // An installed hook must always have its original.
    std::abort();
}
bool patch(void *o,unsigned n,void *fn){
    auto *s=vslot(o,n);
    for(auto &p:patches)if(p.slot==s)return *s==fn;
    if(!*s||*s==fn||patches.size()>=128)return false;
    DWORD old=0,tmp=0;if(!VirtualProtect(s,sizeof(void*),PAGE_EXECUTE_READWRITE,&old))return false;
    patches.push_back({s,*s});InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(s),fn);
    VirtualProtect(s,sizeof(void*),old,&tmp);return true;
}
template<class T> bool hook(void *o,unsigned n,T fn){return patch(o,n,reinterpret_cast<void*>(fn));}
void STDMETHODCALLTYPE copy_resource(ID3D12GraphicsCommandList *cl,ID3D12Resource *d,ID3D12Resource *s){
    auto fn=original<decltype(&copy_resource)>(cl,17);
    {std::lock_guard<std::recursive_mutex> g(lock);uint64_t dst=cookie(d),src=cookie(s);
     if(dst)add(cl,{src&&src!=dst?copy:invalid,dst,src,0});}
    fn(cl,d,s);
}
void STDMETHODCALLTYPE copy_region(ID3D12GraphicsCommandList *cl,const D3D12_TEXTURE_COPY_LOCATION *d,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION *s,const D3D12_BOX *b){
    auto fn=original<decltype(&copy_region)>(cl,16);
    {std::lock_guard<std::recursive_mutex> g(lock);
     uint64_t dst=d?cookie(d->pResource):0,src=s?cookie(s->pResource):0;
     bool full=dst&&src&&dst!=src&&x==0&&y==0&&z==0&&
       d->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&s->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&
       d->SubresourceIndex==0&&s->SubresourceIndex==0&&(!b||(b->left==0&&b->top==0&&b->front==0&&b->right==ow&&b->bottom==oh&&b->back==1));
     if(dst)add(cl,{full?copy:invalid,dst,src,0});}
    fn(cl,d,x,y,z,s,b);
}
HRESULT STDMETHODCALLTYPE reset_list(ID3D12GraphicsCommandList *cl,ID3D12CommandAllocator *a,ID3D12PipelineState *p){
    auto fn=original<decltype(&reset_list)>(cl,10);auto hr=fn(cl,a,p);
    if(SUCCEEDED(hr)){std::lock_guard<std::recursive_mutex> g(lock);retire(cl);}return hr;
}
void STDMETHODCALLTYPE clear_state(ID3D12GraphicsCommandList *cl,ID3D12PipelineState *p){
    auto fn=original<decltype(&clear_state)>(cl,11);
    {std::lock_guard<std::recursive_mutex> g(lock);record(cl).rt={};}fn(cl,p);
}
void STDMETHODCALLTYPE barriers(ID3D12GraphicsCommandList *cl,UINT n,const D3D12_RESOURCE_BARRIER *b){
    auto fn=original<decltype(&barriers)>(cl,26);
    {std::lock_guard<std::recursive_mutex> g(lock);
     for(UINT i=0;b&&i<n;++i){
        if(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&
           (b[i].Transition.StateAfter&(D3D12_RESOURCE_STATE_RENDER_TARGET|D3D12_RESOURCE_STATE_COPY_DEST|D3D12_RESOURCE_STATE_UNORDERED_ACCESS|D3D12_RESOURCE_STATE_RESOLVE_DEST)))
             invalidate(cl,cookie(b[i].Transition.pResource));
        else if(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){
             invalidate(cl,cookie(b[i].Aliasing.pResourceBefore));invalidate(cl,cookie(b[i].Aliasing.pResourceAfter));}
     }}
    fn(cl,n,b);
}
void STDMETHODCALLTYPE create_rtv(ID3D12Device *d,ID3D12Resource *r,const D3D12_RENDER_TARGET_VIEW_DESC *desc,D3D12_CPU_DESCRIPTOR_HANDLE h){
    auto fn=original<decltype(&create_rtv)>(d,20);fn(d,r,desc,h);
    std::lock_guard<std::recursive_mutex> g(lock);rtv_map[h.ptr]=cookie(r);
}
void STDMETHODCALLTYPE copy_desc_simple(ID3D12Device *d,UINT n,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE type){
    auto fn=original<decltype(&copy_desc_simple)>(d,24);fn(d,n,dst,src,type);
    if(type!=D3D12_DESCRIPTOR_HEAP_TYPE_RTV)return;
    std::lock_guard<std::recursive_mutex> g(lock);UINT inc=d->GetDescriptorHandleIncrementSize(type);
    for(UINT i=0;i<n;++i)rtv_map[dst.ptr+SIZE_T(i)*inc]=rtv_map[src.ptr+SIZE_T(i)*inc];
}
void STDMETHODCALLTYPE copy_desc(ID3D12Device *d,UINT nd,const D3D12_CPU_DESCRIPTOR_HANDLE *ds,const UINT *dn,UINT ns,const D3D12_CPU_DESCRIPTOR_HANDLE *ss,const UINT *sn,D3D12_DESCRIPTOR_HEAP_TYPE type){
    auto fn=original<decltype(&copy_desc)>(d,23);fn(d,nd,ds,dn,ns,ss,sn,type);
    if(type!=D3D12_DESCRIPTOR_HEAP_TYPE_RTV)return;
    std::lock_guard<std::recursive_mutex> g(lock);UINT inc=d->GetDescriptorHandleIncrementSize(type),a=0,b=0;
    std::vector<uint64_t> values;
    for(UINT i=0;i<ns;++i)for(UINT j=0;j<(sn?sn[i]:1);++j)values.push_back(rtv_map[ss[i].ptr+SIZE_T(j)*inc]);
    for(UINT i=0;i<nd;++i)for(UINT j=0;j<(dn?dn[i]:1);++j){rtv_map[ds[i].ptr+SIZE_T(j)*inc]=a<values.size()?values[a]:0;++a;}
}
void STDMETHODCALLTYPE set_rt(ID3D12GraphicsCommandList *cl,UINT n,const D3D12_CPU_DESCRIPTOR_HANDLE *rt,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE *ds){
    auto fn=original<decltype(&set_rt)>(cl,46);
    {std::lock_guard<std::recursive_mutex> g(lock);auto &r=record(cl);r.rt={};
     UINT inc=device?device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV):0;
     for(UINT i=0;rt&&i<n&&i<r.rt.size();++i){auto h=contiguous?rt[0].ptr+SIZE_T(i)*inc:rt[i].ptr;
        auto it=rtv_map.find(h);if(it!=rtv_map.end())r.rt[i]=it->second;}}
    fn(cl,n,rt,contiguous,ds);
}
void draw_writes(ID3D12GraphicsCommandList *cl){auto &r=record(cl);for(auto id:r.rt)invalidate(cl,id);}
void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList *cl,UINT a,UINT b,UINT c,UINT d){
    auto fn=original<decltype(&draw)>(cl,12);
    {std::lock_guard<std::recursive_mutex> g(lock);draw_writes(cl);}fn(cl,a,b,c,d);
}
void STDMETHODCALLTYPE draw_indexed(ID3D12GraphicsCommandList *cl,UINT a,UINT b,UINT c,INT d,UINT e){
    auto fn=original<decltype(&draw_indexed)>(cl,13);
    {std::lock_guard<std::recursive_mutex> g(lock);draw_writes(cl);}fn(cl,a,b,c,d,e);
}
void STDMETHODCALLTYPE clear_rtv(ID3D12GraphicsCommandList *cl,D3D12_CPU_DESCRIPTOR_HANDLE h,const FLOAT col[4],UINT n,const D3D12_RECT *r){
    auto fn=original<decltype(&clear_rtv)>(cl,48);
    {std::lock_guard<std::recursive_mutex> g(lock);auto i=rtv_map.find(h.ptr);if(i!=rtv_map.end())invalidate(cl,i->second);}fn(cl,h,col,n,r);
}
void STDMETHODCALLTYPE resolve_sub(ID3D12GraphicsCommandList *cl,ID3D12Resource *d,UINT di,ID3D12Resource *s,UINT si,DXGI_FORMAT f){
    auto fn=original<decltype(&resolve_sub)>(cl,19);
    {std::lock_guard<std::recursive_mutex> g(lock);invalidate(cl,cookie(d));}fn(cl,d,di,s,si,f);
}
void STDMETHODCALLTYPE discard(ID3D12GraphicsCommandList *cl,ID3D12Resource *r,const D3D12_DISCARD_REGION *reg){
    auto fn=original<decltype(&discard)>(cl,51);
    {std::lock_guard<std::recursive_mutex> g(lock);invalidate(cl,cookie(r));}fn(cl,r,reg);
}
void STDMETHODCALLTYPE bundle(ID3D12GraphicsCommandList *cl,ID3D12GraphicsCommandList *b){
    auto fn=original<decltype(&bundle)>(cl,27);
    {std::lock_guard<std::recursive_mutex> g(lock);draw_writes(cl);}fn(cl,b);
}
void STDMETHODCALLTYPE clear_uav_u(ID3D12GraphicsCommandList *cl,D3D12_GPU_DESCRIPTOR_HANDLE g,D3D12_CPU_DESCRIPTOR_HANDLE c,ID3D12Resource *r,const UINT a[4],UINT n,const D3D12_RECT *rect){
    auto fn=original<decltype(&clear_uav_u)>(cl,49);
    {std::lock_guard<std::recursive_mutex> l(lock);invalidate(cl,cookie(r));}fn(cl,g,c,r,a,n,rect);
}
void STDMETHODCALLTYPE clear_uav_f(ID3D12GraphicsCommandList *cl,D3D12_GPU_DESCRIPTOR_HANDLE g,D3D12_CPU_DESCRIPTOR_HANDLE c,ID3D12Resource *r,const FLOAT a[4],UINT n,const D3D12_RECT *rect){
    auto fn=original<decltype(&clear_uav_f)>(cl,50);
    {std::lock_guard<std::recursive_mutex> l(lock);invalidate(cl,cookie(r));}fn(cl,g,c,r,a,n,rect);
}
bool hook_list(ID3D12GraphicsCommandList *cl){
    return hook(cl,10,reset_list)&&hook(cl,11,clear_state)&&hook(cl,12,draw)&&hook(cl,13,draw_indexed)&&
      hook(cl,16,copy_region)&&hook(cl,17,copy_resource)&&hook(cl,19,resolve_sub)&&hook(cl,26,barriers)&&
      hook(cl,27,bundle)&&hook(cl,46,set_rt)&&hook(cl,48,clear_rtv)&&hook(cl,49,clear_uav_u)&&hook(cl,50,clear_uav_f)&&hook(cl,51,discard);
}
Stage *stage(Packet &p,Kind k){return k==cam?&p.camera:k==core?&p.core:k==final?&p.final:nullptr;}
Fence signal(ID3D12CommandQueue *q){
    Fence f;if(!device||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f.f))))return {};
    f.value=1;if(FAILED(q->Signal(f.f.Get(),f.value)))return {};return f;
}
bool wait_submitted(ID3D12CommandQueue *q,const Fence &f,ID3D12CommandQueue *producer){
    if(!f)return false;if(f.done()||q==producer)return true;
    return SUCCEEDED(q->Wait(f.f.Get(),f.value));
}
// Process the *actual* native Execute order, not record time or QPC proximity.
// For a cross-queue copy of proven data, enqueue a wait on the already
// submitted producer fence before forwarding the native list.
void STDMETHODCALLTYPE execute(ID3D12CommandQueue *q,UINT n,ID3D12CommandList *const *ls){
    auto fn=original<decltype(&execute)>(q,10);
    std::lock_guard<std::recursive_mutex> order(submit_lock);
    std::unique_lock<std::recursive_mutex> guard(lock);
    if(q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT){guard.unlock();fn(q,n,ls);return;}
    std::vector<Record*> used;
    std::vector<std::pair<uint64_t,Op>> ops;
    for(UINT i=0;ls&&i<n;++i){
        auto it=records.find(reinterpret_cast<ID3D12GraphicsCommandList*>(ls[i]));
        if(it==records.end()||it->second.ops.empty())continue;
        auto &r=it->second;
        bool repeat=r.submitted||!r.known||std::find(used.begin(),used.end(),&r)!=used.end();
        if(repeat){
            for(auto &o:r.ops){if(auto *p=find_packet(o.packet))p->bad=true;if(o.dst)lineage.erase(o.dst);}
        }
        for(auto o:r.ops){if(repeat){if(o.dst)ops.push_back({r.id,{invalid,o.dst,0,0}});}
            else ops.push_back({r.id,o});}
        used.push_back(&r);
    }
    if(used.empty()){guard.unlock();fn(q,n,ls);return;}
    uint64_t sub=++submission_serial;
    // The same native submission may contain both the final draw and copy.
    // New provenance has no fence until this Execute has actually forwarded.
    for(auto &[rec,o]:ops){
        if(o.kind==invalid){bool had=lineage.erase(o.dst)!=0;if(is_bb(o.dst))log("FG INVALIDATE bbCookie=%llu recording=%llu submit=%llu queue=%p generation=%llu hadLineage=%d",o.dst,rec,sub,q,generation,had);continue;}
        if(o.kind==copy){
            auto it=lineage.find(o.src);
            Provenance v=it==lineage.end()?Provenance{}:it->second;
            lineage.erase(o.dst);
            auto *p=find_packet(v.packet);
            bool valid=p&&!p->bad&&!p->presented&&v.gen==generation&&
              (v.submit==sub||wait_submitted(q,v.fence,v.queue));
            if(valid){v.record=rec;v.submit=sub;v.queue=q;v.fence={};lineage[o.dst]=v;}
            if(valid||is_bb(o.dst))log("FG COPY resolve=%llu srcCookie=%llu dstCookie=%llu nativeRecording=%llu submit=%llu queue=%p generation=%llu proven=%d",
                valid?p->resolve:0,o.src,o.dst,rec,sub,q,generation,valid);
            continue;
        }
        auto *p=find_packet(o.packet);if(!p)continue;
        auto *s=stage(*p,o.kind);
        if(!s||s->record!=rec||s->submitted||p->gen!=generation){p->bad=true;continue;}
        s->queue=q;s->submitted=true;
        if(o.kind==final&&!p->bad)lineage[o.dst]={p->id,rec,sub,generation,q,{}};
        if(o.kind==core&&p->token)marker(sl::PCLMarker::eRenderSubmitStart,*p->token);
        log("FG STAGE_SUBMIT stage=%u resolve=%llu packet=%llu gameRecording=%llu nativeRecording=%llu submit=%llu queue=%p generation=%llu",unsigned(o.kind),p->resolve,p->id,s->game_record,rec,sub,q,generation);
    }
    // Never hold the lineage lock while forwarding into another proxy.
    guard.unlock();fn(q,n,ls);guard.lock();
    for(auto *r:used)r->submitted=true;
    Fence f=signal(q);
    for(auto &[rec,o]:ops)if(auto *p=find_packet(o.packet)){
        if(auto *s=stage(*p,o.kind)){s->fence=f;if(!f){p->bad=true;p->quarantined=true;}
            if(o.kind==final&&p->token)marker(sl::PCLMarker::eRenderSubmitEnd,*p->token);}
    }
    // Repeated submissions are never accepted, but still fence their
    // private allocations; do not free an in-flight reused recording.
    for(auto *r:used)for(auto &o:r->ops)if(auto *p=find_packet(o.packet)){
        if(auto *s=stage(*p,o.kind)){s->fence=f;if(!f)p->quarantined=true;}
    }
    for(auto it=lineage.begin();it!=lineage.end();){
        auto &v=it->second;auto *p=find_packet(v.packet);
        if(!p||p->bad||p->presented||v.gen!=generation||(v.submit==sub&&!f))it=lineage.erase(it);
        else{if(v.submit==sub)v.fence=f;++it;}
    }
    if(lineage.size()>512){lineage.clear();fatal=true;}
}
bool ensure_hooks(){
    if(!device||!present_queue)return false;
    ComPtr<ID3D12CommandAllocator> a;ComPtr<ID3D12GraphicsCommandList> cl;
    if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)))||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&cl)))||FAILED(cl->Close()))return false;
    return hook(device.Get(),20,create_rtv)&&hook(device.Get(),23,copy_desc)&&hook(device.Get(),24,copy_desc_simple)&&hook_list(cl.Get())&&hook(present_queue.Get(),10,execute);
}
bool make_texture(ID3D12Device *d,ComPtr<ID3D12Resource> &r,unsigned w,unsigned h,DXGI_FORMAT fmt,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
    if(r)return true;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=fmt;desc.SampleDesc.Count=1;desc.Flags=flags;
    return SUCCEEDED(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)));
}
void transition(ID3D12GraphicsCommandList *cl,ID3D12Resource *r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    if(before==after)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};cl->ResourceBarrier(1,&b);
}
bool clone(ID3D12GraphicsCommandList *cl,ID3D12Resource *src,ComPtr<ID3D12Resource> &dst,unsigned w,unsigned h,DXGI_FORMAT fmt,D3D12_RESOURCE_STATES state,bool initialized){
    if(!src)return false;auto d=src->GetDesc();
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=w||d.Height!=h||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||
      !(d.Format==fmt||(fmt==DXGI_FORMAT_R32_FLOAT&&d.Format==DXGI_FORMAT_R32_TYPELESS)))return false;
    if(!make_texture(device.Get(),dst,w,h,fmt,D3D12_RESOURCE_STATE_COPY_DEST))return false;
    if(initialized)transition(cl,dst.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    transition(cl,src,state,D3D12_RESOURCE_STATE_COPY_SOURCE);cl->CopyResource(dst.Get(),src);
    transition(cl,src,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
    transition(cl,dst.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);return true;
}
bool chain_info(IDXGISwapChain1 *sc){
    if(sc!=managed)return false;ComPtr<IDXGISwapChain3> s;
    DXGI_SWAP_CHAIN_DESC1 d{};
    shape=SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s)))&&SUCCEEDED(s->GetDesc1(&d))&&d.Width==ow&&d.Height==oh&&d.Format==DXGI_FORMAT_R10G10B10A2_UNORM&&d.BufferCount==3;
    bb={};if(shape)for(unsigned i=0;i<3;++i){ComPtr<ID3D12Resource> b;if(SUCCEEDED(s->GetBuffer(i,IID_PPV_ARGS(&b))))bb[i]=cookie(b.Get());if(!bb[i])shape=false;}
    return shape;
}
#include "streamline_camera.inl"
struct Active {uint64_t packet=0,call=0,gen=0;uint32_t index=0;bool reset=false;};
thread_local Active active;
}
AZEXPORT HRESULT __cdecl azslCreateManagedHdrChain(IDXGIFactory2 *factory,IUnknown *queue_arg,HWND window,const DXGI_SWAP_CHAIN_DESC1 *desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,IDXGIOutput *out,IDXGISwapChain1 **result){
    std::lock_guard<std::recursive_mutex> guard(lock);
    if(!factory||!queue_arg||!desc||!result||managed)return E_INVALIDARG;
    *result=nullptr;
    auto q=native<ID3D12CommandQueue>(queue_arg);
    if(!q||q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
    ComPtr<ID3D12Device> wrapped;if(FAILED(q->GetDevice(IID_PPV_ARGS(&wrapped))))return E_INVALIDARG;
    auto dev=native<ID3D12Device>(wrapped.Get());if(!dev)return E_INVALIDARG;
    static wchar_t runtime[1024],logs[1024],path[1100];static const wchar_t *paths[]={runtime};
    DWORD a=GetEnvironmentVariableW(L"FH2_STREAMLINE_RUNTIME",runtime,1024),b=GetEnvironmentVariableW(L"FH2_STREAMLINE_LOGS",logs,1024);
    if(!a||a>=1024||!b||b>=1024||swprintf_s(path,L"%s\\sl.interposer.dll",runtime)<0)return E_INVALIDARG;
    HMODULE m=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!m)return HRESULT_FROM_WIN32(GetLastError());
    if(swprintf_s(path,L"%s\\fg-live.log",logs)>=0)_wfopen_s(&logfile,path,L"wt");
    auto init=entry<PFun_slInit>(m,"slInit");auto supported=entry<PFun_slIsFeatureSupported>(m,"slIsFeatureSupported");
    auto set_device=entry<PFun_slSetD3DDevice>(m,"slSetD3DDevice");auto get_fn=entry<PFun_slGetFeatureFunction>(m,"slGetFeatureFunction");
    auto upgrade=entry<PFun_slUpgradeInterface>(m,"slUpgradeInterface");get_native=entry<PFun_slGetNativeInterface>(m,"slGetNativeInterface");
    if(!init||!supported||!set_device||!get_fn||!upgrade||!get_native)return E_NOINTERFACE;
    const sl::Feature features[]={sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
    sl::Preferences prefs;prefs.flags=sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    prefs.featuresToLoad=features;prefs.numFeaturesToLoad=_countof(features);prefs.pathsToPlugins=paths;prefs.numPathsToPlugins=1;
    prefs.pathToLogsAndData=logs;prefs.engine=sl::EngineType::eCustom;prefs.engineVersion="1.0.0";prefs.projectId="24afd02c-9d57-443b-ad60-ea52556756a9";
    if(init(prefs,sl::kSDKVersion)!=sl::Result::eOk)return E_ABORT;
    LUID luid=dev->GetAdapterLuid();sl::AdapterInfo adapter;adapter.deviceLUID=reinterpret_cast<uint8_t*>(&luid);adapter.deviceLUIDSizeInBytes=sizeof(luid);
    for(auto f:features)if(supported(f,adapter)!=sl::Result::eOk)return DXGI_ERROR_UNSUPPORTED;
    if(set_device(dev.Get())!=sl::Result::eOk)return E_ABORT;
    set_options=feature<PFun_slDLSSGSetOptions>(get_fn,sl::kFeatureDLSS_G,"slDLSSGSetOptions");
    get_state=feature<PFun_slDLSSGGetState>(get_fn,sl::kFeatureDLSS_G,"slDLSSGGetState");
    marker=feature<PFun_slPCLSetMarker>(get_fn,sl::kFeaturePCL,"slPCLSetMarker");
    reflex_options=feature<PFun_slReflexSetOptions>(get_fn,sl::kFeatureReflex,"slReflexSetOptions");
    new_token=entry<PFun_slGetNewFrameToken>(m,"slGetNewFrameToken");set_constants=entry<PFun_slSetConstants>(m,"slSetConstants");set_tag=entry<PFun_slSetTagForFrame>(m,"slSetTagForFrame");
    if(!set_options||!get_state||!marker||!reflex_options||!new_token||!set_constants||!set_tag)return E_NOINTERFACE;
    options.mode=sl::DLSSGMode::eOff;options.numFramesToGenerate=1;options.numBackBuffers=3;
    options.mvecDepthWidth=rw;options.mvecDepthHeight=rh;options.colorWidth=ow;options.colorHeight=oh;
    options.colorBufferFormat=options.hudLessBufferFormat=DXGI_FORMAT_R10G10B10A2_UNORM;
    options.depthBufferFormat=DXGI_FORMAT_R32_FLOAT;options.mvecBufferFormat=DXGI_FORMAT_R16G16_FLOAT;
    options.queueParallelismMode=sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    if(set_options(sl::ViewportHandle(0),options)!=sl::Result::eOk)return E_ABORT;
    sl::ReflexOptions reflex;reflex.mode=sl::ReflexMode::eLowLatency;
    if(reflex_options(reflex)!=sl::Result::eOk)return E_ABORT;
    void *fp=factory;if(upgrade(&fp)!=sl::Result::eOk||!fp||fp==factory)return E_NOINTERFACE;
    ComPtr<IDXGIFactory2> proxy_factory;proxy_factory.Attach(static_cast<IDXGIFactory2*>(fp));
    ComPtr<IDXGISwapChain1> sc;HRESULT hr=proxy_factory->CreateSwapChainForHwnd(q.Get(),window,desc,fs,out,&sc);
    if(FAILED(hr))return hr;
    ComPtr<IUnknown> base;
    if(get_native(sc.Get(),reinterpret_cast<void**>(base.GetAddressOf()))!=sl::Result::eOk||!base||base.Get()==sc.Get())return E_NOINTERFACE;
    device=dev;present_queue=q;managed=sc.Get();enabled=env(L"FH2_STREAMLINE_LIVE_FG");
    initialized=ensure_hooks();if(!initialized)enabled=false;
    chain_info(managed);
    log("FG OWNER managed=%p physical=%p queue=%p device=%p hooks=%d requested=%d generation=%llu",managed,base.Get(),present_queue.Get(),device.Get(),initialized,enabled,generation);
    *result=sc.Detach();return S_OK;
}
AZEXPORT void __cdecl azslRegisterQueue(ID3D12CommandQueue *wrapper){
    if(!wrapper)return;
    std::lock_guard<std::recursive_mutex> g(lock);
    if(!initialized||fatal)return;
    auto q=native<ID3D12CommandQueue>(wrapper);
    if(!q||q.Get()==wrapper||q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
    if(!patch(q.Get(),10,reinterpret_cast<void*>(&execute))){fatal=true;log("FG FATAL native_queue_hook_failed queue=%p",q.Get());}
}
AZEXPORT int __cdecl azslReady(){std::lock_guard<std::recursive_mutex> g(lock);return initialized&&!fatal&&hdr&&shape;}
AZEXPORT int __cdecl azslNativeChain(IDXGISwapChain1 *sc,IDXGISwapChain1 **out){
    std::lock_guard<std::recursive_mutex> g(lock);if(!out||!get_native||sc!=managed)return 0;*out=nullptr;
    ComPtr<IUnknown> base;if(get_native(sc,reinterpret_cast<void**>(base.GetAddressOf()))!=sl::Result::eOk||!base)return 0;
    return SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(out)));
}
AZEXPORT void __cdecl azslChainChange(IDXGISwapChain1 *sc,int action,unsigned color,HRESULT hr){
    std::lock_guard<std::recursive_mutex> g(lock);if(sc!=managed)return;
    if(action==0){off("resize");++generation;lineage.clear();bb={};shape=false;}
    else if(action==1){if(SUCCEEDED(hr))chain_info(sc);}
    else if(action==2&&SUCCEEDED(hr)){bool pq=color==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        if(pq!=hdr){off("colorspace");++generation;lineage.clear();hdr=pq;}chain_info(sc);}
    log("FG CHAIN action=%d hr=%08lx generation=%llu pq=%d shape=%d",action,(unsigned long)hr,generation,hdr,shape);
}
AZEXPORT uint64_t __cdecl azslCameraBegin(ID3D12Device *wrapper,ID3D12GraphicsCommandList *cl,uint64_t resolve,uint64_t recording,uint64_t seq,D3D12_CPU_DESCRIPTOR_HANDLE *rt){
    std::lock_guard<std::recursive_mutex> g(lock);
    if(!initialized||fatal||!hdr||!shape||!rt||!resolve||!recording||!wrapper||!gameplay_gate())return 0;
    auto n=native<ID3D12GraphicsCommandList>(cl);
    if(!n||n.Get()==cl||!hook_list(n.Get()))return 0;
    auto &r=record(n.Get());if(!r.known||r.submitted)return 0;
    Packet *p=allocate(resolve,seq);if(!p)return 0;
    if(new_token(p->token,&(++token_serial))!=sl::Result::eOk||!p->token){p->bad=true;return 0;}
    if(!make_texture(wrapper,p->camera_tex,64,1,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)){p->bad=true;return 0;}
    if(!p->camera_heap){D3D12_DESCRIPTOR_HEAP_DESC d{};d.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;d.NumDescriptors=1;
        if(FAILED(wrapper->CreateDescriptorHeap(&d,IID_PPV_ARGS(&p->camera_heap)))){p->bad=true;return 0;}
        wrapper->CreateRenderTargetView(p->camera_tex.Get(),nullptr,p->camera_heap->GetCPUDescriptorHandleForHeapStart());}
    if(!p->camera_rb){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=256;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;
        d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&p->camera_rb)))){p->bad=true;return 0;}
    }
    p->camera.record=r.id;p->camera.game_record=recording;*rt=p->camera_heap->GetCPUDescriptorHandleForHeapStart();return p->id;
}
AZEXPORT void __cdecl azslCameraEnd(ID3D12GraphicsCommandList *cl,uint64_t id){
    std::lock_guard<std::recursive_mutex> g(lock);auto *p=find_packet(id);auto n=native<ID3D12GraphicsCommandList>(cl);
    if(!p||!n||record(n.Get()).id!=p->camera.record)return;
    auto tex=native<ID3D12Resource>(p->camera_tex.Get());if(!tex){p->bad=true;return;}
    transition(n.Get(),tex.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION s{},d{};s.pResource=tex.Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.pResource=p->camera_rb.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,64,1,1,256};
    n->CopyTextureRegion(&d,0,0,0,&s,nullptr);transition(n.Get(),tex.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    add(n.Get(),{cam,0,0,id});
}
AZEXPORT void __cdecl azslCore(ID3D12GraphicsCommandList *cl,ID3D12Resource *depth,ID3D12Resource *mv,unsigned state,uint64_t resolve,uint64_t recording,uint64_t seq,int reset,int fresh,int jitter){
    std::lock_guard<std::recursive_mutex> g(lock);auto *p=packet_for(resolve,seq);
    if(!p||p->core.record)return;
    auto n=native<ID3D12GraphicsCommandList>(cl);if(!fresh||jitter||!n||n.Get()==cl||!hook_list(n.Get())){p->bad=true;return;}
    auto &r=record(n.Get());if(!r.known||r.submitted){p->bad=true;return;}
    auto d=native<ID3D12Resource>(depth),m=native<ID3D12Resource>(mv);
    p->core.record=r.id;p->core.game_record=recording;p->reset=reset!=0;
    // Record the stage even on partial allocation failure, so resources
    // stay alive until that recording is retired and its fence completes.
    bool a=clone(n.Get(),d.Get(),p->depth,rw,rh,DXGI_FORMAT_R32_FLOAT,(D3D12_RESOURCE_STATES)state,p->core_initialized);
    bool b=clone(n.Get(),m.Get(),p->mv,rw,rh,DXGI_FORMAT_R16G16_FLOAT,(D3D12_RESOURCE_STATES)state,p->core_initialized);
    p->core_initialized=a&&b;if(!a||!b){p->bad=true;p->quarantined=true;}
    add(n.Get(),{core,0,0,p->id});
}
AZEXPORT int __cdecl azslWantsFinal(uint64_t resolve){
    std::lock_guard<std::recursive_mutex> g(lock);return initialized&&!fatal&&hdr&&shape&&final_packet(resolve)!=nullptr;
}
AZEXPORT void __cdecl azslFinal(ID3D12GraphicsCommandList *cl,ID3D12Resource *target,ID3D12Resource *hudless,uint64_t resolve,uint64_t recording){
    std::lock_guard<std::recursive_mutex> g(lock);auto *p=final_packet(resolve);
    if(!p||p->final.record)return;auto n=native<ID3D12GraphicsCommandList>(cl);
    auto t=native<ID3D12Resource>(target),h=native<ID3D12Resource>(hudless);
    if(!n||n.Get()==cl||!hook_list(n.Get())||!t||!h){p->bad=true;return;}
    auto &r=record(n.Get());uint64_t id=cookie(t.Get());
    if(!r.known||r.submitted||!id){p->bad=true;return;}
    p->final.record=r.id;p->final.game_record=recording;
    bool ok=clone(n.Get(),h.Get(),p->hud,ow,oh,DXGI_FORMAT_R10G10B10A2_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET,p->hud_initialized);
    p->hud_initialized=ok;if(!ok){p->bad=true;p->quarantined=true;}
    add(n.Get(),{final,id,0,p->id});
    log("FG FINAL_RECORD resolve=%llu packet=%llu target=%p cookie=%llu gameRecording=%llu nativeRecording=%llu generation=%llu hudless=%d",resolve,p->id,t.Get(),id,recording,r.id,generation,ok);
}
AZEXPORT int __cdecl azslBeforePresent(IDXGISwapChain1 *sc,UINT flags,uint64_t call){
    std::lock_guard<std::recursive_mutex> g(lock);
    if(sc!=managed||flags&DXGI_PRESENT_TEST||active.call)return 0;
    active={};active.call=call?call:UINT64_MAX;active.gen=generation;
    if(!initialized||fatal||!hdr||!chain_info(sc)||!gameplay_gate()){off("chain_or_hooks_or_gameplay_gate");return 1;}
    ComPtr<IDXGISwapChain3> s;if(FAILED(sc->QueryInterface(IID_PPV_ARGS(&s)))){off("chain3");return 1;}
    unsigned index=s->GetCurrentBackBufferIndex();active.index=index;
    if(index>=bb.size()){off("index");return 1;}
    auto it=lineage.find(bb[index]);auto v=it==lineage.end()?Provenance{}:it->second;
    lineage.erase(bb[index]); // consume once, never re-use old contents
    auto *p=find_packet(v.packet);
    if(!p||p->bad||p->presented||p->gen!=generation||v.gen!=generation||v.queue!=present_queue.Get()||!v.fence){
        off("unproven_present_lineage");
        log("FG PRESENT_SKIP call=%llu generation=%llu index=%u cookie=%llu reason=lineage packet=%llu packetBad=%d packetPresented=%d transferRecording=%llu submit=%llu actualQueue=%p expectedQueue=%p transferFence=%d",call,generation,index,bb[index],v.packet,p?p->bad:0,p?p->presented:0,v.record,v.submit,v.queue,present_queue.Get(),bool(v.fence));return 1;
    }
    if(!p->camera.submitted||!p->core.submitted||!p->final.submitted||!p->camera.retired||!p->core.retired||!p->final.retired){
        off("producer_record_not_retired");
        log("FG PRESENT_SKIP call=%llu resolve=%llu packet=%llu reason=producer_lifetime cameraSubmitted=%d coreSubmitted=%d finalSubmitted=%d cameraRetired=%d coreRetired=%d finalRetired=%d",call,p->resolve,p->id,p->camera.submitted,p->core.submitted,p->final.submitted,p->camera.retired,p->core.retired,p->final.retired);return 1;
    }
    // Only the 256-byte camera is CPU mapped, and only after its fence is
    // already complete. No CPU fence wait or color comparison on Present.
    auto now=GetTickCount64();bool reset=!last_resolve||p->reset||last_generation!=generation||last_resolve+1!=p->resolve||now-last_tick>250;
    sl::Constants camera;
    if(!p->camera.fence.done()){
        p->bad=true;off("camera_pending");log("FG PRESENT_SKIP call=%llu resolve=%llu packet=%llu reason=camera_fence_pending",call,p->resolve,p->id);return 1;
    }
    if(!camera_constants(*p,camera,reset)||!wait_submitted(present_queue.Get(),p->core.fence,p->core.queue)||
       !wait_submitted(present_queue.Get(),p->final.fence,p->final.queue)){
        p->bad=true;off("guides_or_camera");log("FG PRESENT_SKIP call=%llu resolve=%llu packet=%llu reason=guides_or_camera",call,p->resolve,p->id);return 1;
    }
    if(reset&&armed)off("discontinuity");
    const sl::ViewportHandle vp(0);sl::Extent render{0,0,rw,rh},full{0,0,ow,oh};
    sl::Resource depth(sl::ResourceType::eTex2d,p->depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      mv(sl::ResourceType::eTex2d,p->mv.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      hud(sl::ResourceType::eTex2d,p->hud.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const sl::ResourceTag tags[]={{&depth,sl::kBufferTypeDepth,sl::eValidUntilPresent,&render},{&mv,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent,&render},
      {&hud,sl::kBufferTypeHUDLessColor,sl::eValidUntilPresent,&full},{nullptr,sl::kBufferTypeBackbuffer,sl::eValidUntilPresent,&full}};
    auto status=set_constants(camera,*p->token,vp);
    if(status==sl::Result::eOk){p->tagged=true;status=set_tag(*p->token,vp,tags,_countof(tags),nullptr);}
    if(status==sl::Result::eOk)status=marker(sl::PCLMarker::ePresentStart,*p->token);
    if(status==sl::Result::eOk&&enabled&&!armed){options.mode=sl::DLSSGMode::eOn;status=set_options(vp,options);armed=status==sl::Result::eOk;}
    if(status!=sl::Result::eOk){p->bad=true;off("sdk_tag_or_enable");}
    p->presented=true;active.packet=p->id;active.reset=reset;
    if(status==sl::Result::eOk){last_resolve=p->resolve;last_generation=generation;last_tick=now;}
    log("FG PRESENT_TAG call=%llu resolve=%llu packet=%llu token=%u generation=%llu index=%u bufferCookie=%llu transferRecording=%llu submit=%llu queue=%p reset=%d mode=%d result=%u qpc=%lld",call,p->resolve,p->id,(uint32_t)*p->token,generation,index,bb[index],v.record,v.submit,v.queue,reset,armed,(unsigned)status,qpc());
    return 1;
}
AZEXPORT void __cdecl azslAfterPresent(HRESULT hr){
    std::lock_guard<std::recursive_mutex> g(lock);
    if(!active.call)return;auto a=active;active={};auto *p=find_packet(a.packet);
    if(p&&p->token)marker(sl::PCLMarker::ePresentEnd,*p->token);
    sl::DLSSGState state;
    auto r=get_state?get_state(sl::ViewportHandle(0),state,nullptr):static_cast<sl::Result>(-1);
    if(p&&p->tagged){
        if(r==sl::Result::eOk&&state.inputsProcessingCompletionFence&&state.lastPresentInputsProcessingCompletionFenceValue){
            auto *f=static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
            p->consumption.f=f;p->consumption.value=state.lastPresentInputsProcessingCompletionFenceValue;
        }else if(r==sl::Result::eOk&&SUCCEEDED(hr)){
            // D3D12 default eBlockPresentingClientQueue: queue signal is
            // ordered after SL's input-processing wait. Never use this
            // fallback in eBlockNoClientQueues mode.
            p->consumption=signal(present_queue.Get());
        }
        if(!p->consumption)p->quarantined=true;
    }
    log("FG PRESENT_RESULT call=%llu resolve=%llu token=%u reset=%d mode=%d hr=%08lx sdkResult=%u sdkStatus=%u framesSinceQuery=%u consumptionFence=%p consumptionValue=%llu sdkFence=%d qpc=%lld",
       a.call,p?p->resolve:0,p&&p->token?(uint32_t)*p->token:0,a.reset,armed,(unsigned long)hr,(unsigned)r,(unsigned)state.status,state.numFramesActuallyPresented,
       p?p->consumption.f.Get():nullptr,p?p->consumption.value:0,state.inputsProcessingCompletionFence!=nullptr,qpc());
    if(FAILED(hr)||hr!=S_OK||r!=sl::Result::eOk||state.status!=sl::DLSSGStatus::eOk)off("present_or_sdk_status");
}
