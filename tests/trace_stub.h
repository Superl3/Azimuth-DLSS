#ifndef AZT_TEST_STUB_H
#define AZT_TEST_STUB_H
/* Host-only doubles for trace metadata tests. Deliberately NOT the Windows ABI. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>
#include "../src/sr_experiment.h"
typedef unsigned UINT;typedef uint32_t DWORD;typedef uint64_t UINT64;typedef uintptr_t SIZE_T;typedef long LONG;typedef unsigned char BYTE;
typedef int HRESULT;typedef int DXGI_FORMAT;typedef unsigned D3D12_DESCRIPTOR_HEAP_TYPE;typedef unsigned D3D12_RESOURCE_STATES;typedef const void *REFIID;typedef const void *LPCVOID;typedef void *HMODULE;
#define WINAPI
#define FAILED(hr) ((hr)<0)
#define SRWLOCK_INIT 0
#define SRWLOCK int
#define AcquireSRWLockExclusive(x) ((void)(x))
#define ReleaseSRWLockExclusive(x) ((void)(x))
static DWORD GetEnvironmentVariableA(const char *n,char *b,DWORD size){(void)n;(void)b;(void)size;return 0;}
static HMODULE GetModuleHandleW(const void *s){(void)s;return NULL;}
static void *GetProcAddress(HMODULE m,const char *s){(void)m;(void)s;return NULL;}
#define DXGI_FORMAT_UNKNOWN 0
#define DXGI_FORMAT_R11G11B10_FLOAT 26
#define D3D12_RESOURCE_DIMENSION_TEXTURE2D 3
#define D3D12_RTV_DIMENSION_TEXTURE2D 4
#define D3D12_SRV_DIMENSION_TEXTURE2D 4
#define D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING 5768
#define D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV 0
#define D3D12_DESCRIPTOR_HEAP_TYPE_RTV 2
#define D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE 1
#define D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND UINT_MAX
#define D3D12_DESCRIPTOR_RANGE_TYPE_SRV 0
#define D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE 0
#define D3D12_SHADER_VISIBILITY_ALL 0
#define D3D12_SHADER_VISIBILITY_PIXEL 5
#define D3D_ROOT_SIGNATURE_VERSION_1_0 1
#define D3D_ROOT_SIGNATURE_VERSION_1_1 2
#define D3D12_COLOR_WRITE_ENABLE_ALL 15
#define D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE 3
#define D3D12_FILL_MODE_SOLID 3
#define D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST 4
#define D3D12_RESOURCE_BARRIER_TYPE_TRANSITION 0
#define D3D12_RESOURCE_BARRIER_TYPE_ALIASING 1
#define D3D12_RESOURCE_BARRIER_FLAG_NONE 0
#define D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES UINT_MAX
#define D3D12_RESOURCE_STATE_RENDER_TARGET 4u
#define D3D12_RESOURCE_STATE_UNORDERED_ACCESS 8u
#define D3D12_RESOURCE_STATE_COPY_DEST 1024u
#define D3D12_RESOURCE_STATE_COPY_SOURCE 2048u
#define D3D12_RESOURCE_STATE_RESOLVE_DEST 4096u
#define D3D12_COMMAND_LIST_TYPE_DIRECT 0
#define ROLE_NONE 0
#define ROLE_RESOLVE 1
#define ROLE_MV 2

typedef struct { SIZE_T ptr; } D3D12_CPU_DESCRIPTOR_HANDLE;
typedef struct { UINT64 ptr; } D3D12_GPU_DESCRIPTOR_HANDLE;
typedef struct { unsigned Type, NumDescriptors, Flags; } D3D12_DESCRIPTOR_HEAP_DESC;
typedef struct { unsigned Type; } D3D12_COMMAND_QUEUE_DESC;
typedef struct { unsigned Dimension;UINT64 Width;unsigned Height,DepthOrArraySize,MipLevels;DXGI_FORMAT Format;struct{UINT Count,Quality;}SampleDesc;UINT Layout,Flags; } D3D12_RESOURCE_DESC;
typedef struct { DXGI_FORMAT Format;unsigned ViewDimension;struct{unsigned MipSlice,PlaneSlice;}Texture2D; } D3D12_RENDER_TARGET_VIEW_DESC;
typedef struct { DXGI_FORMAT Format;unsigned ViewDimension,Shader4ComponentMapping;struct{unsigned MostDetailedMip,PlaneSlice;float ResourceMinLODClamp;}Texture2D; } D3D12_SHADER_RESOURCE_VIEW_DESC;
typedef struct ID3D12DescriptorHeap ID3D12DescriptorHeap;
typedef struct {void (*GetCPUDescriptorHandleForHeapStart)(ID3D12DescriptorHeap *,D3D12_CPU_DESCRIPTOR_HANDLE *);void (*GetGPUDescriptorHandleForHeapStart)(ID3D12DescriptorHeap *,D3D12_GPU_DESCRIPTOR_HANDLE *);} StubHeapVtbl;
struct ID3D12DescriptorHeap{StubHeapVtbl *lpVtbl;SIZE_T cpu;UINT64 gpu;};
typedef struct ID3D12Resource ID3D12Resource;
typedef struct {void (*GetDesc)(ID3D12Resource *,D3D12_RESOURCE_DESC *);}StubResVtbl;
struct ID3D12Resource{StubResVtbl *lpVtbl;D3D12_RESOURCE_DESC desc;};
typedef struct{int dummy;}ID3D12RootSignature,ID3D12PipelineState,ID3D12GraphicsCommandList,ID3D12CommandQueue,ID3D12Device;
#define ID3D12Device_GetDescriptorHandleIncrementSize(dev,type) ((void)(dev),(void)(type),32u)
typedef struct {UINT RangeType,NumDescriptors,BaseShaderRegister,RegisterSpace,OffsetInDescriptorsFromTableStart;}D3D12_DESCRIPTOR_RANGE,D3D12_DESCRIPTOR_RANGE1;
typedef struct {UINT ParameterType,ShaderVisibility;struct{UINT NumDescriptorRanges;const D3D12_DESCRIPTOR_RANGE *pDescriptorRanges;}DescriptorTable;}D3D12_ROOT_PARAMETER;
typedef struct {UINT ParameterType,ShaderVisibility;struct{UINT NumDescriptorRanges;const D3D12_DESCRIPTOR_RANGE1 *pDescriptorRanges;}DescriptorTable;}D3D12_ROOT_PARAMETER1;
typedef struct {UINT NumParameters,Flags;const D3D12_ROOT_PARAMETER *pParameters;}D3D12_ROOT_SIGNATURE_DESC;
typedef struct {UINT NumParameters,Flags;const D3D12_ROOT_PARAMETER1 *pParameters;}D3D12_ROOT_SIGNATURE_DESC1;
typedef struct {UINT Version;D3D12_ROOT_SIGNATURE_DESC Desc_1_0;D3D12_ROOT_SIGNATURE_DESC1 Desc_1_1;}D3D12_VERSIONED_ROOT_SIGNATURE_DESC;
typedef struct ID3D12VersionedRootSignatureDeserializer ID3D12VersionedRootSignatureDeserializer;
typedef struct {const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *(*GetUnconvertedRootSignatureDesc)(ID3D12VersionedRootSignatureDeserializer *);UINT (*Release)(ID3D12VersionedRootSignatureDeserializer *);}StubDesVtbl;
struct ID3D12VersionedRootSignatureDeserializer{StubDesVtbl *lpVtbl;};
static const int IID_ID3D12VersionedRootSignatureDeserializer=0;
typedef struct {UINT BlendEnable,LogicOpEnable,RenderTargetWriteMask;}D3D12_RENDER_TARGET_BLEND_DESC;
typedef struct {const void *pShaderBytecode;SIZE_T BytecodeLength;}StubBytecode;
typedef struct {ID3D12RootSignature *pRootSignature;StubBytecode PS,VS,GS,HS,DS;UINT NumRenderTargets;DXGI_FORMAT RTVFormats[8],DSVFormat;struct{UINT DepthEnable,StencilEnable;}DepthStencilState;struct{D3D12_RENDER_TARGET_BLEND_DESC RenderTarget[8];UINT AlphaToCoverageEnable;}BlendState;UINT SampleMask,PrimitiveTopologyType;struct{UINT Count,Quality;}SampleDesc;struct{UINT FillMode;}RasterizerState;struct{UINT NumEntries;}StreamOutput;}D3D12_GRAPHICS_PIPELINE_STATE_DESC;
typedef struct {UINT Type,Flags;struct{ID3D12Resource *pResource;UINT Subresource;D3D12_RESOURCE_STATES StateAfter;}Transition;struct{ID3D12Resource *pResourceAfter,*pResourceBefore;}Aliasing;}D3D12_RESOURCE_BARRIER;
typedef struct {float TopLeftX,TopLeftY,Width,Height;}StubVP;
typedef struct {LONG left,top,right,bottom;}StubRect;
typedef struct {ID3D12PipelineState *pso;UINT num_rtv,rtv_single,has_dsv,num_vp,num_sc,num_heaps;D3D12_CPU_DESCRIPTOR_HANDLE rtv[8];StubVP vp[4];StubRect sc[4];ID3D12DescriptorHeap *heaps[2];ID3D12RootSignature *trace_graphics_rs;UINT trace_tables_mask;D3D12_GPU_DESCRIPTOR_HANDLE tables[32];UINT trace_topology,trace_predicated,trace_queries,trace_query_unknown;unsigned long long trace_recording_id;}ListState;
static AzSRMode stub_mode=AZSR_ROUTE;static int stub_enabled=1,stub_copies,stub_transitions,stub_visible;
static ID3D12Resource *stub_src,*stub_dst;
static AzSRMode dlss_sr_mode(void){return stub_mode;}
static int dlss_enabled(void){return stub_enabled;}
static int dlss_sr_trace_only(void){return stub_mode==AZSR_DIAGNOSTIC;}
static void dlss_sr_route_result(int visible){stub_visible=visible;}
static void log_msg(const char *fmt,...){(void)fmt;}
static void hooks_transition(ID3D12GraphicsCommandList *cl,ID3D12Resource *r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){(void)cl;(void)r;(void)a;(void)b;++stub_transitions;}
static void stub_copy(ID3D12GraphicsCommandList *cl,ID3D12Resource *dst,ID3D12Resource *src){(void)cl;++stub_copies;stub_src=src;stub_dst=dst;}
static void (*real_CopyResource)(ID3D12GraphicsCommandList *,ID3D12Resource *,ID3D12Resource *)=stub_copy;
#endif
