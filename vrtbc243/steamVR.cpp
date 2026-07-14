#include "steamVR.h"

#include <algorithm>

bool setActiveJSON(std::string relativePath)
{
    char executable[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, executable, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    std::string fullPath(executable, length);
    std::replace(fullPath.begin(), fullPath.end(), '\\', '/');
    const std::string::size_type separator = fullPath.find_last_of('/');
    if (separator == std::string::npos) return false;
    fullPath.resize(separator + 1);
    fullPath += relativePath;
    if (GetFileAttributesA(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    vr::IVRInput* input = vr::VRInput();
    return input && input->SetActionManifestPath(fullPath.c_str()) == vr::VRInputError_None;
}

bool setActionHandlesGame(inputController* controller)
{
    vr::IVRInput* input = vr::VRInput();
    if (!controller || !input) return false;
    bool ok = input->GetActionSetHandle("/actions/game", &controller->game.setHandle) == vr::VRInputError_None;
    const auto bind = [&](const char* path, vr::VRActionHandle_t& handle) {
        if (input->GetActionHandle(path, &handle) != vr::VRInputError_None) ok = false;
    };
    bind("/actions/game/in/movement", controller->game.movement);
    bind("/actions/game/in/rotate", controller->game.rotate);
    bind("/actions/game/in/lefthand", controller->game.lefthand);
    bind("/actions/game/in/lefthandpalm", controller->game.lefthandpalm);
    bind("/actions/game/in/righthand", controller->game.righthand);
    bind("/actions/game/in/righthandpalm", controller->game.righthandpalm);
    bind("/actions/game/in/button_y", controller->game.button_y);
    bind("/actions/game/in/button_x", controller->game.button_x);
    bind("/actions/game/in/button_a", controller->game.button_a);
    bind("/actions/game/in/button_b", controller->game.button_b);
    bind("/actions/game/in/left_trigger", controller->game.left_trigger);
    bind("/actions/game/in/left_bumper", controller->game.left_bumper);
    bind("/actions/game/in/left_stick_click", controller->game.left_stick_click);
    bind("/actions/game/in/left_anim", controller->game.left_anim);
    bind("/actions/game/in/right_trigger", controller->game.right_trigger);
    bind("/actions/game/in/right_bumper", controller->game.right_bumper);
    bind("/actions/game/in/right_stick_click", controller->game.right_stick_click);
    bind("/actions/game/in/right_anim", controller->game.right_anim);
    bind("/actions/game/in/menu_start", controller->game.menu_start);
    bind("/actions/game/in/menu_select", controller->game.menu_select);
    bind("/actions/game/out/haptic_left", controller->game.haptic_left);
    bind("/actions/game/out/haptic_right", controller->game.haptic_right);
    return ok;
}
