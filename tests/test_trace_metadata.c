/* Host test of the actual metadata/route implementation, not a Windows ABI test. */
#include <assert.h>
#include "trace_stub.h"
#include "../src/sr_route.inl"
static void cpu_handle(ID3D12DescriptorHeap *h,D3D12_CPU_DESCRIPTOR_HANDLE *v){v->ptr=h->cpu;}
static void gpu_handle(ID3D12DescriptorHeap *h,D3D12_GPU_DESCRIPTOR_HANDLE *v){v->ptr=h->gpu;}
static void res_desc(ID3D12Resource *r,D3D12_RESOURCE_DESC *v){*v=r->desc;}
static StubHeapVtbl hv={cpu_handle,gpu_handle};static StubResVtbl rv={res_desc};
static ID3D12Resource tex(unsigned w,unsigned h){ID3D12Resource r;memset(&r,0,sizeof(r));r.lpVtbl=&rv;r.desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;r.desc.Width=w;r.desc.Height=h;r.desc.MipLevels=r.desc.DepthOrArraySize=r.desc.SampleDesc.Count=1;r.desc.Format=DXGI_FORMAT_R11G11B10_FLOAT;return r;}
int main(void){
    ID3D12Device dev={0};ID3D12GraphicsCommandList cl={0},other={0};ID3D12CommandQueue q={0},q2={0};
    ID3D12RootSignature rs={0};ID3D12PipelineState pso={0};
    ID3D12DescriptorHeap host={&hv,0x1000,0},vis={&hv,0x2000,0x8000},rt={&hv,0x3000,0};
    D3D12_DESCRIPTOR_HEAP_DESC hd={D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,32,0};
    sr_trace_heap(&dev,&host,&hd);hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;sr_trace_heap(&dev,&vis,&hd);
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.Flags=0;sr_trace_heap(&dev,&rt,&hd);
    ID3D12Resource scene=tex(2560,1067),output=tex(3840,1600),target=tex(3840,1600);
    D3D12_CPU_DESCRIPTOR_HANDLE source_cpu={0x1000},dest_cpu={0x2040},target_cpu={0x3000};
    sr_trace_srv(&scene,NULL,source_cpu);sr_trace_copy_descriptors(&dev,1,&dest_cpu,NULL,1,&source_cpu,NULL,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    AztDesc *d=azt_slot(azt_heap_for_ptr(&vis),dest_cpu.ptr,0);assert(d&&d->res==&scene&&d->simple);
    sr_trace_rtv(&target,NULL,target_cpu);
    AztRoot *r=azt_root_slot(&rs,1);r->complete=1;UINT next=0;
    azt_add_range(r,0,1,2,0,0,0,&next); /* Two non-SRV descriptors preceding t7. */
    azt_add_range(r,0,D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,7,2,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,&next);
    assert(r->count==1&&r->ranges[0].offset==2&&r->ranges[0].reg==7&&r->ranges[0].space==2);
    AztPso *p=azt_pso_slot(&pso,1);p->rs=&rs;p->pure_candidate=1;p->hash[0]=0x1234;p->hash[1]=0x5678;p->vs_hash[0]=0x9999;
    memcpy(azt_hash,p->hash,16);memcpy(azt_vs_hash,p->vs_hash,16);azt_confirmed=1;azt_reg=7;azt_space=2;
    D3D12_COMMAND_QUEUE_DESC qd={D3D12_COMMAND_LIST_TYPE_DIRECT};sr_trace_queue(&q,&qd);assert(sr_trace_route_ready());
    ListState s;memset(&s,0,sizeof(s));s.pso=&pso;s.trace_graphics_rs=&rs;s.trace_tables_mask=1;s.num_heaps=1;s.heaps[0]=&vis;s.tables[0].ptr=0x8000;
    s.num_rtv=1;s.rtv[0]=target_cpu;s.num_vp=s.num_sc=1;s.vp[0].Width=3840;s.vp[0].Height=1600;s.sc[0].right=3840;s.sc[0].bottom=1600;s.trace_topology=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    D3D12_RESOURCE_BARRIER b;memset(&b,0,sizeof(b));b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=&target;b.Transition.Subresource=0;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    sr_trace_barriers(&cl,&s,1,&b);assert(s.trace_recording_id);
    sr_trace_resolved(&cl,&scene,&output);
    assert(!sr_trace_draw(&other,&s,3,1,ROLE_NONE,1));assert(stub_copies==0);
    s.trace_predicated=1;assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));s.trace_predicated=0;
    s.trace_queries=1;assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));s.trace_queries=0;
    azt_hash[0]^=1;assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));azt_hash[0]^=1;
    azt_vs_hash[0]^=1;assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));azt_vs_hash[0]^=1;
    s.trace_tables_mask=0;assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));s.trace_tables_mask=1;
    assert(!sr_trace_draw(&cl,&s,3,2,ROLE_NONE,1));assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,0));
    assert(sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));assert(stub_copies==1&&stub_transitions==4&&stub_src==&output&&stub_dst==&target&&stub_visible);
    assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));assert(stub_copies==1);
    sr_trace_begin_resolve();sr_trace_resolved(&cl,&scene,&output);sr_trace_write(&cl,&scene);assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));
    sr_trace_begin_resolve();sr_trace_resolved(&cl,&scene,&output);assert(!stub_visible);
    azt_write_desc(source_cpu.ptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,NULL,DXGI_FORMAT_UNKNOWN,0);
    sr_trace_copy_descriptors(&dev,1,&dest_cpu,NULL,1,&source_cpu,NULL,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));
    sr_trace_srv(&scene,NULL,source_cpu);sr_trace_copy_descriptors(&dev,1,&dest_cpu,NULL,1,&source_cpu,NULL,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s.trace_recording_id++;assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));sr_trace_barriers(&cl,&s,1,&b);
    b.Flags=1;sr_trace_barriers(&cl,&s,1,&b);assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));b.Flags=0;sr_trace_barriers(&cl,&s,1,&b);
    sr_trace_reset(&cl);assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));
    sr_trace_resolved(&cl,&scene,&output);sr_trace_queue(&q2,&qd);assert(!sr_trace_route_ready());assert(!sr_trace_draw(&cl,&s,3,1,ROLE_NONE,1));
    /* The actual FH2 capture has five direct queues. Merely observing multiple
       queues must not mark unrelated descriptor tracking incomplete. Never arm route. */
    ID3D12CommandQueue more[3]={{0},{0},{0}};
    for(unsigned i=0;i<3;++i)sr_trace_queue(&more[i],&qd);
    assert(azt_direct_count==5&&!azt_incomplete&&!sr_trace_route_ready());
    sr_trace_queue(&more[0],&qd);assert(azt_direct_count==5);
    /* Heap pointer reused: previous generation cannot resolve an old descriptor. */
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;sr_trace_heap(&dev,&vis,&hd);assert(!azt_slot(azt_heap_for_ptr(&vis),dest_cpu.ptr,0));
    puts("PASS: metadata propagation, root ranges, real route predicate/copy, stale handles, write/reset/queue rejection (host doubles)");return 0;
}
