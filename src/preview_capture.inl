/* Explicit preview only. Copies on producer list; map only after an
   observed submission and completion of a private fence. All refs retained
   to process exit, bounded by two groups. No visible replacement. */
#define AZPV_GROUPS 2
#define AZPV_SHOTS 7
#define AZPV_LIMIT (256ull*1024*1024)
#define AZPV_FILE_LIMIT (64ull*1024*1024)
enum {PV_RECORDED=1,PV_SUBMITTED,PV_SAVED,PV_FAILED};
typedef struct {
    ID3D12Resource *source,*buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT rows,w,h,format;
    UINT64 row_bytes,total;
    const char *name;
} AzpvShot;
typedef struct {
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    unsigned long long resolve;
    unsigned count,good;
    int state,list_current;
    uint64_t started;
    AzpvShot shots[AZPV_SHOTS];
    ID3D12Resource *scene,*postfx,*composite;
} AzpvGroup;
static SRWLOCK azpv_lock=SRWLOCK_INIT;
static AzpvGroup azpv_groups[AZPV_GROUPS];
static unsigned azpv_count;
static UINT64 azpv_bytes;
static volatile LONG azpv_queue_ready;
static int azpv_enabled,azpv_postfx,azpv_done;
static wchar_t azpv_dir[MAX_PATH];
static char azpv_dirname[80];

static void preview_init(void) {
    if(!dlss_sr_preview_mode())return;
    char b[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_PREVIEW_POSTFX",b,sizeof(b));
    azpv_postfx=n==1&&b[0]=='1';
    azpv_enabled=GetModuleHandleW(L"renderdoc.dll")==NULL;
    log_msg("PREVIEW CONFIG readback=%d postfx=%d groups=%u maxBytes=%llu",
        azpv_enabled,azpv_postfx,(unsigned)AZPV_GROUPS,(unsigned long long)AZPV_LIMIT);
}
static int preview_format(DXGI_FORMAT f) {
    return f==DXGI_FORMAT_R11G11B10_FLOAT||f==DXGI_FORMAT_R16G16_FLOAT||
           f==DXGI_FORMAT_D32_FLOAT||f==DXGI_FORMAT_R32_FLOAT||f==DXGI_FORMAT_R32_TYPELESS||
           f==DXGI_FORMAT_R10G10B10A2_TYPELESS||f==DXGI_FORMAT_R10G10B10A2_UNORM;
}
static int preview_add(AzpvGroup *g,ID3D12Resource *res,D3D12_RESOURCE_STATES state,const char *name) {
    if(!res||g->count>=AZPV_SHOTS)return 0;
    D3D12_RESOURCE_DESC d;res->lpVtbl->GetDesc(res,&d);
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!d.Width||d.Width>7680||
       !d.Height||d.Height>4320||d.DepthOrArraySize!=1||d.MipLevels!=1||
       d.SampleDesc.Count!=1||d.SampleDesc.Quality||!preview_format(d.Format))return 0;
    AzpvShot s;memset(&s,0,sizeof(s));s.w=(UINT)d.Width;s.h=d.Height;s.format=(UINT)d.Format;s.name=name;
    g_device->lpVtbl->GetCopyableFootprints(g_device,&d,0,1,0,&s.footprint,&s.rows,&s.row_bytes,&s.total);
    if(!s.total||s.total>AZPV_FILE_LIMIT||s.total>AZPV_LIMIT-azpv_bytes||
       s.rows!=s.h||s.row_bytes!=(UINT64)s.w*4||
       s.footprint.Footprint.RowPitch<s.row_bytes||
       s.footprint.Offset+(UINT64)(s.rows-1)*s.footprint.Footprint.RowPitch+s.row_bytes>s.total)return 0;
    D3D12_HEAP_PROPERTIES hp;memset(&hp,0,sizeof(hp));hp.Type=D3D12_HEAP_TYPE_READBACK;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC bd;memset(&bd,0,sizeof(bd));bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width=s.total;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(ID3D12Device_CreateCommittedResource(g_device,&hp,D3D12_HEAP_FLAG_NONE,&bd,
       D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void **)&s.buffer))||!s.buffer)return 0;
    ID3D12Resource_AddRef(res);s.source=res;
    D3D12_TEXTURE_COPY_LOCATION src,dst;memset(&src,0,sizeof(src));memset(&dst,0,sizeof(dst));
    src.pResource=res;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=0;
    dst.pResource=s.buffer;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=s.footprint;
    if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE)hooks_transition(g->list,res,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
    g->list->lpVtbl->CopyTextureRegion(g->list,&dst,0,0,0,&src,NULL);
    if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE)hooks_transition(g->list,res,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
    g->shots[g->count++]=s;azpv_bytes+=s.total;
    log_msg("PREVIEW RECORD resolve=%llu stage=%s size=%ux%u format=%u bytes=%llu",g->resolve,name,s.w,s.h,s.format,(unsigned long long)s.total);
    return 1;
}
void hooks_preview_record(ID3D12GraphicsCommandList *cl,ID3D12Resource *color,ID3D12Resource *depth,
                          ID3D12Resource *motion,ID3D12Resource *out,D3D12_RESOURCE_STATES read_state,int reset) {
    if(!azpv_enabled||GetModuleHandleW(L"renderdoc.dll")||!cl||!g_device||
       cl->lpVtbl->GetType(cl)!=D3D12_COMMAND_LIST_TYPE_DIRECT||!InterlockedCompareExchange(&azpv_queue_ready,0,0))return;
    AcquireSRWLockExclusive(&azt_lock);
    int wanted=azdbg.active&&azdbg.id==1&&azdbg.observed<AZPV_GROUPS;
    unsigned long long resolve=azt_frame+1;
    ReleaseSRWLockExclusive(&azt_lock);
    if(!wanted)return;
    AcquireSRWLockExclusive(&azpv_lock);
    if(azpv_count>=AZPV_GROUPS||azpv_done)goto done;
    for(unsigned i=0;i<azpv_count;++i)if(azpv_groups[i].resolve==resolve)goto done;
    AzpvGroup *g=&azpv_groups[azpv_count++];
    g->resolve=resolve;g->started=GetTickCount64();
    if(FAILED(ID3D12Device_CreateFence(g_device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void **)&g->fence))||!g->fence){
        g->state=PV_FAILED;log_msg("PREVIEW FAILED resolve=%llu reason=fence_create",resolve);goto done;
    }
    ID3D12GraphicsCommandList_AddRef(cl);g->list=cl;g->list_current=1;g->state=PV_RECORDED;g->scene=color;
    log_msg("PREVIEW GROUP resolve=%llu reset=%d jitter=0 list=%p",resolve,reset,(void *)cl);
    if(!preview_add(g,color,read_state,"color"))log_msg("PREVIEW SKIP resolve=%llu stage=color",resolve);
    if(!preview_add(g,depth,read_state,"depth"))log_msg("PREVIEW SKIP resolve=%llu stage=depth",resolve);
    if(!preview_add(g,motion,read_state,"mv"))log_msg("PREVIEW SKIP resolve=%llu stage=mv",resolve);
    if(!preview_add(g,out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"sr"))log_msg("PREVIEW SKIP resolve=%llu stage=sr",resolve);
    if(!g->count)g->state=PV_FAILED;
done: ReleaseSRWLockExclusive(&azpv_lock);
}
/* Opt-in snapshots of exact observed fullscreen passes only. Require
   matching live RTV metadata and a known RT state in this recording. */
static void preview_after_draw(ID3D12GraphicsCommandList *cl,ListState *s) {
    if(!azpv_enabled||!azpv_postfx||!s||s->num_rtv!=1||s->has_dsv||s->num_vp!=1||
       s->num_sc!=1||s->trace_predicated||s->trace_queries||s->trace_query_unknown||
       s->trace_topology!=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return;
    ID3D12Resource *target=NULL,*input=NULL;DWORD hash=0;unsigned long long resolve=0;
    AcquireSRWLockExclusive(&azt_lock);
    if(azdbg.active&&azdbg.id==1&&azdbg.observed<=AZPV_GROUPS&&!azt_incomplete){
        AztPso *p=azt_pso_slot(s->pso,0);
        AztRoot *r=azt_root_slot(s->trace_graphics_rs,0);
        AztDesc *t=azt_slot(azt_heap_for_cpu(s->rtv[0].ptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV),s->rtv[0].ptr,0);
        const ViewEntry *v=view_find(s->rtv[0].ptr);
        AztState *st=t&&t->res?azt_state(t->res,0):NULL;
        if(p&&r&&p->rs==s->trace_graphics_rs&&azfg_known_candidate(p)&&!p->blend_enabled&&!p->depth_enabled&&
           t&&t->res&&v&&v->resource==t->res&&t->simple&&t->format==DXGI_FORMAT_R10G10B10A2_UNORM&&
           st&&st->known&&st->state==D3D12_RESOURCE_STATE_RENDER_TARGET&&st->list==cl&&
           s->trace_recording_id&&st->recording==s->trace_recording_id&&
           s->vp[0].TopLeftX==0&&s->vp[0].TopLeftY==0&&s->vp[0].Width==(float)t->desc.Width&&s->vp[0].Height==(float)t->desc.Height&&
           s->sc[0].left==0&&s->sc[0].top==0&&s->sc[0].right>=(LONG)t->desc.Width&&s->sc[0].bottom>=(LONG)t->desc.Height){
            unsigned found=0;
            for(UINT i=0;i<r->count;++i)if(r->ranges[i].reg==0&&r->ranges[i].space==0&&r->ranges[i].count){
                AztDesc *a=azt_bound_srv(s,&r->ranges[i],0);if(a&&a->res){input=a->res;++found;}
            }
            if(found==1){target=t->res;hash=p->hash[0];resolve=azt_frame;ID3D12Resource_AddRef(target);}
        }
    }
    ReleaseSRWLockExclusive(&azt_lock);
    if(!target)return;
    AcquireSRWLockExclusive(&azpv_lock);
    for(unsigned i=0;i<azpv_count;++i){
        AzpvGroup *g=&azpv_groups[i];
        if(g->state!=PV_RECORDED||!g->list_current||g->list!=cl||g->resolve!=resolve)continue;
        const char *name=NULL;
        if(hash==0x479be24a&&!g->postfx&&input==g->scene)name="preoverlay";
        else if(hash==0xa7840ad0&&g->postfx&&!g->composite&&input==g->postfx)name="composite";
        else if((hash==0x577d52ef||hash==0xa93721c4)&&g->composite&&input==g->composite)name="final";
        if(!name)continue;
        int seen=0;for(unsigned k=0;k<g->count;++k)if(!strcmp(g->shots[k].name,name))seen=1;
        if(!seen&&preview_add(g,target,D3D12_RESOURCE_STATE_RENDER_TARGET,name)){
            if(hash==0x479be24a)g->postfx=target;
            if(hash==0xa7840ad0)g->composite=target;
        }
    }
    ReleaseSRWLockExclusive(&azpv_lock);
    ID3D12Resource_Release(target);
}
static void preview_reset(ID3D12GraphicsCommandList *cl) {
    if(!azpv_enabled)return;
    AcquireSRWLockExclusive(&azpv_lock);
    for(unsigned i=0;i<azpv_count;++i){
        AzpvGroup *g=&azpv_groups[i];
        if(g->list==cl&&g->list_current){
            g->list_current=0;
            if(g->state==PV_RECORDED){g->state=PV_FAILED;log_msg("PREVIEW FAILED resolve=%llu reason=reset_before_submit",g->resolve);}
        }
    }
    ReleaseSRWLockExclusive(&azpv_lock);
}
typedef void (STDMETHODCALLTYPE *AzpvExecute)(ID3D12CommandQueue *,UINT,ID3D12CommandList *const *);
static void preview_execute(ID3D12CommandQueue *q,UINT n,ID3D12CommandList *const *lists,AzpvExecute original) {
    if(!azpv_enabled){original(q,n,lists);return;}
    AcquireSRWLockExclusive(&azpv_lock);
    unsigned matches[AZPV_GROUPS]={0},any=0;
    for(unsigned i=0;i<azpv_count;++i){
        AzpvGroup *g=&azpv_groups[i];if(!g->list_current)continue;
        for(UINT j=0;lists&&j<n;++j)if((void *)lists[j]==(void *)g->list)++matches[i];
        any|=matches[i];
        if(matches[i]&&(matches[i]!=1||g->state!=PV_RECORDED)){
            if(g->state!=PV_FAILED)log_msg("PREVIEW FAILED resolve=%llu reason=repeated_submission",g->resolve);
            g->state=PV_FAILED;
        }
    }
    if(!any){ReleaseSRWLockExclusive(&azpv_lock);original(q,n,lists);return;}
    original(q,n,lists);
    for(unsigned i=0;i<azpv_count;++i){
        AzpvGroup *g=&azpv_groups[i];if(matches[i]!=1||g->state!=PV_RECORDED)continue;
        if(FAILED(q->lpVtbl->Signal(q,g->fence,1))){
            g->state=PV_FAILED;log_msg("PREVIEW FAILED resolve=%llu reason=signal",g->resolve);
        }else{
            g->state=PV_SUBMITTED;
            log_msg("PREVIEW SUBMITTED resolve=%llu queue=%p list=%p",g->resolve,(void *)q,(void *)g->list);
        }
    }
    ReleaseSRWLockExclusive(&azpv_lock);
}
static int preview_dir(void) {
    if(azpv_dirname[0])return 1;
    DWORD n=GetModuleFileNameW(NULL,azpv_dir,MAX_PATH);
    if(!n||n>=MAX_PATH)return 0;
    while(n&&azpv_dir[n-1]!=L'\\')--n;
    if(!n)return 0;
    char name[80];int m=snprintf(name,sizeof(name),"azimuth_preview_%lu_%llu",
       (unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());
    if(m<=0||(size_t)m>=sizeof(name)||n+(DWORD)m+1>=MAX_PATH)return 0;
    if(!MultiByteToWideChar(CP_UTF8,0,name,-1,azpv_dir+n,(int)(MAX_PATH-n))||!CreateDirectoryW(azpv_dir,NULL))return 0;
    strcpy(azpv_dirname,name);return 1;
}
static int preview_save(AzpvGroup *g,AzpvShot *s) {
    if(!preview_dir())return 0;
    char name[100];int n=snprintf(name,sizeof(name),"f%llu-%s-%ux%u-fmt%u.raw",g->resolve,s->name,s->w,s->h,s->format);
    wchar_t file[MAX_PATH];int m=lstrlenW(azpv_dir);
    if(n<=0||(size_t)n>=sizeof(name)||m+1+n+1>=MAX_PATH)return 0;
    lstrcpyW(file,azpv_dir);file[m++]=L'\\';
    if(!MultiByteToWideChar(CP_UTF8,0,name,-1,file+m,MAX_PATH-m))return 0;
    void *data=NULL;D3D12_RANGE range={0,(SIZE_T)s->total},empty={0,0};
    if(FAILED(s->buffer->lpVtbl->Map(s->buffer,0,&range,&data))||!data)return 0;
    HANDLE out=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    int ok=out!=INVALID_HANDLE_VALUE;
    if(ok){
        for(UINT y=0;y<s->rows;++y){
            DWORD written=0;const BYTE *row=(const BYTE *)data+s->footprint.Offset+(UINT64)y*s->footprint.Footprint.RowPitch;
            if(!WriteFile(out,row,(DWORD)s->row_bytes,&written,NULL)||written!=(DWORD)s->row_bytes){ok=0;break;}
        }
        CloseHandle(out);if(!ok)DeleteFileW(file);
    }
    s->buffer->lpVtbl->Unmap(s->buffer,0,&empty);
    if(ok)log_msg("PREVIEW FILE directory=%s file=%s bytes=%llu",azpv_dirname,name,(unsigned long long)(s->row_bytes*s->rows));
    return ok;
}
static void preview_poll(void) {
    if(!dlss_sr_preview_mode())return;
    AcquireSRWLockExclusive(&azt_lock);
    int ended=azdbg.id!=0&&!azdbg.active;
    ReleaseSRWLockExclusive(&azt_lock);
    AcquireSRWLockExclusive(&azpv_lock);
    if(azpv_done)goto done;
    unsigned pending=0,saved=0,failed=0,files=0;
    uint64_t now=GetTickCount64();
    for(unsigned i=0;i<azpv_count;++i){
        AzpvGroup *g=&azpv_groups[i];
        if(g->state==PV_SUBMITTED){
            UINT64 value=g->fence->lpVtbl->GetCompletedValue(g->fence);
            if(value==UINT64_MAX){g->state=PV_FAILED;log_msg("PREVIEW FAILED resolve=%llu reason=device_removed",g->resolve);}
            else if(value>=1){
                for(unsigned k=0;k<g->count;++k)g->good+=preview_save(g,&g->shots[k])!=0;
                g->state=g->good?PV_SAVED:PV_FAILED;
                log_msg("PREVIEW SAVED resolve=%llu files=%u requested=%u fenceComplete=1",g->resolve,g->good,g->count);
                if(g->good!=g->count)log_msg("PREVIEW PARTIAL resolve=%llu",g->resolve);
            }
        }
        if((g->state==PV_RECORDED||g->state==PV_SUBMITTED)&&now-g->started>=20000){
            g->state=PV_FAILED;log_msg("PREVIEW FAILED resolve=%llu reason=completion_timeout",g->resolve);
        }
        if(g->state==PV_SAVED){++saved;files+=g->good;}
        else if(g->state==PV_FAILED)++failed;
        else ++pending;
    }
    if(ended&&!pending){
        if(!azpv_count)log_msg("PREVIEW NO_GROUPS: no successful selected probe, queue unavailable, or external capture runtime");
        azpv_done=1;log_msg("PREVIEW COMPLETE groups=%u saved=%u failed=%u recordedFiles=%u",azpv_count,saved,failed,files);
    }
done: ReleaseSRWLockExclusive(&azpv_lock);
}
