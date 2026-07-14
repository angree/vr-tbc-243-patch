#pragma once

// Common includes for the VR layer and the two hook lifecycle entry points.
// game_extras.cpp installs every Microsoft Detours hook inside InitDetours and
// removes them again in ExitDetours; proxydll.cpp calls both from DllMain.

#undef UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>
#include <DirectXMath.h>

#include <list>
#include <fstream>
#include <iostream>

#include "detours.h"
#include "simpleVR.h"
#include "stDX11.h"
#include "stBasicTexture.h"

using namespace DirectX;

#define DllExport __declspec(dllexport)
#define DllImport __declspec(dllimport)

void InitDetours(HANDLE hModule);
void ExitDetours();
