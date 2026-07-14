#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>

#include <cstring>

union uMatrix
{
    float matrix[4][4];
    float _m[16];

    uMatrix* operator=(const float source[4][4])
    {
        std::memcpy(matrix, source, sizeof(matrix));
        return this;
    }
};

struct Vector3
{
    float x;
    float y;
    float z;

    bool operator==(const Vector3& other) const
    { return x == other.x && y == other.y && z == other.z; }
    Vector3 operator-(const Vector3& other) const
    { return {x - other.x, y - other.y, z - other.z}; }
    Vector3 operator+(const Vector3& other) const
    { return {x + other.x, y + other.y, z + other.z}; }
    Vector3 operator=(const float source[4])
    { return {source[0], source[1], source[2]}; }
};

enum poseType
{
    None = 0,
    Projection = 1,
    EyeOffset = 5,
    hmdPosition = 10,
    LeftHand = 20,
    LeftHandPalm = 21,
    RightHand = 30,
    RightHandPalm = 31,
};

struct stScreenLayout
{
    UINT64 pid = 0;
    HWND hwnd = nullptr;
    int width = 0;
    int height = 0;
    bool haveLayout = false;

    void SetFromSwapchain(IDXGISwapChain4* swapchain)
    {
        if (!swapchain) return;
        DXGI_SWAP_CHAIN_DESC description = {};
        if (SUCCEEDED(swapchain->GetDesc(&description))) {
            hwnd = description.OutputWindow;
            width = static_cast<int>(description.BufferDesc.Width);
            height = static_cast<int>(description.BufferDesc.Height);
            haveLayout = true;
        }
    }
};
