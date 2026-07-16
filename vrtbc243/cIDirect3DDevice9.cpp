#include "cIDirect3DDevice9.h"

#include "rec_probe.h"

#include <algorithm>
#include <fstream>

extern std::ofstream ofOut;
extern cIDirect3DDevice9* glIDirect3DDevice9;
extern cIDirect3DDevice9Ex* glIDirect3DDevice9Ex;
extern void VR_PreReset();
extern void VR_PostReset(long result);
extern volatile bool g_plateZTest;   // set by the eye loop around the nameplate (world-strata) list draw

namespace {
DWORD pointerTag(const void* value)
{
    return static_cast<DWORD>(reinterpret_cast<uintptr_t>(value));
}

void normalizeManagedResource(DWORD& usage, D3DPOOL& pool)
{
    if (usage == 0) usage = D3DUSAGE_DYNAMIC;
    if (pool == D3DPOOL_MANAGED) pool = D3DPOOL_DEFAULT;
}

void drawHighlightPasses(IDirect3DDevice9* device, D3DPRIMITIVETYPE primitiveType,
    INT baseVertex, UINT minVertex, UINT vertexCount, UINT firstIndex, UINT primitiveCount)
{
    if (!device || !g_fix75_Highlight || !g_hlBoost) return;
    int passCount = static_cast<int>(g_hlBoostPasses);
    if (passCount < 1) passCount = 1;
    if (passCount > 5) passCount = 5;
    const auto channel = [](float value) -> DWORD {
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        return static_cast<DWORD>(value * 255.0f);
    };
    const DWORD blendColor = D3DCOLOR_ARGB(255, channel(g_hlBoostColor[0]),
        channel(g_hlBoostColor[1]), channel(g_hlBoostColor[2]));

    DWORD oldBlendEnable = 0, oldSourceBlend = 0, oldDestinationBlend = 0;
    DWORD oldDepthFunction = 0, oldDepthWrite = 0, oldBlendFactor = 0;
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldBlendEnable);
    device->GetRenderState(D3DRS_SRCBLEND, &oldSourceBlend);
    device->GetRenderState(D3DRS_DESTBLEND, &oldDestinationBlend);
    device->GetRenderState(D3DRS_ZFUNC, &oldDepthFunction);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &oldDepthWrite);
    device->GetRenderState(D3DRS_BLENDFACTOR, &oldBlendFactor);

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_BLENDFACTOR, blendColor);
    for (int pass = 0; pass < passCount; ++pass)
        device->DrawIndexedPrimitive(primitiveType, baseVertex, minVertex, vertexCount,
            firstIndex, primitiveCount);

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, oldBlendEnable);
    device->SetRenderState(D3DRS_SRCBLEND, oldSourceBlend);
    device->SetRenderState(D3DRS_DESTBLEND, oldDestinationBlend);
    device->SetRenderState(D3DRS_ZFUNC, oldDepthFunction);
    device->SetRenderState(D3DRS_ZWRITEENABLE, oldDepthWrite);
    device->SetRenderState(D3DRS_BLENDFACTOR, oldBlendFactor);
}
}

#define DEVICE9_FORWARD_METHODS(X) \
X(HRESULT, TestCooperativeLevel, (), ()) \
X(UINT, GetAvailableTextureMem, (), ()) \
X(HRESULT, EvictManagedResources, (), ()) \
X(HRESULT, GetDirect3D, (IDirect3D9** value), (value)) \
X(HRESULT, GetDeviceCaps, (D3DCAPS9* value), (value)) \
X(HRESULT, GetDisplayMode, (UINT chain, D3DDISPLAYMODE* value), (chain, value)) \
X(HRESULT, GetCreationParameters, (D3DDEVICE_CREATION_PARAMETERS* value), (value)) \
X(HRESULT, SetCursorProperties, (UINT x, UINT y, IDirect3DSurface9* bitmap), (x, y, bitmap)) \
X(void, SetCursorPosition, (int x, int y, DWORD flags), (x, y, flags)) \
X(BOOL, ShowCursor, (BOOL show), (show)) \
X(HRESULT, CreateAdditionalSwapChain, (D3DPRESENT_PARAMETERS* parameters, IDirect3DSwapChain9** chain), (parameters, chain)) \
X(HRESULT, GetSwapChain, (UINT index, IDirect3DSwapChain9** chain), (index, chain)) \
X(UINT, GetNumberOfSwapChains, (), ()) \
X(HRESULT, Present, (const RECT* source, const RECT* destination, HWND window, const RGNDATA* dirty), (source, destination, window, dirty)) \
X(HRESULT, GetBackBuffer, (UINT chain, UINT buffer, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** value), (chain, buffer, type, value)) \
X(HRESULT, GetRasterStatus, (UINT chain, D3DRASTER_STATUS* value), (chain, value)) \
X(HRESULT, SetDialogBoxMode, (BOOL enabled), (enabled)) \
X(void, SetGammaRamp, (UINT chain, DWORD flags, const D3DGAMMARAMP* value), (chain, flags, value)) \
X(void, GetGammaRamp, (UINT chain, D3DGAMMARAMP* value), (chain, value)) \
X(HRESULT, CreateRenderTarget, (UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable, IDirect3DSurface9** surface, HANDLE* shared), (width, height, format, samples, quality, lockable, surface, shared)) \
X(HRESULT, CreateDepthStencilSurface, (UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL discard, IDirect3DSurface9** surface, HANDLE* shared), (width, height, format, samples, quality, discard, surface, shared)) \
X(HRESULT, UpdateSurface, (IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* destination, const POINT* point), (source, sourceRect, destination, point)) \
X(HRESULT, UpdateTexture, (IDirect3DBaseTexture9* source, IDirect3DBaseTexture9* destination), (source, destination)) \
X(HRESULT, GetRenderTargetData, (IDirect3DSurface9* source, IDirect3DSurface9* destination), (source, destination)) \
X(HRESULT, GetFrontBufferData, (UINT chain, IDirect3DSurface9* destination), (chain, destination)) \
X(HRESULT, StretchRect, (IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* destination, const RECT* destinationRect, D3DTEXTUREFILTERTYPE filter), (source, sourceRect, destination, destinationRect, filter)) \
X(HRESULT, ColorFill, (IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color), (surface, rect, color)) \
X(HRESULT, GetRenderTarget, (DWORD index, IDirect3DSurface9** surface), (index, surface)) \
X(HRESULT, GetDepthStencilSurface, (IDirect3DSurface9** surface), (surface)) \
X(HRESULT, BeginScene, (), ()) \
X(HRESULT, EndScene, (), ()) \
X(HRESULT, SetTransform, (D3DTRANSFORMSTATETYPE state, const D3DMATRIX* value), (state, value)) \
X(HRESULT, GetTransform, (D3DTRANSFORMSTATETYPE state, D3DMATRIX* value), (state, value)) \
X(HRESULT, MultiplyTransform, (D3DTRANSFORMSTATETYPE state, const D3DMATRIX* value), (state, value)) \
X(HRESULT, GetViewport, (D3DVIEWPORT9* value), (value)) \
X(HRESULT, SetMaterial, (const D3DMATERIAL9* value), (value)) \
X(HRESULT, GetMaterial, (D3DMATERIAL9* value), (value)) \
X(HRESULT, SetLight, (DWORD index, const D3DLIGHT9* value), (index, value)) \
X(HRESULT, GetLight, (DWORD index, D3DLIGHT9* value), (index, value)) \
X(HRESULT, LightEnable, (DWORD index, BOOL enable), (index, enable)) \
X(HRESULT, GetLightEnable, (DWORD index, BOOL* enable), (index, enable)) \
X(HRESULT, SetClipPlane, (DWORD index, const float* plane), (index, plane)) \
X(HRESULT, GetClipPlane, (DWORD index, float* plane), (index, plane)) \
X(HRESULT, GetRenderState, (D3DRENDERSTATETYPE state, DWORD* value), (state, value)) \
X(HRESULT, CreateStateBlock, (D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** value), (type, value)) \
X(HRESULT, BeginStateBlock, (), ()) \
X(HRESULT, EndStateBlock, (IDirect3DStateBlock9** value), (value)) \
X(HRESULT, SetClipStatus, (const D3DCLIPSTATUS9* value), (value)) \
X(HRESULT, GetClipStatus, (D3DCLIPSTATUS9* value), (value)) \
X(HRESULT, GetTexture, (DWORD stage, IDirect3DBaseTexture9** value), (stage, value)) \
X(HRESULT, GetTextureStageState, (DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value), (stage, type, value)) \
X(HRESULT, SetTextureStageState, (DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value), (stage, type, value)) \
X(HRESULT, GetSamplerState, (DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value), (sampler, type, value)) \
X(HRESULT, SetSamplerState, (DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value), (sampler, type, value)) \
X(HRESULT, ValidateDevice, (DWORD* passes), (passes)) \
X(HRESULT, SetPaletteEntries, (UINT palette, const PALETTEENTRY* entries), (palette, entries)) \
X(HRESULT, GetPaletteEntries, (UINT palette, PALETTEENTRY* entries), (palette, entries)) \
X(HRESULT, SetCurrentTexturePalette, (UINT palette), (palette)) \
X(HRESULT, GetCurrentTexturePalette, (UINT* palette), (palette)) \
X(HRESULT, SetScissorRect, (const RECT* value), (value)) \
X(HRESULT, GetScissorRect, (RECT* value), (value)) \
X(HRESULT, SetSoftwareVertexProcessing, (BOOL enabled), (enabled)) \
X(BOOL, GetSoftwareVertexProcessing, (), ()) \
X(HRESULT, SetNPatchMode, (float segments), (segments)) \
X(float, GetNPatchMode, (), ()) \
X(HRESULT, DrawPrimitiveUP, (D3DPRIMITIVETYPE type, UINT count, const void* vertices, UINT stride), (type, count, vertices, stride)) \
X(HRESULT, DrawIndexedPrimitiveUP, (D3DPRIMITIVETYPE type, UINT minVertex, UINT vertices, UINT count, const void* indices, D3DFORMAT format, const void* vertexData, UINT stride), (type, minVertex, vertices, count, indices, format, vertexData, stride)) \
X(HRESULT, ProcessVertices, (UINT source, UINT destination, UINT count, IDirect3DVertexBuffer9* buffer, IDirect3DVertexDeclaration9* declaration, DWORD flags), (source, destination, count, buffer, declaration, flags)) \
X(HRESULT, CreateVertexDeclaration, (const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** value), (elements, value)) \
X(HRESULT, SetVertexDeclaration, (IDirect3DVertexDeclaration9* value), (value)) \
X(HRESULT, GetVertexDeclaration, (IDirect3DVertexDeclaration9** value), (value)) \
X(HRESULT, SetFVF, (DWORD value), (value)) \
X(HRESULT, GetFVF, (DWORD* value), (value)) \
X(HRESULT, CreateVertexShader, (const DWORD* function, IDirect3DVertexShader9** value), (function, value)) \
X(HRESULT, SetVertexShader, (IDirect3DVertexShader9* value), (value)) \
X(HRESULT, GetVertexShader, (IDirect3DVertexShader9** value), (value)) \
X(HRESULT, SetVertexShaderConstantF, (UINT start, const float* data, UINT count), (start, data, count)) \
X(HRESULT, GetVertexShaderConstantF, (UINT start, float* data, UINT count), (start, data, count)) \
X(HRESULT, SetVertexShaderConstantI, (UINT start, const int* data, UINT count), (start, data, count)) \
X(HRESULT, GetVertexShaderConstantI, (UINT start, int* data, UINT count), (start, data, count)) \
X(HRESULT, SetVertexShaderConstantB, (UINT start, const BOOL* data, UINT count), (start, data, count)) \
X(HRESULT, GetVertexShaderConstantB, (UINT start, BOOL* data, UINT count), (start, data, count)) \
X(HRESULT, SetStreamSource, (UINT stream, IDirect3DVertexBuffer9* data, UINT offset, UINT stride), (stream, data, offset, stride)) \
X(HRESULT, GetStreamSource, (UINT stream, IDirect3DVertexBuffer9** data, UINT* offset, UINT* stride), (stream, data, offset, stride)) \
X(HRESULT, SetStreamSourceFreq, (UINT stream, UINT setting), (stream, setting)) \
X(HRESULT, GetStreamSourceFreq, (UINT stream, UINT* setting), (stream, setting)) \
X(HRESULT, SetIndices, (IDirect3DIndexBuffer9* value), (value)) \
X(HRESULT, GetIndices, (IDirect3DIndexBuffer9** value), (value)) \
X(HRESULT, CreatePixelShader, (const DWORD* function, IDirect3DPixelShader9** value), (function, value)) \
X(HRESULT, SetPixelShader, (IDirect3DPixelShader9* value), (value)) \
X(HRESULT, GetPixelShader, (IDirect3DPixelShader9** value), (value)) \
X(HRESULT, SetPixelShaderConstantF, (UINT start, const float* data, UINT count), (start, data, count)) \
X(HRESULT, GetPixelShaderConstantF, (UINT start, float* data, UINT count), (start, data, count)) \
X(HRESULT, SetPixelShaderConstantI, (UINT start, const int* data, UINT count), (start, data, count)) \
X(HRESULT, GetPixelShaderConstantI, (UINT start, int* data, UINT count), (start, data, count)) \
X(HRESULT, SetPixelShaderConstantB, (UINT start, const BOOL* data, UINT count), (start, data, count)) \
X(HRESULT, GetPixelShaderConstantB, (UINT start, BOOL* data, UINT count), (start, data, count)) \
X(HRESULT, DrawRectPatch, (UINT handle, const float* segments, const D3DRECTPATCH_INFO* info), (handle, segments, info)) \
X(HRESULT, DrawTriPatch, (UINT handle, const float* segments, const D3DTRIPATCH_INFO* info), (handle, segments, info)) \
X(HRESULT, DeletePatch, (UINT handle), (handle)) \
X(HRESULT, CreateQuery, (D3DQUERYTYPE type, IDirect3DQuery9** value), (type, value))

cIDirect3DDevice9::cIDirect3DDevice9(IDirect3DDevice9* original, bool showLog, std::stringstream* log)
    : isSet(original != nullptr), logError(log), doLog(showLog),
      m_pIDirect3DDevice9(original), m_nRefCount(1)
{
}

cIDirect3DDevice9::~cIDirect3DDevice9()
{
    isSet = false;
    if (m_pIDirect3DDevice9) m_pIDirect3DDevice9->Release();
    m_pIDirect3DDevice9 = nullptr;
}

HRESULT cIDirect3DDevice9::QueryInterface(REFIID riid, void** object)
{
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IDirect3DDevice9)) {
        *object = static_cast<IDirect3DDevice9*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG cIDirect3DDevice9::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_nRefCount));
}
ULONG cIDirect3DDevice9::Release()
{
    const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&m_nRefCount));
    if (!remaining) {
        if (glIDirect3DDevice9 == this) glIDirect3DDevice9 = nullptr;
        delete this;
    }
    return remaining;
}

#define DEFINE_BASE_FORWARD(result, name, parameters, arguments) \
result cIDirect3DDevice9::name parameters { return m_pIDirect3DDevice9->name arguments; }
DEVICE9_FORWARD_METHODS(DEFINE_BASE_FORWARD)
#undef DEFINE_BASE_FORWARD

HRESULT cIDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS* parameters)
{
    VR_PreReset();
    const HRESULT result = m_pIDirect3DDevice9->Reset(parameters);
    VR_PostReset(result);
    return result;
}

HRESULT cIDirect3DDevice9::CreateTexture(UINT width, UINT height, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** texture, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9->CreateTexture(width, height, levels, usage, format, pool, texture, shared);
}
HRESULT cIDirect3DDevice9::CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9** texture, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9->CreateVolumeTexture(width, height, depth, levels, usage, format, pool, texture, shared);
}
HRESULT cIDirect3DDevice9::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, IDirect3DCubeTexture9** texture, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9->CreateCubeTexture(edge, levels, usage, format, pool, texture, shared);
}
HRESULT cIDirect3DDevice9::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer9** buffer, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9->CreateVertexBuffer(length, usage, fvf, pool, buffer, shared);
}
HRESULT cIDirect3DDevice9::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DIndexBuffer9** buffer, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9->CreateIndexBuffer(length, usage, format, pool, buffer, shared);
}
HRESULT cIDirect3DDevice9::CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format,
    D3DPOOL pool, IDirect3DSurface9** surface, HANDLE* shared)
{
    if (pool == D3DPOOL_MANAGED) pool = D3DPOOL_DEFAULT;
    return m_pIDirect3DDevice9->CreateOffscreenPlainSurface(width, height, format, pool, surface, shared);
}

HRESULT cIDirect3DDevice9::SetRenderTarget(DWORD index, IDirect3DSurface9* surface)
{
    if (index == 0) RecAppend(1, pointerTag(surface), 0);
    return m_pIDirect3DDevice9->SetRenderTarget(index, surface);
}
HRESULT cIDirect3DDevice9::SetDepthStencilSurface(IDirect3DSurface9* surface)
{
    RecAppend(2, pointerTag(surface), 0);
    return m_pIDirect3DDevice9->SetDepthStencilSurface(surface);
}
HRESULT cIDirect3DDevice9::Clear(DWORD count, const D3DRECT* rects, DWORD flags,
    D3DCOLOR color, float depth, DWORD stencil)
{
    RecAppend(3, flags, color);
    return m_pIDirect3DDevice9->Clear(count, rects, flags, color, depth, stencil);
}
HRESULT cIDirect3DDevice9::SetViewport(const D3DVIEWPORT9* viewport)
{
    if (viewport) RecAppend(8, (viewport->X << 16) | (viewport->Y & 0xffff),
        (viewport->Width << 16) | (viewport->Height & 0xffff));
    return m_pIDirect3DDevice9->SetViewport(viewport);
}
HRESULT cIDirect3DDevice9::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    RecMaybeRS(static_cast<unsigned int>(state), value);
    // (Nameplate z-test state rewrites removed 2026-07-16: rewriting ZENABLE against
    // the engine desyncs its Gx state cache and corrupts world rendering. The whole
    // screen-space z-test path is abandoned — see NAMEPLATE_WORLDSPACE_REDESIGN.md.)
    return m_pIDirect3DDevice9->SetRenderState(state, value);
}
HRESULT cIDirect3DDevice9::SetTexture(DWORD stage, IDirect3DBaseTexture9* texture)
{
    if (stage == 0) {
        g_recCurTex = pointerTag(texture);
        RecAppend(7, g_recCurTex, 0);
        if (texture && g_nameBarTid == GetCurrentThreadId())
            g_nameBarTex0 = texture;   // font atlas of the name being drawn
    }
    return m_pIDirect3DDevice9->SetTexture(stage, texture);
}
HRESULT cIDirect3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    if (g_warmupSkipDraws) return D3D_OK;
    RecAppend(6, g_recCurTex, count);
    return m_pIDirect3DDevice9->DrawPrimitive(type, start, count);
}
HRESULT cIDirect3DDevice9::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex,
    UINT minVertex, UINT vertices, UINT firstIndex, UINT count)
{
    if (g_warmupSkipDraws) return D3D_OK;
    RecAppend(5, g_recCurTex, count);
    const HRESULT result = m_pIDirect3DDevice9->DrawIndexedPrimitive(type, baseVertex,
        minVertex, vertices, firstIndex, count);
    drawHighlightPasses(m_pIDirect3DDevice9, type, baseVertex, minVertex, vertices, firstIndex, count);
    return result;
}

cIDirect3DDevice9Ex::cIDirect3DDevice9Ex(IDirect3DDevice9Ex* original, bool showLog,
    std::stringstream* log)
    : isSet(original != nullptr), logError(log), doLog(showLog),
      m_pIDirect3DDevice9Ex(original), m_nRefCount(1)
{
}

cIDirect3DDevice9Ex::~cIDirect3DDevice9Ex()
{
    isSet = false;
    if (m_pIDirect3DDevice9Ex) m_pIDirect3DDevice9Ex->Release();
    m_pIDirect3DDevice9Ex = nullptr;
}

HRESULT cIDirect3DDevice9Ex::QueryInterface(REFIID riid, void** object)
{
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IDirect3DDevice9) ||
        riid == __uuidof(IDirect3DDevice9Ex)) {
        *object = static_cast<IDirect3DDevice9Ex*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG cIDirect3DDevice9Ex::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_nRefCount));
}
ULONG cIDirect3DDevice9Ex::Release()
{
    const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&m_nRefCount));
    if (!remaining) {
        if (glIDirect3DDevice9Ex == this) glIDirect3DDevice9Ex = nullptr;
        delete this;
    }
    return remaining;
}

#define DEFINE_EX_FORWARD(result, name, parameters, arguments) \
result cIDirect3DDevice9Ex::name parameters { return m_pIDirect3DDevice9Ex->name arguments; }
DEVICE9_FORWARD_METHODS(DEFINE_EX_FORWARD)
#undef DEFINE_EX_FORWARD

HRESULT cIDirect3DDevice9Ex::Reset(D3DPRESENT_PARAMETERS* parameters)
{
    VR_PreReset();
    const HRESULT result = m_pIDirect3DDevice9Ex->Reset(parameters);
    VR_PostReset(result);
    return result;
}

#define EX_DIRECT(result, name, parameters, arguments) \
result cIDirect3DDevice9Ex::name parameters { return m_pIDirect3DDevice9Ex->name arguments; }
EX_DIRECT(HRESULT, SetRenderTarget, (DWORD index, IDirect3DSurface9* surface), (index, surface))
EX_DIRECT(HRESULT, SetDepthStencilSurface, (IDirect3DSurface9* surface), (surface))
EX_DIRECT(HRESULT, Clear, (DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float depth, DWORD stencil), (count, rects, flags, color, depth, stencil))
EX_DIRECT(HRESULT, SetViewport, (const D3DVIEWPORT9* viewport), (viewport))
EX_DIRECT(HRESULT, SetTexture, (DWORD stage, IDirect3DBaseTexture9* texture), (stage, texture))
EX_DIRECT(HRESULT, DrawPrimitive, (D3DPRIMITIVETYPE type, UINT start, UINT count), (type, start, count))
EX_DIRECT(HRESULT, DrawIndexedPrimitive, (D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT vertices, UINT firstIndex, UINT count), (type, baseVertex, minVertex, vertices, firstIndex, count))
#undef EX_DIRECT

HRESULT cIDirect3DDevice9Ex::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    RecMaybeRS(static_cast<unsigned int>(state), value);
    // (Nameplate z-test state rewrites removed 2026-07-16 — see the base device note.)
    return m_pIDirect3DDevice9Ex->SetRenderState(state, value);
}

HRESULT cIDirect3DDevice9Ex::CreateTexture(UINT width, UINT height, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** texture, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9Ex->CreateTexture(width, height, levels, usage, format, pool, texture, shared);
}
HRESULT cIDirect3DDevice9Ex::CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9** texture, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9Ex->CreateVolumeTexture(width, height, depth, levels, usage, format, pool, texture, shared);
}
HRESULT cIDirect3DDevice9Ex::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, IDirect3DCubeTexture9** texture, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9Ex->CreateCubeTexture(edge, levels, usage, format, pool, texture, shared);
}
HRESULT cIDirect3DDevice9Ex::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer9** buffer, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9Ex->CreateVertexBuffer(length, usage, fvf, pool, buffer, shared);
}
HRESULT cIDirect3DDevice9Ex::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DIndexBuffer9** buffer, HANDLE* shared)
{
    normalizeManagedResource(usage, pool);
    return m_pIDirect3DDevice9Ex->CreateIndexBuffer(length, usage, format, pool, buffer, shared);
}
HRESULT cIDirect3DDevice9Ex::CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format,
    D3DPOOL pool, IDirect3DSurface9** surface, HANDLE* shared)
{
    if (pool == D3DPOOL_MANAGED) pool = D3DPOOL_DEFAULT;
    return m_pIDirect3DDevice9Ex->CreateOffscreenPlainSurface(width, height, format, pool, surface, shared);
}

HRESULT cIDirect3DDevice9Ex::SetConvolutionMonoKernel(UINT width, UINT height, float* rows, float* columns)
{ return m_pIDirect3DDevice9Ex->SetConvolutionMonoKernel(width, height, rows, columns); }
HRESULT cIDirect3DDevice9Ex::ComposeRects(IDirect3DSurface9* source, IDirect3DSurface9* destination,
    IDirect3DVertexBuffer9* sourceRects, UINT count, IDirect3DVertexBuffer9* destinationRects,
    D3DCOMPOSERECTSOP operation, int x, int y)
{ return m_pIDirect3DDevice9Ex->ComposeRects(source, destination, sourceRects, count, destinationRects, operation, x, y); }
HRESULT cIDirect3DDevice9Ex::PresentEx(const RECT* source, const RECT* destination, HWND window,
    const RGNDATA* dirty, DWORD flags)
{ return m_pIDirect3DDevice9Ex->PresentEx(source, destination, window, dirty, flags); }
HRESULT cIDirect3DDevice9Ex::GetGPUThreadPriority(INT* priority)
{ return m_pIDirect3DDevice9Ex->GetGPUThreadPriority(priority); }
HRESULT cIDirect3DDevice9Ex::SetGPUThreadPriority(INT priority)
{ return m_pIDirect3DDevice9Ex->SetGPUThreadPriority(priority); }
HRESULT cIDirect3DDevice9Ex::WaitForVBlank(UINT chain)
{ return m_pIDirect3DDevice9Ex->WaitForVBlank(chain); }
HRESULT cIDirect3DDevice9Ex::CheckResourceResidency(IDirect3DResource9** resources, UINT32 count)
{ return m_pIDirect3DDevice9Ex->CheckResourceResidency(resources, count); }
HRESULT cIDirect3DDevice9Ex::SetMaximumFrameLatency(UINT value)
{ return m_pIDirect3DDevice9Ex->SetMaximumFrameLatency(value); }
HRESULT cIDirect3DDevice9Ex::GetMaximumFrameLatency(UINT* value)
{ return m_pIDirect3DDevice9Ex->GetMaximumFrameLatency(value); }
HRESULT cIDirect3DDevice9Ex::CheckDeviceState(HWND window)
{ return m_pIDirect3DDevice9Ex->CheckDeviceState(window); }
HRESULT cIDirect3DDevice9Ex::CreateRenderTargetEx(UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable, IDirect3DSurface9** surface,
    HANDLE* shared, DWORD usage)
{ return m_pIDirect3DDevice9Ex->CreateRenderTargetEx(width, height, format, samples, quality, lockable, surface, shared, usage); }
HRESULT cIDirect3DDevice9Ex::CreateOffscreenPlainSurfaceEx(UINT width, UINT height, D3DFORMAT format,
    D3DPOOL pool, IDirect3DSurface9** surface, HANDLE* shared, DWORD usage)
{ return m_pIDirect3DDevice9Ex->CreateOffscreenPlainSurfaceEx(width, height, format, pool, surface, shared, usage); }
HRESULT cIDirect3DDevice9Ex::CreateDepthStencilSurfaceEx(UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL discard, IDirect3DSurface9** surface,
    HANDLE* shared, DWORD usage)
{ return m_pIDirect3DDevice9Ex->CreateDepthStencilSurfaceEx(width, height, format, samples, quality, discard, surface, shared, usage); }
HRESULT cIDirect3DDevice9Ex::ResetEx(D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX* fullscreenMode)
{
    VR_PreReset();
    const HRESULT result = m_pIDirect3DDevice9Ex->ResetEx(parameters, fullscreenMode);
    VR_PostReset(result);
    return result;
}
HRESULT cIDirect3DDevice9Ex::GetDisplayModeEx(UINT chain, D3DDISPLAYMODEEX* mode,
    D3DDISPLAYROTATION* rotation)
{ return m_pIDirect3DDevice9Ex->GetDisplayModeEx(chain, mode, rotation); }

#undef DEVICE9_FORWARD_METHODS
