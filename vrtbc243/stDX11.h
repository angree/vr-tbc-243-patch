#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <sstream>
#include <string>

struct stDX11
{
    UINT NumFeatureLevels = 5;
    D3D_FEATURE_LEVEL FeatureLevels[5] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_1};
    D3D_FEATURE_LEVEL FeatureLevel = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* devcon = nullptr;
    IDXGISwapChain1* swapchain = nullptr;
    ID3D11RenderTargetView* backbuffer = nullptr;
    IDXGIFactory* factory = nullptr;
    DWORD occlusionCookie = 0;
    std::stringstream logError;

    stDX11() = default;
    ~stDX11() { Release(); }

    bool createDevice()
    {
        Release();
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            FeatureLevels, NumFeatureLevels, D3D11_SDK_VERSION, &dev, &FeatureLevel, &devcon);
        if (result == E_INVALIDARG) {
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                FeatureLevels + 1, NumFeatureLevels - 1, D3D11_SDK_VERSION,
                &dev, &FeatureLevel, &devcon);
        }
        if (FAILED(result)) {
            logError << "D3D11CreateDevice failed: 0x" << std::hex << result << '\n';
            return false;
        }
        result = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
        if (FAILED(result)) {
            logError << "CreateDXGIFactory1 failed: 0x" << std::hex << result << '\n';
            Release();
            return false;
        }
        return true;
    }

    bool createFactory(HWND window, bool disableAltEnter = false)
    {
        if (!dev) return false;
        if (factory) { factory->Release(); factory = nullptr; }
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        HRESULT result = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
        if (SUCCEEDED(result)) result = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter));
        if (dxgiDevice) dxgiDevice->Release();
        if (SUCCEEDED(result)) result = adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
        if (adapter) adapter->Release();
        if (FAILED(result) || !factory) { logError << "DXGI factory lookup failed\n"; return false; }
        if (disableAltEnter && window && FAILED(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER))) {
            logError << "MakeWindowAssociation failed\n"; return false;
        }
        return true;
    }

    bool createSwapchain(HWND window, int width, int height, bool disableAltEnter = false)
    {
        if (!factory && !createFactory(window, disableAltEnter)) return false;
        IDXGIFactory2* factory2 = nullptr;
        HRESULT result = factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2));
        if (FAILED(result)) return false;
        DXGI_SWAP_CHAIN_DESC1 description = {};
        description.Width = width; description.Height = height;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1; description.BufferCount = 2;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        result = factory2->CreateSwapChainForHwnd(dev, window, &description, nullptr, nullptr, &swapchain);
        factory2->Release();
        if (FAILED(result)) { logError << "CreateSwapChainForHwnd failed: 0x" << std::hex << result << '\n'; return false; }
        return true;
    }

    bool createBackBuffer()
    {
        if (!swapchain || !dev) return false;
        ID3D11Texture2D* texture = nullptr;
        HRESULT result = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        if (SUCCEEDED(result)) result = dev->CreateRenderTargetView(texture, nullptr, &backbuffer);
        if (texture) texture->Release();
        if (FAILED(result)) { logError << "Backbuffer creation failed: 0x" << std::hex << result << '\n'; return false; }
        return true;
    }

    DWORD isOccluded() { return occlusionCookie; }
    void Release()
    {
        if (backbuffer) { backbuffer->Release(); backbuffer = nullptr; }
        if (swapchain) { swapchain->Release(); swapchain = nullptr; }
        if (factory) { factory->Release(); factory = nullptr; }
        if (devcon) { devcon->Release(); devcon = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
    }
    std::string GetErrors()
    {
        std::string result = logError.str(); logError.str(std::string()); logError.clear(); return result;
    }
};
