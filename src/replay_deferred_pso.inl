/* Only validated PSOs, with all pointer-bearing description fields owned. */
typedef struct {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    D3D12_INPUT_ELEMENT_DESC elements[16];char names[16][64];
    void *ps,*vs;ID3D12PipelineState *original;
} AzrpDeferred;
static void azrp_free_deferred(AzrpDeferred *x){
    if(!x)return;
    if(x->d.pRootSignature)ID3D12RootSignature_Release(x->d.pRootSignature);
    if(x->original)ID3D12PipelineState_Release(x->original);
    if(x->ps)HeapFree(GetProcessHeap(),0,x->ps);
    if(x->vs)HeapFree(GetProcessHeap(),0,x->vs);
    HeapFree(GetProcessHeap(),0,x);
}
static AzrpDeferred *azrp_hold_deferred(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d,ID3D12PipelineState *pso){
    if(!d->pRootSignature||!pso||!d->VS.pShaderBytecode||!d->PS.pShaderBytecode||
       !d->VS.BytecodeLength||!d->PS.BytecodeLength||d->VS.BytecodeLength>1048576||d->PS.BytecodeLength>1048576||
       d->GS.BytecodeLength||d->HS.BytecodeLength||d->DS.BytecodeLength||d->StreamOutput.NumEntries||
       d->StreamOutput.NumStrides||d->InputLayout.NumElements>16||(d->InputLayout.NumElements&&!d->InputLayout.pInputElementDescs))return NULL;
    AzrpDeferred *x=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*x));
    if(!x)return NULL;
    x->d=*d;ID3D12RootSignature_AddRef(x->d.pRootSignature);
    ID3D12PipelineState_AddRef(pso);x->original=pso;
    x->ps=HeapAlloc(GetProcessHeap(),0,d->PS.BytecodeLength);x->vs=HeapAlloc(GetProcessHeap(),0,d->VS.BytecodeLength);
    if(!x->ps||!x->vs)goto fail;
    memcpy(x->ps,d->PS.pShaderBytecode,d->PS.BytecodeLength);memcpy(x->vs,d->VS.pShaderBytecode,d->VS.BytecodeLength);
    x->d.PS.pShaderBytecode=x->ps;x->d.VS.pShaderBytecode=x->vs;
    x->d.GS.pShaderBytecode=x->d.HS.pShaderBytecode=x->d.DS.pShaderBytecode=NULL;
    x->d.CachedPSO.pCachedBlob=NULL;x->d.CachedPSO.CachedBlobSizeInBytes=0;
    x->d.StreamOutput.pSODeclaration=NULL;x->d.StreamOutput.pBufferStrides=NULL;
    x->d.InputLayout.pInputElementDescs=d->InputLayout.NumElements?x->elements:NULL;
    for(UINT i=0;i<d->InputLayout.NumElements;++i){
        const D3D12_INPUT_ELEMENT_DESC *e=&d->InputLayout.pInputElementDescs[i];
        if(!e->SemanticName||strlen(e->SemanticName)>=sizeof(x->names[i]))goto fail;
        strcpy(x->names[i],e->SemanticName);x->elements[i]=*e;x->elements[i].SemanticName=x->names[i];
    }
    return x;
fail: azrp_free_deferred(x);return NULL;
}
static int azrp_defer(AzrpDeferred **slots,unsigned cap,const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d,ID3D12PipelineState *pso){
    for(unsigned i=0;i<cap;++i)if(slots[i]&&slots[i]->original==pso)return 1;
    for(unsigned i=0;i<cap;++i)if(!slots[i]){slots[i]=azrp_hold_deferred(d,pso);return slots[i]!=NULL;}
    return 0;
}
