#pragma once

// d3d9.dll proxy: the shared Win32 / Direct3D 9 includes plus a tiny global
// error channel. The concrete device wrappers only need to include this one
// header to get everything they depend on (d3d9 types + std::stringstream).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <string>
#include <sstream>
#include <fstream>

#include "cIDirect3D9.h"
#include "cIDirect3DDevice9.h"

// Global error log, written by the wrappers and drained at shutdown.
bool        HasErrors();
std::string GetErrors();
void        PrintErrors();
