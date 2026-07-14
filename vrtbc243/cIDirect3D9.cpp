#include "cIDirect3D9.h"

#include "cIDirect3DDevice9.h"

#include <fstream>
#include <new>

extern std::ofstream ofOut;
extern cIDirect3D9* glIDirect3D9;
extern cIDirect3DDevice9* glIDirect3DDevice9;
extern cIDirect3DDevice9Ex* glIDirect3DDevice9Ex;

cIDirect3D9::cIDirect3D9(IDirect3D9Ex* original, bool showLog, std::stringstream* log)
    : logError(log), doLog(showLog), m_pIDirect3D9(original)
{
}

cIDirect3D9::~cIDirect3D9()
{
    if (m_pIDirect3D9) {
        m_pIDirect3D9->Release();
        m_pIDirect3D9 = nullptr;
    }
}

HRESULT __stdcall cIDirect3D9::QueryInterface(REFIID riid, void** object)
{
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IDirect3D9) || riid == __uuidof(IDirect3D9Ex)) {
        *object = static_cast<IDirect3D9Ex*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG __stdcall cIDirect3D9::AddRef()
{
    return m_pIDirect3D9 ? m_pIDirect3D9->AddRef() : 0;
}

ULONG __stdcall cIDirect3D9::Release()
{
    if (!m_pIDirect3D9) return 0;
    const ULONG remaining = m_pIDirect3D9->Release();
    if (remaining == 0) {
        m_pIDirect3D9 = nullptr;
        if (glIDirect3D9 == this) glIDirect3D9 = nullptr;
        delete this;
    }
    return remaining;
}

HRESULT __stdcall cIDirect3D9::RegisterSoftwareDevice(void* initialize)
{ return m_pIDirect3D9->RegisterSoftwareDevice(initialize); }
UINT __stdcall cIDirect3D9::GetAdapterCount()
{ return m_pIDirect3D9->GetAdapterCount(); }
HRESULT __stdcall cIDirect3D9::GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* identifier)
{ return m_pIDirect3D9->GetAdapterIdentifier(adapter, flags, identifier); }
UINT __stdcall cIDirect3D9::GetAdapterModeCount(UINT adapter, D3DFORMAT format)
{ return m_pIDirect3D9->GetAdapterModeCount(adapter, format); }
HRESULT __stdcall cIDirect3D9::EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* displayMode)
{ return m_pIDirect3D9->EnumAdapterModes(adapter, format, mode, displayMode); }
HRESULT __stdcall cIDirect3D9::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode)
{ return m_pIDirect3D9->GetAdapterDisplayMode(adapter, mode); }
HRESULT __stdcall cIDirect3D9::CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
    D3DFORMAT backBufferFormat, BOOL windowed)
{ return m_pIDirect3D9->CheckDeviceType(adapter, type, adapterFormat, backBufferFormat, windowed); }
HRESULT __stdcall cIDirect3D9::CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
    DWORD usage, D3DRESOURCETYPE resourceType, D3DFORMAT checkFormat)
{ return m_pIDirect3D9->CheckDeviceFormat(adapter, type, adapterFormat, usage, resourceType, checkFormat); }
HRESULT __stdcall cIDirect3D9::CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type,
    D3DFORMAT surfaceFormat, BOOL windowed, D3DMULTISAMPLE_TYPE samples, DWORD* qualityLevels)
{ return m_pIDirect3D9->CheckDeviceMultiSampleType(adapter, type, surfaceFormat, windowed, samples, qualityLevels); }
HRESULT __stdcall cIDirect3D9::CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type,
    D3DFORMAT adapterFormat, D3DFORMAT targetFormat, D3DFORMAT depthFormat)
{ return m_pIDirect3D9->CheckDepthStencilMatch(adapter, type, adapterFormat, targetFormat, depthFormat); }
HRESULT __stdcall cIDirect3D9::CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type,
    D3DFORMAT sourceFormat, D3DFORMAT targetFormat)
{ return m_pIDirect3D9->CheckDeviceFormatConversion(adapter, type, sourceFormat, targetFormat); }
HRESULT __stdcall cIDirect3D9::GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps)
{ return m_pIDirect3D9->GetDeviceCaps(adapter, type, caps); }
HMONITOR __stdcall cIDirect3D9::GetAdapterMonitor(UINT adapter)
{ return m_pIDirect3D9->GetAdapterMonitor(adapter); }

HRESULT __stdcall cIDirect3D9::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow,
    DWORD behavior, D3DPRESENT_PARAMETERS* presentation, IDirect3DDevice9** output)
{
    if (!output) return D3DERR_INVALIDCALL;
    *output = nullptr;
    IDirect3DDevice9* realDevice = nullptr;
    const HRESULT result = m_pIDirect3D9->CreateDevice(adapter, type, focusWindow, behavior,
        presentation, &realDevice);
    if (FAILED(result)) return result;

    cIDirect3DDevice9* wrapper = new (std::nothrow) cIDirect3DDevice9(realDevice, doLog, logError);
    if (!wrapper) {
        realDevice->Release();
        return E_OUTOFMEMORY;
    }
    glIDirect3DDevice9 = wrapper;
    *output = wrapper;
    ofOut << "[proxy] wrapped IDirect3DDevice9 created" << std::endl;
    return result;
}

UINT __stdcall cIDirect3D9::GetAdapterModeCountEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter)
{ return m_pIDirect3D9->GetAdapterModeCountEx(adapter, filter); }
HRESULT __stdcall cIDirect3D9::EnumAdapterModesEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter,
    UINT mode, D3DDISPLAYMODEEX* displayMode)
{ return m_pIDirect3D9->EnumAdapterModesEx(adapter, filter, mode, displayMode); }
HRESULT __stdcall cIDirect3D9::GetAdapterDisplayModeEx(UINT adapter, D3DDISPLAYMODEEX* mode,
    D3DDISPLAYROTATION* rotation)
{ return m_pIDirect3D9->GetAdapterDisplayModeEx(adapter, mode, rotation); }

HRESULT __stdcall cIDirect3D9::CreateDeviceEx(UINT adapter, D3DDEVTYPE type, HWND focusWindow,
    DWORD behavior, D3DPRESENT_PARAMETERS* presentation, D3DDISPLAYMODEEX* fullscreenMode,
    IDirect3DDevice9Ex** output)
{
    if (!output) return D3DERR_INVALIDCALL;
    *output = nullptr;
    IDirect3DDevice9Ex* realDevice = nullptr;
    const HRESULT result = m_pIDirect3D9->CreateDeviceEx(adapter, type, focusWindow, behavior,
        presentation, fullscreenMode, &realDevice);
    if (FAILED(result)) return result;

    cIDirect3DDevice9Ex* wrapper = new (std::nothrow) cIDirect3DDevice9Ex(realDevice, doLog, logError);
    if (!wrapper) {
        realDevice->Release();
        return E_OUTOFMEMORY;
    }
    glIDirect3DDevice9Ex = wrapper;
    *output = wrapper;
    ofOut << "[proxy] wrapped IDirect3DDevice9Ex created" << std::endl;
    return result;
}

HRESULT __stdcall cIDirect3D9::GetAdapterLUID(UINT adapter, LUID* luid)
{ return m_pIDirect3D9->GetAdapterLUID(adapter, luid); }
