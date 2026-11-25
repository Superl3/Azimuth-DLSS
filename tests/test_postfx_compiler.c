/* Windows SDK compiler fixture: verifies real DXBC checksum and disassembly.
   No GPU, proprietary game shader or driver-dependent PSO is required. */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <stdio.h>
#include "../src/postfx_dxbc.h"
#include "../src/visible_merge_shader.h"
int main(void) {
    typedef HRESULT(WINAPI *CompileFn)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO *,ID3DInclude *,LPCSTR,LPCSTR,UINT,UINT,ID3DBlob **,ID3DBlob **);
    typedef HRESULT(WINAPI *DisasmFn)(LPCVOID,SIZE_T,UINT,LPCSTR,ID3DBlob **);
    HMODULE mod=LoadLibraryExW(L"d3dcompiler_47.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    assert(mod);
    CompileFn compile=(CompileFn)(void *)GetProcAddress(mod,"D3DCompile");
    DisasmFn disasm=(DisasmFn)(void *)GetProcAddress(mod,"D3DDisassemble");
    assert(compile&&disasm);
    const char *source="Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target { return t.Load(int3(int2(p.xy),0)); }";
    ID3DBlob *code=NULL,*err=NULL,*asm0=NULL,*asm1=NULL;
    HRESULT hr=compile(source,strlen(source),"fixture",NULL,NULL,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&err);
    assert(SUCCEEDED(hr)&&code);
    if(err)ID3D10Blob_Release(err);
    const unsigned char *bytes=(const unsigned char *)ID3D10Blob_GetBufferPointer(code);
    size_t size=ID3D10Blob_GetBufferSize(code);unsigned char hash[16];assert(size>20);
    azdx_retail(bytes+20,size-20,hash);assert(!memcmp(bytes+4,hash,16));
    assert(SUCCEEDED(disasm(bytes,size,0,NULL,&asm0))&&asm0);
    void *out=HeapAlloc(GetProcessHeap(),0,size+40);assert(out);
    size_t n=azdx_patch_positions(bytes,size,out,size+40,2.0f/3.0f,2.0f/3.0f,1);
    if(!n)fprintf(stderr,"Unexpected compiler fixture tokens:\n%s\n",(const char *)ID3D10Blob_GetBufferPointer(asm0));
    assert(n==size+40);
    assert(SUCCEEDED(disasm(out,n,0,NULL,&asm1))&&asm1);
    const char *text=(const char *)ID3D10Blob_GetBufferPointer(asm1);
    assert(strstr(text,"mul ")&&strstr(text,"ftoi ")&&strstr(text,"0.666667"));
    ID3D10Blob_Release(asm1);ID3D10Blob_Release(asm0);ID3D10Blob_Release(code);
    HeapFree(GetProcessHeap(),0,out);
    code=NULL;err=NULL;
    hr=compile(azvis_merge_hlsl,strlen(azvis_merge_hlsl),"visible_merge",NULL,NULL,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&err);
    if(FAILED(hr)&&err)fprintf(stderr,"Visible shader: %s\n",(const char *)ID3D10Blob_GetBufferPointer(err));
    assert(SUCCEEDED(hr)&&code);
    if(err)ID3D10Blob_Release(err);
    ID3D10Blob_Release(code);FreeLibrary(mod);
    puts("PASS: real D3DCompile retail checksum and position-remapped DXBC disassembly");
    return 0;
}
