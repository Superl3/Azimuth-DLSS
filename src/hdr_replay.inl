/* FH2 exact HDR final shader replay. Keep native output color transform,
   curve, sampler, Levels CB and original viewport. t0 and t2 use the
   SAME SR scene outside conservative native HUD regions. No Present hook.
   Offscreen by default; native target replacement requires explicit opt-in. */
#include "hdr_replay_policy.h"
static int azhdr_final_bindings(ListState *s,const AzHdrContract *h,AzvisFrame *v,AzrpBindings *a,AzrpBindings *b,AzrpBindings *f,unsigned long long resolve){
    memset(a,0,sizeof(*a));memset(b,0,sizeof(*b));memset(f,0,sizeof(*f));
    unsigned passes=v->fg_hudless?3u:2u;
    AzrpRootEvidence e;memset(&e,0,sizeof(e));UINT bytes=(UINT)sizeof(e);
    if(!s->trace_graphics_rs||FAILED(s->trace_graphics_rs->lpVtbl->GetPrivateData(s->trace_graphics_rs,&azrp_layout_key,&bytes,&e))||
       bytes!=sizeof(e)||e.overflow||e.rows>AZRP_SCHEMA_ROWS||e.parameters>AZRB_TABLES||(e.flags&0xc00u))return 0;
    AzvisReg req[4]={{0}},sampler={0};
    for(unsigned i=0;i<3;++i)if(!azvis_reg(&e,AZRB_SRV,i,&req[i])||req[i].mode!=AZRB_TABLE)return 0;
    if(!azvis_reg(&e,AZRB_SAMPLER,0,&sampler)||!azhdr_cb0(&e,&req[3]))return 0;
    unsigned count=req[3].mode==AZRB_TABLE?4u:3u;
    unsigned roots[4]={0},offsets[4]={0},base[AZRB_TABLES]={0},span[AZRB_TABLES]={0},total=0;
    uint64_t mask=0;
    for(unsigned i=0;i<count;++i){
        UINT r=req[i].root;roots[i]=r;offsets[i]=req[i].offset;
        if(r>=AZRB_TABLES)return 0;
        if(!(mask&azrb_bit(r))){
            if(!azvis_table_span(&e,r,&span[r])||span[r]>AZRB_HEAP_LIMIT/passes-total)return 0;
            base[r]=total;total+=span[r];mask|=azrb_bit(r);
        }
        if(req[i].offset>=span[r])return 0;
    }
    if(!total||!azhdr_distinct_slots(roots,offsets,count))return 0;
    int ok=0;const char *reason="restore";
    AcquireSRWLockExclusive(&azt_lock);
    AztRoot *gr=azt_root_slot(s->trace_graphics_rs,0),*cr=s->compute_rs?azt_root_slot(s->compute_rs,0):NULL;
    uint64_t gv=azrp_valid_tables(s,gr,s->trace_tables_mask,s->tables),cv=azrp_valid_tables(s,cr,s->trace_compute_mask,s->trace_compute_tables);
    int graphics=azrp_restore_ok(s,gr,s->trace_tables_mask,gv);
    int compute=s->compute_rs?azrp_restore_ok(s,cr,s->trace_compute_mask,cv):s->trace_bindings_known&&!s->trace_bindings_uncertain&&!s->trace_compute_mask;
    if(azt_incomplete||!graphics||!compute||!real_CreateDescriptorHeap||!real_SetComputeRootDescriptorTable||
       !real_CopyDescriptorsSimple||!real_CreateShaderResourceView||!azrp_shadow||!azrp_shadow_inc||s->num_heaps>2)goto done;
    if((gv&mask)!=mask||(sampler.mode==AZRB_TABLE&&!(gv&azrb_bit(sampler.root)))){reason="unbound_tables";goto done;}
    int hi=-1;
    for(UINT i=0;i<s->num_heaps;++i){
        AztHeap *heap=azt_heap_for_ptr(s->heaps[i]);
        if(heap&&heap->type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV){
            if(hi>=0||heap->inc!=azrp_shadow_inc){reason="resource_heap";goto done;}hi=(int)i;
        }
    }
    if(hi<0){reason="resource_heap";goto done;}
    AztDesc *d[4]={0};
    for(unsigned i=0;i<count;++i){
        d[i]=azhdr_table_desc(s,&req[i]);
        if(!d[i]||!d[i]->shadow_valid||!d[i]->shadow||d[i]->shadow_type!=(i==3?AZRB_CBV:AZRB_SRV)){
            reason="descriptor_shadow";goto done;
        }
    }
    if((uintptr_t)d[0]->res!=h->image||(uintptr_t)d[1]->res!=h->curve||(uintptr_t)d[2]->res!=h->scene){
        reason="descriptor_identity";goto done;
    }
    if(req[3].mode==AZRB_DIRECT&&!(s->trace_graphics_cbv_mask&azrb_bit(req[3].root))){reason="direct_cb0";goto done;}
    if(req[3].mode==AZRB_CONSTANT&&(s->trace_graphics_constant_words[req[3].root]&7)!=7){reason="constant_cb0";goto done;}
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=passes*total;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(real_CreateDescriptorHeap(g_device,&hd,&IID_ID3D12DescriptorHeap,(void **)&v->heap))||!v->heap){reason="heap_allocation";goto done;}
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    v->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(v->heap,&cpu);
    v->heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(v->heap,&gpu);
    for(unsigned pass=0;pass<passes;++pass){
        AzrpBindings *o=pass==2?f:pass?b:a;unsigned start=pass*total;
        o->heap=v->heap;o->num_heaps=s->num_heaps;o->mask=mask;
        for(UINT i=0;i<s->num_heaps;++i)o->heaps[i]=(int)i==hi?v->heap:s->heaps[i];
        for(unsigned r=0;r<AZRB_TABLES;++r)if(mask&azrb_bit(r))o->tables[r].ptr=gpu.ptr+(UINT64)(start+base[r])*azrp_shadow_inc;
        if(sampler.mode==AZRB_TABLE){o->mask|=azrb_bit(sampler.root);o->tables[sampler.root]=s->tables[sampler.root];}
        for(unsigned i=0;i<count;++i){
            D3D12_CPU_DESCRIPTOR_HANDLE dst={cpu.ptr+(SIZE_T)(start+base[req[i].root]+req[i].offset)*azrp_shadow_inc};
            if(i==0||(pass&&i==2)){
                D3D12_SHADER_RESOURCE_VIEW_DESC view={0};
                view.Format=DXGI_FORMAT_R10G10B10A2_UNORM;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if(pass){view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;}
                else{view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.MipLevels=1;view.Texture2DArray.ArraySize=3;}
                real_CreateShaderResourceView(g_device,pass==2?v->sr_surface.image:pass?(i==0?v->merged:v->hdr_scene):v->layers,&view,dst);
            }else real_CopyDescriptorsSimple(g_device,1,dst,azrp_shadow_handle(d[i]->shadow),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    log_msg("VISIBLE HDR_BINDINGS resolve=%llu t0Root=%u t1Root=%u t2Root=%u cb0Mode=%u entries=%u nativeCurve=1 nativeLevels=1 graphicsRestore=%d computeRestore=%d",
        resolve,req[0].root,req[1].root,req[2].root,req[3].mode,passes*total,graphics,compute);
    ok=1;
done:
    ReleaseSRWLockExclusive(&azt_lock);
    if(!ok)log_msg("VISIBLE HDR_SKIP resolve=%llu reason=%s graphicsRestore=%d computeRestore=%d",resolve,reason,graphics,compute);
    return ok;
}
static int visible_hdr_finalize(AzvisFrame *v,ID3D12GraphicsCommandList *cl,ListState *s,const AzHdrContract *h,
    ID3D12Resource *target,unsigned long long resolve,int copy_match,int capture){
    AcquireSRWLockShared(&azt_lock);
    int active=azvis_window_locked();
    ReleaseSRWLockShared(&azt_lock);
    int recording=v->pre_ready&&v->composite_ready&&v->list==cl&&v->recording==s->trace_recording_id;
    int contract=h->replay_inputs_ok&&h->scene_ref&&h->image_ref&&
        azvis_texture(h->scene_ref,azrp_profile.rw,azrp_profile.rh)&&azvis_texture(h->image_ref,azrp_profile.rw,azrp_profile.rh);
    int draw=active&&v->enabled&&v->layers&&azvis_texture(target,azrp_profile.ow,azrp_profile.oh)&&!s->vp_jittered;
    int mode=azhdr_route_mode(azvis_hdr_live,capture,contract,copy_match,recording,draw);
    if(!mode){
        log_msg("VISIBLE HDR_SKIP resolve=%llu reason=contract_or_recording contract=%d copyMatch=%d recording=%d draw=%d",resolve,contract,copy_match,recording,draw);
        return 0;
    }
    ID3D12PipelineState *merge=NULL,*scene=NULL;
    AcquireSRWLockShared(&azvis_pso_lock);
    for(unsigned i=0;i<azvis_pso_count;++i)if(azvis_psos[i].original==s->pso){merge=azvis_psos[i].merge;scene=azvis_psos[i].scene;break;}
    ReleaseSRWLockShared(&azvis_pso_lock);
    if(!merge||!scene){log_msg("VISIBLE HDR_SKIP resolve=%llu reason=no_exact_hdr_pso",resolve);return 0;}
    v->merged=azvis_new_texture(1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    v->hdr_scene=azvis_new_texture(1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    if(mode==2)v->probe_final=azvis_new_texture(1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    if(!v->merged||!v->hdr_scene||(mode==2&&!v->probe_final))return 0;
    if(azvis_fg_probe&&capture&&mode==1)v->fg_hudless=azvis_new_texture(1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=(mode==2?3u:2u)+(v->fg_hudless?1u:0u);
    if(!real_CreateDescriptorHeap||FAILED(real_CreateDescriptorHeap(g_device,&hd,&IID_ID3D12DescriptorHeap,(void **)&v->rtv))||!v->rtv)return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];v->rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(v->rtv,&rtv[0]);
    UINT inc=ID3D12Device_GetDescriptorHandleIncrementSize(g_device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtv[1].ptr=rtv[0].ptr+inc;rtv[2].ptr=rtv[1].ptr+inc;rtv[3].ptr=rtv[2].ptr+inc;
    real_CreateRenderTargetView(g_device,v->merged,NULL,rtv[0]);
    real_CreateRenderTargetView(g_device,v->hdr_scene,NULL,rtv[1]);
    if(mode==2)real_CreateRenderTargetView(g_device,v->probe_final,NULL,rtv[2]);
    D3D12_CPU_DESCRIPTOR_HANDLE fg_rtv=rtv[mode==2?3u:2u];
    if(v->fg_hudless)real_CreateRenderTargetView(g_device,v->fg_hudless,NULL,fg_rtv);
    AzrpBindings a,b,f;
    if(!azhdr_final_bindings(s,h,v,&a,&b,&f,resolve))return 0;
    /* Native final draw already ran. Snapshot the exact current inputs,
       not an earlier possibly stale composite. Only t0/t2 transition;
       t1 and CB0 remain in the native state. */
    azvis_copy_layer_state(cl,v->layers,1,h->scene_ref,h->scene_state);
    azvis_copy_layer_state(cl,v->layers,2,h->image_ref,h->image_state);
    hooks_transition(cl,v->layers,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    real_SetDescriptorHeaps(cl,a.num_heaps,a.heaps);azvis_set_tables(cl,&a);
    real_SetPipelineState(cl,merge);real_OMSetRenderTargets(cl,1,&rtv[0],FALSE,NULL);
    real_DrawInstanced(cl,3,1,0,0);
    hooks_transition(cl,v->merged,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    real_SetPipelineState(cl,scene);real_OMSetRenderTargets(cl,1,&rtv[1],FALSE,NULL);
    real_DrawInstanced(cl,3,1,0,0);
    hooks_transition(cl,v->hdr_scene,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    azvis_set_tables(cl,&b);real_SetPipelineState(cl,s->pso);
    if(mode==2)real_OMSetRenderTargets(cl,1,&rtv[2],FALSE,NULL);
    else real_OMSetRenderTargets(cl,s->num_rtv,s->rtv,s->rtv_single,NULL);
    real_DrawInstanced(cl,3,1,0,0);
    if(v->fg_hudless)azvis_fg_draw(v,cl,s,&f,fg_rtv,resolve);
    real_OMSetRenderTargets(cl,s->num_rtv,s->rtv,s->rtv_single,NULL);
    real_SetDescriptorHeaps(cl,s->num_heaps,s->heaps);
    for(UINT i=0;i<AZRB_TABLES;++i){
        if(s->trace_tables_mask&azrb_bit(i))real_SetGraphicsRootDescriptorTable(cl,i,s->tables[i]);
        if(s->trace_compute_mask&azrb_bit(i))real_SetComputeRootDescriptorTable(cl,i,s->trace_compute_tables[i]);
    }
    log_msg("VISIBLE HDR_REPLAY resolve=%llu mode=%s size=%ux%u scene=%p composite=%p nativeFinalShader=1 nativeCurve=1 nativeLevels=1 outputMath=BT2020_PQ outputColorSpace=unknown presentVerified=0 hudPolicy=%s",
        resolve,mode==1?"live":"probe",azrp_profile.ow,azrp_profile.oh,(void *)v->hdr_scene,(void *)v->merged,azvis_hud_delta?"delta_lower_bound":"native_region");
    if(mode==2){log_msg("VISIBLE PROBE_FINAL_RECORDED resolve=%llu list=%p recording=%llu target=%p qpc=%lld nativePreserved=1 presentVerified=0 hdr=1",
        resolve,(void *)cl,v->recording,(void *)v->probe_final,azvis_qpc());return 2;}
    v->routed=1;v->target_id=(uintptr_t)target;
    log_msg("VISIBLE FINAL_RECORDED resolve=%llu list=%p recording=%llu target=%p qpc=%lld nativeFinalShader=1 marker=%d presentVerified=0 hdr=1",
        resolve,(void *)cl,v->recording,(void *)target,azvis_qpc(),azvis_marker);
    return 1;
}
