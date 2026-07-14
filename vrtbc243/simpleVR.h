#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <openvr.h>

#include <sstream>
#include <string>

#include "stCommon.h"

class simpleVR
{
    vr::IVRSystem* openVRSession = nullptr;
    vr::IVRChaperone* openVRChaperone = nullptr;
    vr::IVRRenderModels* openVRModels = nullptr;
    vr::TrackedDevicePose_t rTrackedDevicePose[vr::k_unMaxTrackedDeviceCount] = {};
    vr::HmdMatrix34_t hmdRawPose = {};
    bool hmdRawPoseValid = false;
    bool _isConnected = false;
    POINT bufferSize = {};
    POINT resolution = {};
    uMatrix projMatrixRaw[2] = {};
    uMatrix eyeViewMatrixRaw[2] = {};
    uMatrix eyeViewMatrix[2] = {};
    uMatrix identMatrix = {};
    uMatrix hmdMatrix = {};
    uMatrix controllerMatrix[4] = {};
    uMatrix genericMatrix[3] = {};
    int gTrackCount = 3;
    float currentIPD = 0.0f;
    bool printLogs = false;
    std::stringstream logError;
    vr::HmdVector2_t depthRange = {};
    vr::VRTextureBounds_t textureBounds[2] = {};
    bool asymmetricProjection = true;

    void InitalizeVR();

public:
    explicit simpleVR(bool showLogs = false);
    ~simpleVR();

    bool monoSubmit = true;
    float overscan = 1.0f;
    float overscanApplied = 0.0f;
    float unionTanX = 0.0f;
    float unionTanY = 0.0f;
    vr::VRTextureBounds_t monoBounds[2] = {};
    bool monoBoundsInit = false;

    void EnsureMonoBounds();
    bool PreloadVR();
    bool RefreshBufferSizeFromLive();
    bool StartVR();
    bool StopVR();
    bool isEnabled();
    void Recenter();
    POINT GetBufferSize();
    void SetProjection(vr::HmdVector2_t depth);
    void SetActionPose(vr::HmdMatrix34_t matPose, poseType poseType);
    void SetSkeletalPose(vr::VRBoneTransform_t* boneArray, int boneCount, poseType poseType);
    void SetFramePose();
    uMatrix GetFramePose(poseType poseType, int eye);
    bool GetHmdRawPose(vr::HmdMatrix34_t* out);
    void Render(ID3D11Texture2D* leftEye, ID3D11Texture2D* leftDepth,
        ID3D11Texture2D* rightEye, ID3D11Texture2D* rightDepth,
        const vr::HmdMatrix34_t* renderPose = nullptr);
    void WaitGetPoses();
    void MakeIPDOffset();
    bool HasErrors();
    std::string GetErrors();
};
