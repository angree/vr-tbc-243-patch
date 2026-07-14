#pragma once

#include "proxydll.h"

// Pass-through wrapper for the Direct3D 9 factory interface.
//
// Every method below is a straight re-declaration of Microsoft's IDirect3D9Ex
// COM interface (see the Windows SDK d3d9.h). To subclass a COM interface the
// derived class MUST repeat the exact vtable signatures, so this list is fixed
// by the platform and carries no expression original to this project or any
// third party. The wrapper simply forwards to the real object; the only place
// we add behaviour is CreateDevice, which returns our device wrapper instead.
class cIDirect3D9 final : public IDirect3D9Ex
{
public:
    cIDirect3D9(IDirect3D9Ex* pOriginal, bool showLog, std::stringstream* log);
    virtual ~cIDirect3D9();

    // IUnknown.
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG   __stdcall AddRef() override;
    ULONG   __stdcall Release() override;

    // IDirect3D9.
    HRESULT __stdcall RegisterSoftwareDevice(void* pInitializeFunction) override;
    UINT    __stdcall GetAdapterCount() override;
    HRESULT __stdcall GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override;
    UINT    __stdcall GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override;
    HRESULT __stdcall EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override;
    HRESULT __stdcall GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override;
    HRESULT __stdcall CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat,
        D3DFORMAT BackBufferFormat, BOOL bWindowed) override;
    HRESULT __stdcall CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
        DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override;
    HRESULT __stdcall CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType,
        D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
        DWORD* pQualityLevels) override;
    HRESULT __stdcall CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType,
        D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override;
    HRESULT __stdcall CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType,
        D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override;
    HRESULT __stdcall GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override;
    HMONITOR __stdcall GetAdapterMonitor(UINT Adapter) override;
    HRESULT __stdcall CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
        DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface) override;

    // IDirect3D9Ex.
    UINT    __stdcall GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter) override;
    HRESULT __stdcall EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter,
        UINT Mode, D3DDISPLAYMODEEX* pMode) override;
    HRESULT __stdcall GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode,
        D3DDISPLAYROTATION* pRotation) override;
    HRESULT __stdcall CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
        DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
        D3DDISPLAYMODEEX* pFullscreenDisplayMode,
        IDirect3DDevice9Ex** ppReturnedDeviceInterface) override;
    HRESULT __stdcall GetAdapterLUID(UINT Adapter, LUID* pLUID) override;

private:
    IDirect3D9Ex*      m_pIDirect3D9;
    std::stringstream* logError;
    bool               doLog;
};
