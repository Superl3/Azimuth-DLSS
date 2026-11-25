/* Read-only validation for the exact FH2 PQ final shader. The caller
   already matched the full PS/VS hashes. No root/descriptor replacement,
   no guessed color space, no swapchain refs, no mapping of native CBVs.
   Only the exact t2 resource may be retained for bounded readback. */
typedef struct {
    uintptr_t scene,curve,image;
    D3D12_RESOURCE_STATES scene_state;
    int scene_readable,inputs_ok;
    ID3D12Resource *scene_ref;
} AzHdrContract;
static AztDesc *azhdr_table_desc(ListState *s,const AzvisReg *reg){
    if(reg->mode!=AZRB_TABLE||reg->root>=AZRB_TABLES||!(s->trace_tables_mask&azrb_bit(reg->root)))return NULL;
    for(UINT i=0;i<s->num_heaps&&i<2;++i){
        AztHeap *h=azt_heap_for_ptr(s->heaps[i]);
        uintptr_t cpu=0;
        if(h&&h->type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV&&azroute_descriptor_cpu(s->tables[reg->root].ptr,h->gpu,h->cpu,h->inc,h->count,reg->offset,&cpu))return azt_slot(h,(SIZE_T)cpu,0);
    }
    return NULL;
}
static int azhdr_read_state(ID3D12GraphicsCommandList *cl,ListState *s,AztDesc *d,D3D12_RESOURCE_STATES *state){
    if(!d||!d->res)return 0;
    AztState *a=azt_state(d->res,0);
    if(!a||!a->known||a->list!=cl||a->recording!=s->trace_recording_id)return 0;
    *state=a->state;
    /* Only the exact observed compatible read states; no guessed
       promotion, mixed write states, or implicit cross-queue ordering. */
    return *state==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE||
        *state==(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
static int azhdr_texture(AztDesc *d,UINT dim,int scene){
    if(!d||!d->res||!d->shadow_valid||d->shadow_type!=AZRB_SRV||
       d->desc.Dimension!=dim||d->desc.DepthOrArraySize!=1||d->desc.SampleDesc.Count!=1)return 0;
    if(scene)return d->simple&&d->format==DXGI_FORMAT_R10G10B10A2_UNORM&&
        azrp_match_render(azrp_profile,d->desc.Width,d->desc.Height)&&d->desc.MipLevels==1;
    return d->desc.Width>=1025;
}
static int azhdr_cb0(const AzrpRootEvidence *e,AzvisReg *out){
    unsigned hits=0;
    for(UINT i=0;i<e->rows;++i){
        const AzrpSchemaRow *x=&e->row[i];
        if(x->space||x->type!=AZRB_CBV||!azrb_vis(x->visibility,AZRB_PS))continue;
        if(x->kind==AZRB_TABLE&&x->reg==0&&x->count&&x->offset!=UINT_MAX&&x->root<e->parameters){
            *out=(AzvisReg){x->root,x->offset,AZRB_TABLE};++hits;
        }else if(x->reg==0&&x->root<e->parameters&&(x->kind==AZRB_DIRECT||(x->kind==AZRB_CONSTANT&&x->count>=3))){
            *out=(AzvisReg){x->root,0,x->kind};++hits;
        }
    }
    return hits==1;
}
/* Called under azt_lock with the native current draw bindings still live.
   t0/t2 are the same pre-final encoded domain; never label them PQ.
   The shader's output math is PQ, but DXGI signal/color space remains
   unknown. A valid input report is not a routing permit. */
static AzHdrContract azhdr_inspect(ID3D12GraphicsCommandList *cl,ListState *s,unsigned long long resolve,uintptr_t target){
    AzHdrContract c={0};
    AzrpRootEvidence e;memset(&e,0,sizeof(e));UINT bytes=(UINT)sizeof(e);
    if(!s->trace_graphics_rs||FAILED(s->trace_graphics_rs->lpVtbl->GetPrivateData(s->trace_graphics_rs,&azrp_layout_key,&bytes,&e))||
       bytes!=sizeof(e)||e.overflow||e.rows>AZRP_SCHEMA_ROWS||e.parameters>AZRB_TABLES||(e.flags&0xc00u)){
        log_msg("POSTFX HDR_CONTRACT resolve=%llu root=%p reason=root_schema_missing nativePreserved=1 outputColorSpace=unknown",resolve,(void *)s->trace_graphics_rs);return c;
    }
    AzvisReg t[3]={{0}},sampler={0},cb={0};AztDesc *d[3]={0};int mapped[3]={0},shape[3]={0},read[3]={0};
    D3D12_RESOURCE_STATES state[3]={0};
    for(UINT i=0;i<3;++i){
        mapped[i]=azvis_reg(&e,AZRB_SRV,i,&t[i])&&t[i].mode==AZRB_TABLE;
        if(mapped[i])d[i]=azhdr_table_desc(s,&t[i]);
        shape[i]=azhdr_texture(d[i],i==1?D3D12_RESOURCE_DIMENSION_TEXTURE1D:D3D12_RESOURCE_DIMENSION_TEXTURE2D,i!=1);
        read[i]=azhdr_read_state(cl,s,d[i],&state[i]);
        log_msg("POSTFX HDR_REQ resolve=%llu reg=t%u mapped=%d root=%u offset=%u resource=%p dimension=%u size=%llux%u format=%u shape=%d shadow=%d readStateKnown=%d state=%u",resolve,i,mapped[i],t[i].root,t[i].offset,d[i]?(void *)d[i]->res:NULL,d[i]?(unsigned)d[i]->desc.Dimension:0,d[i]?(unsigned long long)d[i]->desc.Width:0,d[i]?d[i]->desc.Height:0,d[i]?(unsigned)d[i]->format:0,shape[i],d[i]?d[i]->shadow_valid:0,read[i],(unsigned)state[i]);
    }
    AztRoot *root=azt_root_slot(s->trace_graphics_rs,0);
    uint64_t valid=azrp_valid_tables(s,root,s->trace_tables_mask,s->tables);
    int sok=azvis_reg(&e,AZRB_SAMPLER,0,&sampler)&&(sampler.mode==AZRB_STATIC||(sampler.mode==AZRB_TABLE&&(valid&azrb_bit(sampler.root))));
    int cbmap=azhdr_cb0(&e,&cb),cbok=0;
    if(cbmap){
        if(cb.mode==AZRB_DIRECT)cbok=(s->trace_graphics_cbv_mask&azrb_bit(cb.root))!=0;
        else if(cb.mode==AZRB_CONSTANT)cbok=(s->trace_graphics_constant_words[cb.root]&7)==7;
        else{AztDesc *x=azhdr_table_desc(s,&cb);cbok=x&&x->shadow_valid&&x->shadow_type==AZRB_CBV;}
    }
    c.image=d[0]?(uintptr_t)d[0]->res:0;c.curve=d[1]?(uintptr_t)d[1]->res:0;c.scene=d[2]?(uintptr_t)d[2]->res:0;
    c.scene_state=state[2];c.scene_readable=!azt_incomplete&&shape[2]&&read[2]&&c.scene&&c.scene!=target&&c.scene!=c.image;
    if(c.scene_readable){c.scene_ref=d[2]->res;ID3D12Resource_AddRef(c.scene_ref);}
    c.inputs_ok=!azt_incomplete&&shape[0]&&shape[1]&&shape[2]&&read[0]&&read[1]&&read[2]&&sok&&cbok&&c.scene!=c.image&&c.scene!=target&&c.image!=target;
    log_msg("POSTFX HDR_CONTRACT resolve=%llu list=%p recording=%llu root=%p t0=%p t1=%p t2=%p target=%p samplerMapped=%d samplerMode=%u samplerRoot=%u cb0Mapped=%d cb0Mode=%u cb0Root=%u cb0Observed=%d levelsValuesKnown=0 samplerValuesKnown=0 inputsVerified=%d t2Readable=%d inputDomain=pre_final_encoded outputMath=BT2020_PQ outputColorSpace=unknown nativePreserved=1",resolve,(void *)cl,s->trace_recording_id,(void *)s->trace_graphics_rs,(void *)c.image,(void *)c.curve,(void *)c.scene,(void *)target,sok,sampler.mode,sampler.root,cbmap,cb.mode,cb.root,cbok,c.inputs_ok,c.scene_readable);
    return c;
}
