#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <openvr.h>

#include <string>

struct inputActionGame
{
    vr::VRActionSetHandle_t setHandle = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t movement = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t rotate = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t lefthand = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t lefthandpalm = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t righthand = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t righthandpalm = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t button_y = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t button_x = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t button_a = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t button_b = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t left_trigger = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t left_bumper = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t left_stick_click = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t left_anim = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t right_trigger = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t right_bumper = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t right_stick_click = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t right_anim = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t menu_start = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t menu_select = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t haptic_left = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t haptic_right = vr::k_ulInvalidActionHandle;
};

struct inputController { inputActionGame game; };

bool setActiveJSON(std::string relativeFilePath);
bool setActionHandlesGame(inputController* input);
