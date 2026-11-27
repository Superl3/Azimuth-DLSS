/* Explicit FH2 Streamline managed-chain opt-in. The SDK stays in an
   independently compiled MSVC sidecar; the normal ReShade proxy is untouched.
   Borrowed chain identity only; no permanent swapchain/backbuffer refs. */
typedef HRESULT (__cdecl *AzslCreate)(IDXGIFactory2 *,IUnknown *,HWND,const DXGI_SWAP_CHAIN_DESC1 *,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *,IDXGIOutput *,IDXGISwapChain1 **);
static _Thread_local int azsl_in_factory;
static volatile LONG azsl_selected,azsl_profile_ready;
static IDXGISwapChain1 *azsl_managed_chain;
static int (__cdecl *azsl_ready)(void);
static int (__cdecl *azsl_native_chain)(IDXGISwapChain1 *,IDXGISwapChain1 **);
static void (__cdecl *azsl_change)(IDXGISwapChain1 *,int,unsigned,HRESULT);
static uint64_t (__cdecl *azsl_camera_begin)(ID3D12Device *,ID3D12GraphicsCommandList *,uint64_t,uint64_t,uint64_t,D3D12_CPU_DESCRIPTOR_HANDLE *);
static void (__cdecl *azsl_camera_end)(ID3D12GraphicsCommandList *,uint64_t);
static void (__cdecl *azsl_core)(ID3D12GraphicsCommandList *,ID3D12Resource *,ID3D12Resource *,unsigned,uint64_t,uint64_t,uint64_t,int,int,int);
static int (__cdecl *azsl_wants_final)(uint64_t);
static void (__cdecl *azsl_final)(ID3D12GraphicsCommandList *,ID3D12Resource *,ID3D12Resource *,uint64_t,uint64_t);
static int (__cdecl *azsl_before)(IDXGISwapChain1 *,UINT,uint64_t);
static void (__cdecl *azsl_after)(HRESULT);
static int azsl_requested(void){
    wchar_t path[1024];DWORD n=GetEnvironmentVariableW(L"FH2_STREAMLINE_CHAIN_PROBE",path,ARRAYSIZE(path));
    return n&&n<ARRAYSIZE(path);
}
static int azsl_live_available(void){return azsl_managed_chain&&InterlockedCompareExchange(&azsl_profile_ready,0,0);}
static void azsl_update_ready(void){InterlockedExchange(&azsl_profile_ready,azsl_ready&&azsl_ready());}
static int azsl_chain_probe(IDXGIFactory2 *factory,IUnknown *queue,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1 *desc,
                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen,IDXGIOutput *output,IDXGISwapChain1 **chain,HRESULT *result){
    if(!desc||!queue||!hwnd||!chain||desc->Width!=2592||desc->Height!=1080||desc->BufferCount!=3||
       desc->Format!=DXGI_FORMAT_R10G10B10A2_UNORM||desc->SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD||
       (fullscreen&&!fullscreen->Windowed))return 0;
    wchar_t path[1024];DWORD n=GetEnvironmentVariableW(L"FH2_STREAMLINE_CHAIN_PROBE",path,ARRAYSIZE(path));
    if(!n||n>=ARRAYSIZE(path)||InterlockedCompareExchange(&azsl_selected,1,0)!=0)return 0;
    HMODULE m=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    AzslCreate create=m?(AzslCreate)(void *)GetProcAddress(m,"azslCreateManagedHdrChain"):NULL;
    if(!create){log_msg("STREAMLINE CHAIN_PREPARE sidecar_not_loaded win32=%lu",(unsigned long)GetLastError());return 0;}
#define AZSL_LOAD(name,exp) *(FARPROC *)&name=GetProcAddress(m,exp)
    AZSL_LOAD(azsl_ready,"azslReady");AZSL_LOAD(azsl_native_chain,"azslNativeChain");AZSL_LOAD(azsl_change,"azslChainChange");
    AZSL_LOAD(azsl_camera_begin,"azslCameraBegin");AZSL_LOAD(azsl_camera_end,"azslCameraEnd");
    AZSL_LOAD(azsl_core,"azslCore");AZSL_LOAD(azsl_wants_final,"azslWantsFinal");AZSL_LOAD(azsl_final,"azslFinal");
    AZSL_LOAD(azsl_before,"azslBeforePresent");AZSL_LOAD(azsl_after,"azslAfterPresent");
#undef AZSL_LOAD
    if(!azsl_ready||!azsl_native_chain||!azsl_change||!azsl_camera_begin||!azsl_camera_end||
       !azsl_core||!azsl_wants_final||!azsl_final||!azsl_before||!azsl_after){log_msg("STREAMLINE CHAIN_PREPARE missing_exports");return 0;}
    *chain=NULL;azsl_in_factory=1;
    *result=create(factory,queue,hwnd,desc,fullscreen,output,chain);
    azsl_in_factory=0;
    log_msg("STREAMLINE CHAIN_PREPARE chain=%p queue=%p hr=%08lx",(void *)*chain,(void *)queue,(unsigned long)*result);
    if(SUCCEEDED(*result)&&*chain){azsl_managed_chain=*chain;azsl_update_ready();return 1;}
    if(*chain){IDXGISwapChain1_Release(*chain);*chain=NULL;}return 0;
}
