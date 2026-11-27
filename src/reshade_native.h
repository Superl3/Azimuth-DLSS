#pragma once
#include <windows.h>
#include <unknwn.h>
/* ReShade 6.8.0 native interface used in the supplied successful SR trial. */
static inline HRESULT az_reshade_native(IUnknown *object,REFIID iid,void **out){
    static const GUID unwrap={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
    IUnknown *original=NULL;
    if(!object||!out)return E_INVALIDARG;
    *out=NULL;
    HRESULT hr=IUnknown_QueryInterface(object,&unwrap,(void **)&original);
    if(hr==E_NOINTERFACE)return IUnknown_QueryInterface(object,iid,out);
    if(FAILED(hr)||!original)return FAILED(hr)?hr:E_UNEXPECTED;
    hr=IUnknown_QueryInterface(original,iid,out);
    IUnknown_Release(original);
    return hr;
}
