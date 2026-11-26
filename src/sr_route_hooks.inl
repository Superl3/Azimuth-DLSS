/* Additional opt-in vtable interceptors; included after sr_route.inl. */
static HRESULT(STDMETHODCALLTYPE *real_CreateCommandQueue)(ID3D12Device *,const D3D12_COMMAND_QUEUE_DESC *,REFIID,void **);
static HRESULT(STDMETHODCALLTYPE *real_CreateRootSignature)(ID3D12Device *,UINT,const void *,SIZE_T,REFIID,void **);
static void(STDMETHODCALLTYPE *real_CopyDescriptors)(ID3D12Device *,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE *,const UINT *,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE *,const UINT *,D3D12_DESCRIPTOR_HEAP_TYPE);
static void(STDMETHODCALLTYPE *real_CopyBufferRegion)(ID3D12GraphicsCommandList *,ID3D12Resource *,UINT64,ID3D12Resource *,UINT64,UINT64);
static void(STDMETHODCALLTYPE *real_ClearDepthStencilView)(ID3D12GraphicsCommandList *,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT *);
static void(STDMETHODCALLTYPE *real_ResolveQueryData)(ID3D12GraphicsCommandList *,ID3D12QueryHeap *,D3D12_QUERY_TYPE,UINT,UINT,ID3D12Resource *,UINT64);
static void(STDMETHODCALLTYPE *real_CopyTiles)(ID3D12GraphicsCommandList *,ID3D12Resource *,const D3D12_TILED_RESOURCE_COORDINATE *,const D3D12_TILE_REGION_SIZE *,ID3D12Resource *,UINT64,D3D12_TILE_COPY_FLAGS);
static void(STDMETHODCALLTYPE *real_ResolveSubresource)(ID3D12GraphicsCommandList *,ID3D12Resource *,UINT,ID3D12Resource *,UINT,DXGI_FORMAT);
static void(STDMETHODCALLTYPE *real_ClearRenderTargetView)(ID3D12GraphicsCommandList *,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT *,UINT,const D3D12_RECT *);
static void(STDMETHODCALLTYPE *real_ClearUnorderedAccessViewUint)(ID3D12GraphicsCommandList *,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource *,const UINT *,UINT,const D3D12_RECT *);
static void(STDMETHODCALLTYPE *real_ClearUnorderedAccessViewFloat)(ID3D12GraphicsCommandList *,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource *,const FLOAT *,UINT,const D3D12_RECT *);
static void(STDMETHODCALLTYPE *real_Dispatch)(ID3D12GraphicsCommandList *,UINT,UINT,UINT);
static void(STDMETHODCALLTYPE *real_ExecuteIndirect)(ID3D12GraphicsCommandList *,ID3D12CommandSignature *,UINT,ID3D12Resource *,UINT64,ID3D12Resource *,UINT64);
static void(STDMETHODCALLTYPE *real_ExecuteBundle)(ID3D12GraphicsCommandList *,ID3D12GraphicsCommandList *);
static void(STDMETHODCALLTYPE *real_DiscardResource)(ID3D12GraphicsCommandList *,ID3D12Resource *,const D3D12_DISCARD_REGION *);
static void(STDMETHODCALLTYPE *real_IASetPrimitiveTopology)(ID3D12GraphicsCommandList *,D3D_PRIMITIVE_TOPOLOGY);
static void(STDMETHODCALLTYPE *real_SetPredication)(ID3D12GraphicsCommandList *,ID3D12Resource *,UINT64,D3D12_PREDICATION_OP);
static void(STDMETHODCALLTYPE *real_BeginQuery)(ID3D12GraphicsCommandList *,ID3D12QueryHeap *,D3D12_QUERY_TYPE,UINT);
static void(STDMETHODCALLTYPE *real_EndQuery)(ID3D12GraphicsCommandList *,ID3D12QueryHeap *,D3D12_QUERY_TYPE,UINT);

static HRESULT STDMETHODCALLTYPE trace_CreateCommandQueue(ID3D12Device *dev,const D3D12_COMMAND_QUEUE_DESC *d,REFIID riid,void **out) {
    HRESULT hr=real_CreateCommandQueue(dev,d,riid,out);
    if(SUCCEEDED(hr)&&out&&*out){ID3D12CommandQueue *q=NULL;if(SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out,&IID_ID3D12CommandQueue,(void **)&q))){sr_trace_queue(q,d);debug_trace_queue_hook(q,d);ID3D12CommandQueue_Release(q);}}
    return hr;
}
static HRESULT STDMETHODCALLTYPE trace_CreateDescriptorHeap(ID3D12Device *dev,const D3D12_DESCRIPTOR_HEAP_DESC *d,REFIID riid,void **out) {
    HRESULT hr=real_CreateDescriptorHeap(dev,d,riid,out);
    if(SUCCEEDED(hr)&&out&&*out){ID3D12DescriptorHeap *h=NULL;if(SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out,&IID_ID3D12DescriptorHeap,(void **)&h))){sr_trace_heap(dev,h,d);ID3D12DescriptorHeap_Release(h);}}
    return hr;
}
static HRESULT STDMETHODCALLTYPE trace_CreateRootSignature(ID3D12Device *dev,UINT mask,const void *blob,SIZE_T bytes,REFIID riid,void **out) {
    HRESULT hr=real_CreateRootSignature(dev,mask,blob,bytes,riid,out);
    if(SUCCEEDED(hr)&&out&&*out){ID3D12RootSignature *r=NULL;if(SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out,&IID_ID3D12RootSignature,(void **)&r))){sr_trace_root(r,blob,bytes);azrp_note_root(r,blob,bytes);ID3D12RootSignature_Release(r);}}
    return hr;
}
static void STDMETHODCALLTYPE trace_CreateShaderResourceView(ID3D12Device *dev,ID3D12Resource *res,const D3D12_SHADER_RESOURCE_VIEW_DESC *d,D3D12_CPU_DESCRIPTOR_HANDLE h){real_CreateShaderResourceView(dev,res,d,h);sr_trace_srv(res,d,h);azrp_shadow_srv(dev,res,d,h);}
static void STDMETHODCALLTYPE trace_CreateUnorderedAccessView(ID3D12Device *dev,ID3D12Resource *res,ID3D12Resource *counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC *d,D3D12_CPU_DESCRIPTOR_HANDLE h){real_CreateUnorderedAccessView(dev,res,counter,d,h);azt_write_desc(h.ptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,NULL,DXGI_FORMAT_UNKNOWN,0);azrp_shadow_uav(dev,res,counter,d,h);}
static void STDMETHODCALLTYPE trace_CreateConstantBufferView(ID3D12Device *dev,const D3D12_CONSTANT_BUFFER_VIEW_DESC *d,D3D12_CPU_DESCRIPTOR_HANDLE h){real_CreateConstantBufferView(dev,d,h);azt_write_desc(h.ptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,NULL,DXGI_FORMAT_UNKNOWN,0);azrp_shadow_cbv(dev,d,h);}
static void STDMETHODCALLTYPE trace_CopyDescriptors(ID3D12Device *dev,UINT nd,const D3D12_CPU_DESCRIPTOR_HANDLE *dst,const UINT *ds,UINT ns,const D3D12_CPU_DESCRIPTOR_HANDLE *src,const UINT *ss,D3D12_DESCRIPTOR_HEAP_TYPE type){real_CopyDescriptors(dev,nd,dst,ds,ns,src,ss,type);sr_trace_copy_descriptors(dev,nd,dst,ds,ns,src,ss,type);}
static void STDMETHODCALLTYPE trace_CopyDescriptorsSimple(ID3D12Device *dev,UINT n,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE type){real_CopyDescriptorsSimple(dev,n,dst,src,type);sr_trace_copy_descriptors(dev,1,&dst,&n,1,&src,&n,type);}
static void STDMETHODCALLTYPE trace_CopyResource(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst,ID3D12Resource *src){visible_count_work(cl,state_for(cl),"CopyResource",dst,src,0,0,0,0);debug_trace_copy(cl,dst,src,"resource");sr_trace_write(cl,dst);real_CopyResource(cl,dst,src);visible_trace_copy(cl,dst,src,1);}
static void STDMETHODCALLTYPE trace_CopyTextureRegion(ID3D12GraphicsCommandList *cl,const D3D12_TEXTURE_COPY_LOCATION *dst,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION *src,const D3D12_BOX *box){visible_count_work(cl,state_for(cl),"CopyTextureRegion",dst?dst->pResource:NULL,src?src->pResource:NULL,x,y,z,box!=NULL);if(dst&&src)debug_trace_copy(cl,dst->pResource,src->pResource,"region");if(dst)sr_trace_write(cl,dst->pResource);real_CopyTextureRegion(cl,dst,x,y,z,src,box);preview_hdr_copy(cl,dst,x,y,z,src,box);if(dst&&src)visible_trace_copy(cl,dst->pResource,src->pResource,!x&&!y&&!z&&!box&&dst->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&src->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&!dst->SubresourceIndex&&!src->SubresourceIndex);}
static void STDMETHODCALLTYPE trace_ResolveSubresource(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst,UINT di,ID3D12Resource *src,UINT si,DXGI_FORMAT fmt){visible_count_work(cl,state_for(cl),"ResolveSubresource",dst,src,di,si,(UINT)fmt,0);debug_trace_copy(cl,dst,src,"resolve");sr_trace_write(cl,dst);real_ResolveSubresource(cl,dst,di,src,si,fmt);}
static void STDMETHODCALLTYPE trace_ClearRenderTargetView(ID3D12GraphicsCommandList *cl,D3D12_CPU_DESCRIPTOR_HANDLE h,const FLOAT *rgba,UINT n,const D3D12_RECT *rect){
    ListState *s=state_for(cl);
    AcquireSRWLockExclusive(&azt_lock);
    AztDesc *d=azt_slot(azt_heap_for_cpu(h.ptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV),h.ptr,0);
    ID3D12Resource *res=d?d->res:NULL;const ViewEntry *live=view_find(h.ptr);
    AztState *st=res?azt_state(res,0):NULL;
    int verified=azvis_observing()&&s&&s->trace_watch_active&&!n&&rgba&&d&&d->simple&&
        d->format==DXGI_FORMAT_R10G10B10A2_UNORM&&d->desc.Width==1920&&d->desc.Height==1080&&
        d->desc.DepthOrArraySize==1&&d->desc.MipLevels==1&&d->desc.SampleDesc.Count==1&&
        live&&live->resource==res&&st&&st->known&&st->list==cl&&st->recording==s->trace_recording_id&&
        st->state==D3D12_RESOURCE_STATE_RENDER_TARGET&&s->num_rtv==1&&s->rtv[0].ptr==h.ptr&&!s->has_dsv;
    ReleaseSRWLockExclusive(&azt_lock);
    visible_count_work(cl,s,"ClearRenderTargetView",res,NULL,n,0,0,(UINT)verified);sr_trace_write(cl,res);real_ClearRenderTargetView(cl,h,rgba,n,rect);
}
static void STDMETHODCALLTYPE trace_CopyBufferRegion(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst,UINT64 doff,ID3D12Resource *src,UINT64 soff,UINT64 bytes){
    visible_count_work(cl,state_for(cl),"CopyBufferRegion",dst,src,(UINT)doff,(UINT)soff,(UINT)bytes,0);real_CopyBufferRegion(cl,dst,doff,src,soff,bytes);
}
static void STDMETHODCALLTYPE trace_ClearDepthStencilView(ID3D12GraphicsCommandList *cl,D3D12_CPU_DESCRIPTOR_HANDLE h,D3D12_CLEAR_FLAGS flags,FLOAT depth,UINT8 stencil,UINT n,const D3D12_RECT *rect){
    visible_count_work(cl,state_for(cl),"ClearDepthStencilView",NULL,NULL,(UINT)flags,n,0,0);real_ClearDepthStencilView(cl,h,flags,depth,stencil,n,rect);
}
static void STDMETHODCALLTYPE trace_ResolveQueryData(ID3D12GraphicsCommandList *cl,ID3D12QueryHeap *heap,D3D12_QUERY_TYPE type,UINT start,UINT count,ID3D12Resource *dst,UINT64 offset){
    visible_count_work(cl,state_for(cl),"ResolveQueryData",dst,NULL,(UINT)type,start,count,(UINT)offset);real_ResolveQueryData(cl,heap,type,start,count,dst,offset);
}
static void STDMETHODCALLTYPE trace_CopyTiles(ID3D12GraphicsCommandList *cl,ID3D12Resource *tiled,const D3D12_TILED_RESOURCE_COORDINATE *coord,const D3D12_TILE_REGION_SIZE *size,ID3D12Resource *buffer,UINT64 offset,D3D12_TILE_COPY_FLAGS flags){
    visible_count_work(cl,state_for(cl),"CopyTiles",tiled,buffer,(UINT)flags,(UINT)offset,0,0);real_CopyTiles(cl,tiled,coord,size,buffer,offset,flags);
}
static void STDMETHODCALLTYPE trace_ClearUnorderedAccessViewUint(ID3D12GraphicsCommandList *cl,D3D12_GPU_DESCRIPTOR_HANDLE gh,D3D12_CPU_DESCRIPTOR_HANDLE ch,ID3D12Resource *res,const UINT *values,UINT n,const D3D12_RECT *rect){visible_count_work(cl,state_for(cl),"ClearUnorderedAccessViewUint",res,NULL,n,0,0,0);sr_trace_write(cl,res);real_ClearUnorderedAccessViewUint(cl,gh,ch,res,values,n,rect);}
static void STDMETHODCALLTYPE trace_ClearUnorderedAccessViewFloat(ID3D12GraphicsCommandList *cl,D3D12_GPU_DESCRIPTOR_HANDLE gh,D3D12_CPU_DESCRIPTOR_HANDLE ch,ID3D12Resource *res,const FLOAT *values,UINT n,const D3D12_RECT *rect){visible_count_work(cl,state_for(cl),"ClearUnorderedAccessViewFloat",res,NULL,n,0,0,0);sr_trace_write(cl,res);real_ClearUnorderedAccessViewFloat(cl,gh,ch,res,values,n,rect);}
static void STDMETHODCALLTYPE trace_Dispatch(ID3D12GraphicsCommandList *cl,UINT x,UINT y,UINT z){visible_count_work(cl,state_for(cl),"Dispatch",NULL,NULL,x,y,z,0);debug_trace_dispatch(cl,x,y,z);sr_trace_write(cl,NULL);real_Dispatch(cl,x,y,z);}
static void STDMETHODCALLTYPE trace_ExecuteIndirect(ID3D12GraphicsCommandList *cl,ID3D12CommandSignature *sig,UINT max,ID3D12Resource *arg,UINT64 ao,ID3D12Resource *count,UINT64 co){visible_count_work(cl,state_for(cl),"ExecuteIndirect",count,arg,max,(UINT)ao,(UINT)co,0);sr_trace_write(cl,NULL);if(azrp_sr_enabled){ListState *s=state_for(cl);if(s){s->trace_graphics_cbv_mask=0;memset(s->trace_graphics_constant_words,0,sizeof(s->trace_graphics_constant_words));}}real_ExecuteIndirect(cl,sig,max,arg,ao,count,co);}
static void STDMETHODCALLTYPE trace_ExecuteBundle(ID3D12GraphicsCommandList *cl,ID3D12GraphicsCommandList *bundle){visible_count_work(cl,state_for(cl),"ExecuteBundle",NULL,NULL,0,0,0,0);sr_trace_write(cl,NULL);ListState *s=state_for(cl);if(s){s->trace_tables_mask=0;s->trace_compute_mask=0;s->trace_graphics_cbv_mask=0;s->trace_bindings_uncertain=1;s->trace_graphics_rs=NULL;s->trace_topology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;s->trace_query_unknown=1;}real_ExecuteBundle(cl,bundle);}
static void STDMETHODCALLTYPE trace_DiscardResource(ID3D12GraphicsCommandList *cl,ID3D12Resource *res,const D3D12_DISCARD_REGION *region){visible_count_work(cl,state_for(cl),"DiscardResource",res,NULL,0,0,0,0);sr_trace_write(cl,res);real_DiscardResource(cl,res,region);}
static void STDMETHODCALLTYPE trace_IASetPrimitiveTopology(ID3D12GraphicsCommandList *cl,D3D_PRIMITIVE_TOPOLOGY topology){ListState *s=state_for(cl);if(s)s->trace_topology=topology;real_IASetPrimitiveTopology(cl,topology);}
static void STDMETHODCALLTYPE trace_SetPredication(ID3D12GraphicsCommandList *cl,ID3D12Resource *res,UINT64 offset,D3D12_PREDICATION_OP op){ListState *s=state_for(cl);if(s)s->trace_predicated=res!=NULL;real_SetPredication(cl,res,offset,op);}
static void STDMETHODCALLTYPE trace_BeginQuery(ID3D12GraphicsCommandList *cl,ID3D12QueryHeap *heap,D3D12_QUERY_TYPE type,UINT index){ListState *s=state_for(cl);if(s){if(s->trace_queries==UINT_MAX)s->trace_query_unknown=1;else ++s->trace_queries;}real_BeginQuery(cl,heap,type,index);}
static void STDMETHODCALLTYPE trace_EndQuery(ID3D12GraphicsCommandList *cl,ID3D12QueryHeap *heap,D3D12_QUERY_TYPE type,UINT index){ListState *s=state_for(cl);if(s && type!=D3D12_QUERY_TYPE_TIMESTAMP){if(s->trace_queries)--s->trace_queries;else s->trace_query_unknown=1;}real_EndQuery(cl,heap,type,index);}

#define AZT_HOOK_DEV(method) patch_slot(VT_SLOT(dev,ID3D12DeviceVtbl,method),(void *)trace_##method,(void **)&real_##method)
static void sr_trace_hook_device(ID3D12Device *dev){
    if(!sr_trace_enabled())return;
    sr_trace_init();
    AZT_HOOK_DEV(CreateCommandQueue);AZT_HOOK_DEV(CreateDescriptorHeap);AZT_HOOK_DEV(CreateRootSignature);AZT_HOOK_DEV(CreateShaderResourceView);
    AZT_HOOK_DEV(CreateUnorderedAccessView);AZT_HOOK_DEV(CreateConstantBufferView);AZT_HOOK_DEV(CopyDescriptors);AZT_HOOK_DEV(CopyDescriptorsSimple);
}
#undef AZT_HOOK_DEV
#define AZT_HOOK_CL(method) patch_slot(VT_SLOT(cl,ID3D12GraphicsCommandListVtbl,method),(void *)trace_##method,(void **)&real_##method)
static void sr_trace_hook_list(ID3D12GraphicsCommandList *cl){
    if(!sr_trace_enabled())return;
    AZT_HOOK_CL(CopyResource);AZT_HOOK_CL(CopyTextureRegion);AZT_HOOK_CL(ResolveSubresource);AZT_HOOK_CL(ClearRenderTargetView);
    AZT_HOOK_CL(ClearUnorderedAccessViewUint);AZT_HOOK_CL(ClearUnorderedAccessViewFloat);AZT_HOOK_CL(Dispatch);
    AZT_HOOK_CL(ExecuteIndirect);AZT_HOOK_CL(ExecuteBundle);AZT_HOOK_CL(DiscardResource);AZT_HOOK_CL(IASetPrimitiveTopology);
    AZT_HOOK_CL(SetPredication);AZT_HOOK_CL(BeginQuery);AZT_HOOK_CL(EndQuery);
    if(azvis_enabled){AZT_HOOK_CL(CopyBufferRegion);AZT_HOOK_CL(ClearDepthStencilView);AZT_HOOK_CL(ResolveQueryData);AZT_HOOK_CL(CopyTiles);}
}
#undef AZT_HOOK_CL
