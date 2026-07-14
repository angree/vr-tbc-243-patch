#include "proxydll.h"

#include "game_extras.h"
#include "simpleVR.h"

#include <new>

extern simpleVR* svr;

cIDirect3DDevice9* glIDirect3DDevice9 = nullptr;
cIDirect3DDevice9Ex* glIDirect3DDevice9Ex = nullptr;
cIDirect3D9* glIDirect3D9 = nullptr;
HINSTANCE glOriginalDll = nullptr;
HINSTANCE glThisInstance = nullptr;

std::ofstream ofOut;
std::stringstream logError;
bool doLog = false;

namespace {
template <typename Function>
Function originalExport(const char* name)
{
    if (!glOriginalDll || !name) return nullptr;
    return reinterpret_cast<Function>(GetProcAddress(glOriginalDll, name));
}

bool loadSystemD3D9()
{
    if (glOriginalDll) return true;
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 10) {
        logError << "GetSystemDirectoryW failed: " << GetLastError() << '\n';
        return false;
    }
    if (wcscat_s(systemDirectory, L"\\d3d9.dll") != 0) {
        logError << "System d3d9 path is too long\n";
        return false;
    }
    glOriginalDll = LoadLibraryW(systemDirectory);
    if (!glOriginalDll)
        logError << "LoadLibraryW(system d3d9.dll) failed: " << GetLastError() << '\n';
    return glOriginalDll != nullptr;
}

void initializeProxy(HMODULE module)
{
    glThisInstance = module;
    ofOut.open("./vr_version/output.txt", std::ios::out | std::ios::trunc);
    ofOut.precision(5);
    ofOut << "=== InitInstance start ===\n";

    if (!loadSystemD3D9()) {
        PrintErrors();
        return;
    }
    ofOut << "[init] system d3d9.dll loaded\n";

    // SteamVR Direct Mode waits during D3D9 device creation. Scene-app
    // registration therefore has to complete before any game detours are active.
    bool vrStarted = false;
    if (svr) vrStarted = svr->StartVR();
    ofOut << "[init] VR pre-registration: " << (vrStarted ? "OK" : "FAILED") << '\n';
    PrintErrors();

    InitDetours(module);
    PrintErrors();
    ofOut << "=== InitInstance done ===\n";
    ofOut.flush();
}

void shutdownProxy()
{
    ExitDetours();
    if (glOriginalDll) {
        FreeLibrary(glOriginalDll);
        glOriginalDll = nullptr;
    }
    PrintErrors();
    if (ofOut.is_open()) ofOut.close();
}
}

bool HasErrors()
{
    return !logError.str().empty();
}

std::string GetErrors()
{
    std::string result = logError.str();
    logError.str(std::string());
    logError.clear();
    return result;
}

void PrintErrors()
{
    if (!HasErrors()) return;
    if (ofOut.is_open()) {
        ofOut << GetErrors();
        ofOut.flush();
    } else {
        GetErrors();
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        initializeProxy(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        shutdownProxy();
    }
    return TRUE;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name)
{
    using Function = int (WINAPI*)(D3DCOLOR, LPCWSTR);
    Function call = originalExport<Function>("D3DPERF_BeginEvent");
    return call ? call(color, name) : -1;
}

int WINAPI D3DPERF_EndEvent()
{
    using Function = int (WINAPI*)();
    Function call = originalExport<Function>("D3DPERF_EndEvent");
    return call ? call() : -1;
}

DWORD WINAPI D3DPERF_GetStatus()
{
    using Function = DWORD (WINAPI*)();
    Function call = originalExport<Function>("D3DPERF_GetStatus");
    return call ? call() : 0;
}

BOOL WINAPI D3DPERF_QueryRepeatFrame()
{
    using Function = BOOL (WINAPI*)();
    Function call = originalExport<Function>("D3DPERF_QueryRepeatFrame");
    return call ? call() : FALSE;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name)
{
    using Function = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    Function call = originalExport<Function>("D3DPERF_SetMarker");
    if (call) call(color, name);
}

void WINAPI D3DPERF_SetOptions(DWORD options)
{
    using Function = void (WINAPI*)(DWORD);
    Function call = originalExport<Function>("D3DPERF_SetOptions");
    if (call) call(options);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name)
{
    using Function = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    Function call = originalExport<Function>("D3DPERF_SetRegion");
    if (call) call(color, name);
}

IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    using Create9Ex = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
    Create9Ex create = originalExport<Create9Ex>("Direct3DCreate9Ex");
    if (!create) {
        logError << "Direct3DCreate9Ex export not found\n";
        PrintErrors();
        return nullptr;
    }
    IDirect3D9Ex* original = nullptr;
    const HRESULT result = create(sdkVersion, &original);
    if (FAILED(result) || !original) {
        logError << "Direct3DCreate9Ex failed: 0x" << std::hex << result << std::dec << '\n';
        PrintErrors();
        return nullptr;
    }
    cIDirect3D9* wrapper = new (std::nothrow) cIDirect3D9(original, doLog, &logError);
    if (!wrapper) {
        original->Release();
        return nullptr;
    }
    glIDirect3D9 = wrapper;
    return wrapper;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** output)
{
    if (!output) return D3DERR_INVALIDCALL;
    *output = nullptr;
    using Create9Ex = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
    Create9Ex create = originalExport<Create9Ex>("Direct3DCreate9Ex");
    if (!create) return D3DERR_NOTAVAILABLE;

    IDirect3D9Ex* original = nullptr;
    const HRESULT result = create(sdkVersion, &original);
    if (FAILED(result) || !original) return result;
    cIDirect3D9* wrapper = new (std::nothrow) cIDirect3D9(original, doLog, &logError);
    if (!wrapper) {
        original->Release();
        return E_OUTOFMEMORY;
    }
    glIDirect3D9 = wrapper;
    *output = wrapper;
    return result;
}

void WINAPI Direct3DShaderValidatorCreate9()
{
    using Function = void (WINAPI*)();
    Function call = originalExport<Function>("Direct3DShaderValidatorCreate9");
    if (call) call();
}
