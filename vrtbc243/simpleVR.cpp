#include "simpleVR.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

extern std::ofstream ofOut;

namespace {
uMatrix identityMatrix()
{
    uMatrix result = {};
    result.matrix[0][0] = result.matrix[1][1] = 1.0f;
    result.matrix[2][2] = result.matrix[3][3] = 1.0f;
    return result;
}

uMatrix transposeOpenVR(const vr::HmdMatrix34_t& source)
{
    uMatrix result = {};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
            result.matrix[column][row] = source.m[row][column];
    result.matrix[3][3] = 1.0f;
    return result;
}

uMatrix transposeOpenVR(const vr::HmdMatrix44_t& source)
{
    uMatrix result = {};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            result.matrix[column][row] = source.m[row][column];
    return result;
}

float positiveMaximum(float a, float b, float c, float d)
{
    return (std::max)((std::max)(a, b), (std::max)(c, d));
}
}

simpleVR::simpleVR(bool showLogs) : printLogs(showLogs)
{
    InitalizeVR();
}

simpleVR::~simpleVR()
{
    StopVR();
}

void simpleVR::InitalizeVR()
{
    identMatrix = identityMatrix();
    hmdMatrix = identMatrix;
    for (int eye = 0; eye < 2; ++eye) {
        projMatrixRaw[eye] = identMatrix;
        eyeViewMatrixRaw[eye] = identMatrix;
        eyeViewMatrix[eye] = identMatrix;
    }
    for (uMatrix& matrix : controllerMatrix) matrix = identMatrix;
    for (uMatrix& matrix : genericMatrix) matrix = identMatrix;
}

bool simpleVR::PreloadVR()
{
#ifdef upstream_DISABLE_VR_INIT
    return false;
#else
    if (!vr::VR_IsHmdPresent()) return false;
    vr::EVRInitError error = vr::VRInitError_None;
    vr::IVRSystem* temporary = vr::VR_Init(&error, vr::VRApplication_Scene);
    if (error != vr::VRInitError_None || !temporary) {
        if (printLogs) logError << "VR_Init failed in PreloadVR: " << static_cast<int>(error) << '\n';
        return false;
    }
    uint32_t width = 0, height = 0;
    int32_t x = 0, y = 0;
    if (vr::IVRExtendedDisplay* display = vr::VRExtendedDisplay()) {
        display->GetWindowBounds(&x, &y, &width, &height);
        resolution = {static_cast<LONG>(width), static_cast<LONG>(height)};
    }
    temporary->GetRecommendedRenderTargetSize(&width, &height);
    bufferSize = {static_cast<LONG>(width), static_cast<LONG>(height)};
    vr::VR_Shutdown();
    return width != 0 && height != 0;
#endif
}

bool simpleVR::RefreshBufferSizeFromLive()
{
    if (!openVRSession) return false;
    uint32_t width = 0, height = 0;
    openVRSession->GetRecommendedRenderTargetSize(&width, &height);
    if (!width || !height) return false;
    bufferSize = {static_cast<LONG>(width), static_cast<LONG>(height)};
    return true;
}

void simpleVR::SetProjection(vr::HmdVector2_t depth)
{
    if (!openVRSession) return;
    if (depthRange.v[0] == depth.v[0] && depthRange.v[1] == depth.v[1] &&
        overscanApplied == overscan) return;
    depthRange = depth;
    overscanApplied = overscan;

    if (asymmetricProjection) {
        for (int eye = 0; eye < 2; ++eye) {
            const vr::EVREye vrEye = eye == 0 ? vr::Eye_Left : vr::Eye_Right;
            projMatrixRaw[eye] = transposeOpenVR(openVRSession->GetProjectionMatrix(
                vrEye, depth.v[0], depth.v[1]));
            const float scale = overscan > 1.0f ? overscan : 1.0f;
            projMatrixRaw[eye].matrix[0][0] /= scale;
            projMatrixRaw[eye].matrix[1][1] /= scale;
            float left = 0, right = 0, top = 0, bottom = 0;
            openVRSession->GetProjectionRaw(vrEye, &left, &right, &top, &bottom);
            textureBounds[eye].uMin = (left - scale * left) / (scale * (right - left));
            textureBounds[eye].uMax = (right - scale * left) / (scale * (right - left));
            textureBounds[eye].vMin = (scale * bottom - bottom) / (scale * (bottom - top));
            textureBounds[eye].vMax = (scale * bottom - top) / (scale * (bottom - top));
        }
        return;
    }

    float ll, lr, lt, lb, rl, rr, rt, rb;
    openVRSession->GetProjectionRaw(vr::Eye_Left, &ll, &lr, &lt, &lb);
    openVRSession->GetProjectionRaw(vr::Eye_Right, &rl, &rr, &rt, &rb);
    const float tanX = positiveMaximum(-ll, lr, -rl, rr);
    const float tanY = positiveMaximum(-lt, lb, -rt, rb);
    textureBounds[0] = {0.5f + 0.5f * ll / tanX, 0.5f - 0.5f * lb / tanY,
        0.5f + 0.5f * lr / tanX, 0.5f - 0.5f * lt / tanY};
    textureBounds[1] = {0.5f + 0.5f * rl / tanX, 0.5f - 0.5f * rb / tanY,
        0.5f + 0.5f * rr / tanX, 0.5f - 0.5f * rt / tanY};
    uMatrix projection = {};
    const float inverseDepth = 1.0f / (depth.v[1] - depth.v[0]);
    projection.matrix[0][0] = 1.0f / tanX;
    projection.matrix[1][1] = 1.0f / tanY;
    projection.matrix[2][2] = -depth.v[1] * inverseDepth;
    projection.matrix[2][3] = -1.0f;
    projection.matrix[3][2] = -depth.v[1] * depth.v[0] * inverseDepth;
    projMatrixRaw[0] = projMatrixRaw[1] = projection;
}

bool simpleVR::StartVR()
{
#ifdef upstream_DISABLE_VR_INIT
    return false;
#else
    if (_isConnected) return true;
    if (!vr::VR_IsHmdPresent()) return false;
    vr::EVRInitError error = vr::VRInitError_None;
    openVRSession = vr::VR_Init(&error, vr::VRApplication_Scene);
    if (error != vr::VRInitError_None || !openVRSession) {
        openVRSession = nullptr;
        if (printLogs) logError << "VR_Init failed in StartVR: " << static_cast<int>(error) << '\n';
        return false;
    }
    openVRChaperone = vr::VRChaperone();
    openVRModels = static_cast<vr::IVRRenderModels*>(
        vr::VR_GetGenericInterface(vr::IVRRenderModels_Version, &error));
    if (vr::IVRCompositor* compositor = vr::VRCompositor())
        compositor->SetTrackingSpace(vr::TrackingUniverseSeated);
    RefreshBufferSizeFromLive();
    SetProjection({0.06f, 1000.0f});
    eyeViewMatrixRaw[0] = transposeOpenVR(openVRSession->GetEyeToHeadTransform(vr::Eye_Left));
    eyeViewMatrixRaw[1] = transposeOpenVR(openVRSession->GetEyeToHeadTransform(vr::Eye_Right));
    MakeIPDOffset();
    _isConnected = true;
    monoBoundsInit = false;
    EnsureMonoBounds();
    Recenter();
    return true;
#endif
}

bool simpleVR::StopVR()
{
    if (!_isConnected && !openVRSession) return false;
    _isConnected = false;
    hmdRawPoseValid = false;
    openVRModels = nullptr;
    openVRChaperone = nullptr;
    openVRSession = nullptr;
    vr::VR_Shutdown();
    return true;
}

bool simpleVR::isEnabled() { return _isConnected; }

void simpleVR::Recenter()
{
    if (openVRChaperone)
        openVRChaperone->ResetZeroPose(vr::TrackingUniverseSeated);
}

POINT simpleVR::GetBufferSize() { return bufferSize; }

void simpleVR::SetFramePose()
{
    vr::IVRCompositor* compositor = vr::VRCompositor();
    if (!_isConnected || !openVRSession || !compositor) return;
    compositor->GetLastPoses(rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    hmdRawPoseValid = false;
    hmdMatrix = identMatrix;
    int genericIndex = 0;
    for (uint32_t index = 0; index < vr::k_unMaxTrackedDeviceCount; ++index) {
        const vr::TrackedDevicePose_t& pose = rTrackedDevicePose[index];
        if (!pose.bDeviceIsConnected || !pose.bPoseIsValid) continue;
        const vr::ETrackedDeviceClass deviceClass = openVRSession->GetTrackedDeviceClass(index);
        if (deviceClass == vr::TrackedDeviceClass_HMD) {
            hmdRawPose = pose.mDeviceToAbsoluteTracking;
            hmdRawPoseValid = true;
            hmdMatrix = transposeOpenVR(hmdRawPose);
        } else if (deviceClass == vr::TrackedDeviceClass_GenericTracker && genericIndex < 3) {
            genericMatrix[genericIndex++] = transposeOpenVR(pose.mDeviceToAbsoluteTracking);
        }
    }
}

void simpleVR::SetActionPose(vr::HmdMatrix34_t pose, poseType type)
{
    const uMatrix converted = transposeOpenVR(pose);
    switch (type) {
    case LeftHand: controllerMatrix[0] = converted; break;
    case LeftHandPalm: controllerMatrix[1] = converted; break;
    case RightHand: controllerMatrix[2] = converted; break;
    case RightHandPalm: controllerMatrix[3] = converted; break;
    default: break;
    }
}

void simpleVR::SetSkeletalPose(vr::VRBoneTransform_t*, int, poseType)
{
}

uMatrix simpleVR::GetFramePose(poseType type, int eye)
{
    switch (type) {
    case Projection: return eye >= 0 && eye < 2 ? projMatrixRaw[eye] : identMatrix;
    case EyeOffset: return eye >= 0 && eye < 2 ? eyeViewMatrix[eye] : identMatrix;
    case hmdPosition: return hmdMatrix;
    case LeftHand: return controllerMatrix[0];
    case LeftHandPalm: return controllerMatrix[1];
    case RightHand: return controllerMatrix[2];
    case RightHandPalm: return controllerMatrix[3];
    default: return identMatrix;
    }
}

bool simpleVR::GetHmdRawPose(vr::HmdMatrix34_t* output)
{
    if (!output || !hmdRawPoseValid) return false;
    *output = hmdRawPose;
    return true;
}

void simpleVR::EnsureMonoBounds()
{
    if (monoBoundsInit || !openVRSession) return;
    float ll, lr, lt, lb, rl, rr, rt, rb;
    openVRSession->GetProjectionRaw(vr::Eye_Left, &ll, &lr, &lt, &lb);
    openVRSession->GetProjectionRaw(vr::Eye_Right, &rl, &rr, &rt, &rb);
    unionTanX = positiveMaximum(-ll, lr, -rl, rr);
    unionTanY = positiveMaximum(-lt, lb, -rt, rb);
    if (unionTanX <= 0.0f || unionTanY <= 0.0f) return;
    monoBounds[0] = {0.5f + 0.5f * ll / unionTanX, 0.5f - 0.5f * lb / unionTanY,
        0.5f + 0.5f * lr / unionTanX, 0.5f - 0.5f * lt / unionTanY};
    monoBounds[1] = {0.5f + 0.5f * rl / unionTanX, 0.5f - 0.5f * rb / unionTanY,
        0.5f + 0.5f * rr / unionTanX, 0.5f - 0.5f * rt / unionTanY};
    monoBoundsInit = true;
    ofOut << "[svr] mono union tangents " << unionTanX << " x " << unionTanY << '\n';
    ofOut.flush();
}

void simpleVR::Render(ID3D11Texture2D* leftEye, ID3D11Texture2D* leftDepth,
    ID3D11Texture2D* rightEye, ID3D11Texture2D* rightDepth,
    const vr::HmdMatrix34_t* renderPose)
{
    (void)leftDepth;
    (void)rightDepth;
    vr::IVRCompositor* compositor = vr::VRCompositor();
    if (!_isConnected || !openVRSession || !compositor || !leftEye || !rightEye) return;
    EnsureMonoBounds();
    const vr::VRTextureBounds_t* leftBounds = monoSubmit ? &monoBounds[0] : &textureBounds[0];
    const vr::VRTextureBounds_t* rightBounds = monoSubmit ? &monoBounds[1] : &textureBounds[1];

    vr::EVRCompositorError leftError;
    vr::EVRCompositorError rightError;
    if (renderPose) {
        vr::VRTextureWithPose_t textures[2] = {};
        textures[0].handle = leftEye;
        textures[1].handle = rightEye;
        for (vr::VRTextureWithPose_t& texture : textures) {
            texture.eType = vr::TextureType_DirectX;
            texture.eColorSpace = vr::ColorSpace_Gamma;
            texture.mDeviceToAbsoluteTracking = *renderPose;
        }
        leftError = compositor->Submit(vr::Eye_Left,
            reinterpret_cast<vr::Texture_t*>(&textures[0]), leftBounds, vr::Submit_TextureWithPose);
        rightError = compositor->Submit(vr::Eye_Right,
            reinterpret_cast<vr::Texture_t*>(&textures[1]), rightBounds, vr::Submit_TextureWithPose);
    } else {
        vr::Texture_t textures[2] = {
            {leftEye, vr::TextureType_DirectX, vr::ColorSpace_Gamma},
            {rightEye, vr::TextureType_DirectX, vr::ColorSpace_Gamma}
        };
        leftError = compositor->Submit(vr::Eye_Left, &textures[0], leftBounds, vr::Submit_Default);
        rightError = compositor->Submit(vr::Eye_Right, &textures[1], rightBounds, vr::Submit_Default);
    }
    if (leftError != vr::VRCompositorError_None || rightError != vr::VRCompositorError_None) {
        logError << "VRCompositor Submit errors L=" << static_cast<int>(leftError)
            << " R=" << static_cast<int>(rightError) << '\n';
    }
}

void simpleVR::WaitGetPoses()
{
    if (vr::IVRCompositor* compositor = vr::VRCompositor())
        compositor->WaitGetPoses(rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
}

void simpleVR::MakeIPDOffset()
{
    eyeViewMatrix[0] = eyeViewMatrixRaw[0];
    eyeViewMatrix[1] = eyeViewMatrixRaw[1];
}

bool simpleVR::HasErrors() { return !logError.str().empty(); }

std::string simpleVR::GetErrors()
{
    std::string result = logError.str();
    logError.str(std::string());
    logError.clear();
    return result;
}
