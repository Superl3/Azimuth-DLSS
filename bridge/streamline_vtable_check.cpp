// Fail the build if Windows SDK COM layout differs from the native hooks.
#define CINTERFACE
#include <windows.h>
#include <d3d12.h>
#include <cstddef>
#define SLOT(T,M,N) static_assert(offsetof(T##Vtbl,M)==(N)*sizeof(void*),#T "." #M)
SLOT(ID3D12GraphicsCommandList,Reset,10);SLOT(ID3D12GraphicsCommandList,ClearState,11);
SLOT(ID3D12GraphicsCommandList,DrawInstanced,12);SLOT(ID3D12GraphicsCommandList,DrawIndexedInstanced,13);
SLOT(ID3D12GraphicsCommandList,CopyTextureRegion,16);SLOT(ID3D12GraphicsCommandList,CopyResource,17);
SLOT(ID3D12GraphicsCommandList,ResolveSubresource,19);SLOT(ID3D12GraphicsCommandList,ResourceBarrier,26);
SLOT(ID3D12GraphicsCommandList,ExecuteBundle,27);SLOT(ID3D12GraphicsCommandList,OMSetRenderTargets,46);
SLOT(ID3D12GraphicsCommandList,ClearRenderTargetView,48);SLOT(ID3D12GraphicsCommandList,ClearUnorderedAccessViewUint,49);
SLOT(ID3D12GraphicsCommandList,ClearUnorderedAccessViewFloat,50);SLOT(ID3D12GraphicsCommandList,DiscardResource,51);
SLOT(ID3D12Device,CreateRenderTargetView,20);SLOT(ID3D12Device,CopyDescriptors,23);
SLOT(ID3D12Device,CopyDescriptorsSimple,24);SLOT(ID3D12CommandQueue,ExecuteCommandLists,10);
