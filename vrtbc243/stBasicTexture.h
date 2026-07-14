#pragma once

#include <d3d11_4.h>
#include <d3d9.h>
#include <wincodec.h>

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

class WIC_DXGI
{
    IWICImagingFactory* factory = nullptr;

public:
    ~WIC_DXGI() { if (factory) factory->Release(); }

    DXGI_FORMAT GetFormat(WICPixelFormatGUID format)
    {
        if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) return DXGI_FORMAT_B8G8R8A8_UNORM;
        if (IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA)) return DXGI_FORMAT_R8G8B8A8_UNORM;
        if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGR)) return DXGI_FORMAT_B8G8R8X8_UNORM;
        if (IsEqualGUID(format, GUID_WICPixelFormat64bppRGBAHalf)) return DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (IsEqualGUID(format, GUID_WICPixelFormat64bppRGBA)) return DXGI_FORMAT_R16G16B16A16_UNORM;
        if (IsEqualGUID(format, GUID_WICPixelFormat128bppRGBAFloat)) return DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (IsEqualGUID(format, GUID_WICPixelFormat32bppGrayFloat)) return DXGI_FORMAT_R32_FLOAT;
        if (IsEqualGUID(format, GUID_WICPixelFormat16bppGrayHalf)) return DXGI_FORMAT_R16_FLOAT;
        if (IsEqualGUID(format, GUID_WICPixelFormat16bppGray)) return DXGI_FORMAT_R16_UNORM;
        if (IsEqualGUID(format, GUID_WICPixelFormat8bppGray)) return DXGI_FORMAT_R8_UNORM;
        if (IsEqualGUID(format, GUID_WICPixelFormat8bppAlpha)) return DXGI_FORMAT_A8_UNORM;
        return DXGI_FORMAT_UNKNOWN;
    }

    int GetBitsPerPixel(DXGI_FORMAT format)
    {
        switch (format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 128;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM: return 64;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R32_FLOAT: return 32;
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM: return 16;
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_A8_UNORM: return 8;
        default: return 0;
        }
    }

    WICPixelFormatGUID GetBaseFormat(WICPixelFormatGUID format)
    {
        if (GetFormat(format) != DXGI_FORMAT_UNKNOWN) return format;
        return GUID_WICPixelFormat32bppBGRA;
    }

    int LoadImageDataFromFile(const char* filename, BYTE** imageData, DXGI_FORMAT* format,
        int* width, int* height, int* fileSize, int* texturePitch)
    {
        if (!filename || !imageData || !format || !width || !height || !fileSize || !texturePitch)
            return 1;
        *imageData = nullptr;
        if (!factory) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)))) return 1;
        }
        wchar_t widePath[MAX_PATH] = {};
        if (!MultiByteToWideChar(CP_ACP, 0, filename, -1, widePath, MAX_PATH)) return 1;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        HRESULT result = factory->CreateDecoderFromFilename(widePath, nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(result)) return 2;
        result = decoder->GetFrame(0, &frame);
        if (FAILED(result)) { decoder->Release(); return 3; }
        UINT imageWidth = 0, imageHeight = 0;
        result = frame->GetSize(&imageWidth, &imageHeight);
        if (FAILED(result) || !imageWidth || !imageHeight) {
            frame->Release(); decoder->Release(); return 5;
        }
        result = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result)) result = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(result)) {
            if (converter) converter->Release(); frame->Release(); decoder->Release(); return 8;
        }
        *format = DXGI_FORMAT_B8G8R8A8_UNORM;
        *width = static_cast<int>(imageWidth); *height = static_cast<int>(imageHeight);
        *texturePitch = static_cast<int>(imageWidth * 4);
        *fileSize = *texturePitch * static_cast<int>(imageHeight);
        *imageData = static_cast<BYTE*>(std::malloc(static_cast<size_t>(*fileSize)));
        if (!*imageData) { converter->Release(); frame->Release(); decoder->Release(); return 9; }
        result = converter->CopyPixels(nullptr, *texturePitch, *fileSize, *imageData);
        converter->Release(); frame->Release(); decoder->Release();
        if (FAILED(result)) { std::free(*imageData); *imageData = nullptr; return 10; }
        return 0;
    }
};

struct stBasicTexture9
{
    IDirect3DTexture9* pTexture = nullptr;
    IDirect3DSurface9* pRenderTarget = nullptr;
    IDirect3DSurface9* pShaderResource = nullptr;
    IDirect3DSurface9* pDepthStencilView = nullptr;
    HANDLE pSharedHandle = nullptr;
    int creationType = 0;
    std::stringstream logError;
    D3DFORMAT renderFormat = D3DFMT_A8R8G8B8;
    int width = 0;
    int height = 0;

    void SetWidthHeight(int newWidth, int newHeight) { width = newWidth; height = newHeight; }
    bool Create(IDirect3DDevice9* device, bool rtv, bool srv, bool dsv, bool shared)
    {
        if (!device || width <= 0 || height <= 0) { logError << "Invalid D3D9 texture arguments\n"; return false; }
        return shared && pSharedHandle ? CreateShared(device, rtv, srv, dsv) :
            CreateNew(device, rtv, srv, dsv, shared);
    }
    bool CreateNew(IDirect3DDevice9* device, bool rtv, bool srv, bool dsv, bool shared)
    {
        Release();
        HRESULT result;
        HANDLE* sharedOut = shared ? &pSharedHandle : nullptr;
        if (rtv) result = device->CreateRenderTarget(width, height, renderFormat,
            D3DMULTISAMPLE_NONE, 0, TRUE, &pRenderTarget, sharedOut);
        else if (dsv) result = device->CreateDepthStencilSurface(width, height, renderFormat,
            D3DMULTISAMPLE_NONE, 0, FALSE, &pDepthStencilView, sharedOut);
        else result = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
            renderFormat, D3DPOOL_DEFAULT, &pTexture, sharedOut);
        if (FAILED(result)) { logError << "D3D9 resource creation failed: 0x" << std::hex << result << '\n'; return false; }
        creationType = 1;
        if (shared && !pSharedHandle) { logError << "D3D9 shared resource returned a null handle\n"; Release(); return false; }
        return !srv || CreateShaderResourceView(device);
    }
    bool CreateFromFile(IDirect3DDevice9* device, bool, bool srv, bool, const char* path)
    {
        if (!device) return false;
        BYTE* pixels = nullptr; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        int pitch = 0, bytes = 0;
        WIC_DXGI loader;
        const int load = loader.LoadImageDataFromFile(path, &pixels, &format, &width, &height, &bytes, &pitch);
        if (load != 0) { logError << "Image load failed (" << load << "): " << (path ? path : "") << '\n'; return false; }
        IDirect3DTexture9* staging = nullptr;
        HRESULT result = device->CreateTexture(width, height, 1, 0, renderFormat,
            D3DPOOL_SYSTEMMEM, &staging, nullptr);
        if (SUCCEEDED(result)) result = device->CreateTexture(width, height, 1,
            D3DUSAGE_RENDERTARGET, renderFormat, D3DPOOL_DEFAULT, &pTexture, nullptr);
        if (SUCCEEDED(result)) {
            D3DLOCKED_RECT locked = {};
            result = staging->LockRect(0, &locked, nullptr, 0);
            if (SUCCEEDED(result)) {
                for (int row = 0; row < height; ++row)
                    std::memcpy(static_cast<BYTE*>(locked.pBits) + row * locked.Pitch,
                        pixels + row * pitch, static_cast<size_t>(pitch));
                staging->UnlockRect(0);
                result = device->UpdateTexture(staging, pTexture);
            }
        }
        if (staging) staging->Release();
        std::free(pixels);
        if (FAILED(result)) { logError << "D3D9 image texture creation failed: 0x" << std::hex << result << '\n'; Release(); return false; }
        creationType = 1;
        return !srv || CreateShaderResourceView(device);
    }
    bool CreateShared(IDirect3DDevice9* device, bool rtv, bool srv, bool dsv)
    {
        if (!pSharedHandle) { logError << "Cannot open a null D3D9 shared handle\n"; return false; }
        const HANDLE handle = pSharedHandle;
        Release();
        pSharedHandle = handle;
        HRESULT result;
        if (rtv) result = device->CreateRenderTarget(width, height, renderFormat,
            D3DMULTISAMPLE_NONE, 0, TRUE, &pRenderTarget, &pSharedHandle);
        else if (dsv) result = device->CreateDepthStencilSurface(width, height, renderFormat,
            D3DMULTISAMPLE_NONE, 0, FALSE, &pDepthStencilView, &pSharedHandle);
        else result = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
            renderFormat, D3DPOOL_DEFAULT, &pTexture, &pSharedHandle);
        if (FAILED(result)) { logError << "D3D9 shared resource open failed: 0x" << std::hex << result << '\n'; Release(); return false; }
        creationType = 2;
        return !srv || CreateShaderResourceView(device);
    }
    bool CreateShaderResourceView(IDirect3DDevice9*)
    {
        if (!pTexture) { logError << "D3D9 shader surface requires a texture\n"; return false; }
        const HRESULT result = pTexture->GetSurfaceLevel(0, &pShaderResource);
        if (FAILED(result)) { logError << "GetSurfaceLevel failed: 0x" << std::hex << result << '\n'; return false; }
        return true;
    }
    void Release()
    {
        if (pDepthStencilView) { pDepthStencilView->Release(); pDepthStencilView = nullptr; }
        if (pShaderResource) { pShaderResource->Release(); pShaderResource = nullptr; }
        if (pRenderTarget) { pRenderTarget->Release(); pRenderTarget = nullptr; }
        if (pTexture) { pTexture->Release(); pTexture = nullptr; }
        pSharedHandle = nullptr; creationType = 0;
    }
    bool HasErrors() { return !logError.str().empty(); }
    std::string GetErrors()
    { std::string result = logError.str(); logError.str(std::string()); logError.clear(); return result; }
};

struct stBasicTexture
{
    ID3D11Texture2D* pTexture = nullptr;
    ID3D11RenderTargetView* pRenderTarget = nullptr;
    ID3D11ShaderResourceView* pShaderResource = nullptr;
    ID3D11DepthStencilView* pDepthStencilView = nullptr;
    HANDLE pSharedHandle = nullptr;
    int creationType = 0;
    std::stringstream logError;
    D3D11_TEXTURE2D_DESC textureDesc = {};

    stBasicTexture()
    {
        textureDesc.MipLevels = 1; textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1; textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    }
    void SetWidthHeight(int width, int height)
    { textureDesc.Width = static_cast<UINT>(width); textureDesc.Height = static_cast<UINT>(height); }
    bool Create(ID3D11Device* device, bool rtv, bool srv, bool dsv, bool shared)
    {
        if (!device || !textureDesc.Width || !textureDesc.Height) { logError << "Invalid D3D11 texture arguments\n"; return false; }
        return shared && pSharedHandle ? CreateShared(device, rtv, srv, dsv) :
            CreateNew(device, rtv, srv, dsv, shared);
    }
    bool CreateNew(ID3D11Device* device, bool rtv, bool srv, bool dsv, bool shared,
        D3D11_SUBRESOURCE_DATA* data = nullptr)
    {
        Release();
        D3D11_TEXTURE2D_DESC description = textureDesc;
        if (shared) description.MiscFlags |= D3D11_RESOURCE_MISC_SHARED;
        const HRESULT result = device->CreateTexture2D(&description, data, &pTexture);
        if (FAILED(result)) { logError << "CreateTexture2D failed: 0x" << std::hex << result << '\n'; return false; }
        textureDesc = description; creationType = 1;
        if (shared && !GetSharedHandle()) { Release(); return false; }
        if (rtv && !CreateRenderTargetView(device)) return false;
        if (srv && !CreateShaderResourceView(device)) return false;
        if (dsv && !CreateDepthStencilView(device)) return false;
        return true;
    }
    bool CreateShared(ID3D11Device* device, bool rtv, bool srv, bool dsv)
    {
        if (!pSharedHandle) { logError << "Cannot open a null D3D11 shared handle\n"; return false; }
        const HANDLE handle = pSharedHandle;
        Release(); pSharedHandle = handle;
        const HRESULT result = device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&pTexture));
        if (FAILED(result) || !pTexture) { logError << "OpenSharedResource failed: 0x" << std::hex << result << '\n'; Release(); return false; }
        pTexture->GetDesc(&textureDesc); creationType = 2;
        if (rtv && !CreateRenderTargetView(device)) return false;
        if (srv && !CreateShaderResourceView(device)) return false;
        if (dsv && !CreateDepthStencilView(device)) return false;
        return true;
    }
    bool GetSharedHandle()
    {
        if (!pTexture) return false;
        IDXGIResource* resource = nullptr;
        HRESULT result = pTexture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
        if (SUCCEEDED(result)) result = resource->GetSharedHandle(&pSharedHandle);
        if (resource) resource->Release();
        if (FAILED(result) || !pSharedHandle) { logError << "D3D11 shared handle retrieval failed: 0x" << std::hex << result << '\n'; return false; }
        return true;
    }
    bool CreateRenderTargetView(ID3D11Device* device, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN)
    {
        if (!pTexture) return false;
        D3D11_RENDER_TARGET_VIEW_DESC description = {};
        description.Format = format == DXGI_FORMAT_UNKNOWN ? textureDesc.Format : format;
        description.ViewDimension = textureDesc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        const HRESULT result = device->CreateRenderTargetView(pTexture, &description, &pRenderTarget);
        if (FAILED(result)) { logError << "CreateRenderTargetView failed: 0x" << std::hex << result << '\n'; return false; }
        return true;
    }
    bool CreateShaderResourceView(ID3D11Device* device, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN)
    {
        if (!pTexture) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC description = {};
        description.Format = format == DXGI_FORMAT_UNKNOWN ? textureDesc.Format : format;
        description.ViewDimension = textureDesc.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        description.Texture2D.MipLevels = 1;
        const HRESULT result = device->CreateShaderResourceView(pTexture, &description, &pShaderResource);
        if (FAILED(result)) { logError << "CreateShaderResourceView failed: 0x" << std::hex << result << '\n'; return false; }
        return true;
    }
    bool CreateDepthStencilView(ID3D11Device* device, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN)
    {
        if (!pTexture) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC description = {};
        description.Format = format == DXGI_FORMAT_UNKNOWN ? textureDesc.Format : format;
        description.ViewDimension = textureDesc.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        const HRESULT result = device->CreateDepthStencilView(pTexture, &description, &pDepthStencilView);
        if (FAILED(result)) { logError << "CreateDepthStencilView failed: 0x" << std::hex << result << '\n'; return false; }
        return true;
    }
    void Release()
    {
        if (pDepthStencilView) { pDepthStencilView->Release(); pDepthStencilView = nullptr; }
        if (pShaderResource) { pShaderResource->Release(); pShaderResource = nullptr; }
        if (pRenderTarget) { pRenderTarget->Release(); pRenderTarget = nullptr; }
        if (pTexture) { pTexture->Release(); pTexture = nullptr; }
        pSharedHandle = nullptr; creationType = 0;
    }
    bool HasErrors() { return !logError.str().empty(); }
    std::string GetErrors()
    { std::string result = logError.str(); logError.str(std::string()); logError.clear(); return result; }
};
