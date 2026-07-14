// SEH helpers — pure C-style, no C++ destructors. Lets us use __try/__except
// to safely probe arbitrary memory pointers without crashing the process.
#include <Windows.h>
#include <d3d9.h>

extern "C" __declspec(noinline)
IDirect3DDevice9* SEH_ScanForDevice(void* ecx, int maxOffset, int* foundOffset)
{
    IDirect3DDevice9* result = NULL;
    *foundOffset = -1;
    for (int offset = 0; offset < maxOffset; offset += 4) {
        DWORD val = *(DWORD*)((BYTE*)ecx + offset);
        // pointer sanity: must point to user-mode VA range
        if (val < 0x10000 || val >= 0x80000000) continue;
        __try {
            IDirect3DDevice9* candidate = (IDirect3DDevice9*)val;
            D3DDEVICE_CREATION_PARAMETERS dcp;
            ZeroMemory(&dcp, sizeof(dcp));
            HRESULT hr = candidate->GetCreationParameters(&dcp);
            if (hr == S_OK) {
                result = candidate;
                *foundOffset = offset;
                break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // not a valid pointer — continue
        }
    }
    return result;
}

// fix #73: run one submit-thread iteration body under SEH. If anything inside
// dereferences a dead pointer (resource teardown race during loading screens),
// we swallow the fault and skip the iteration instead of crashing the game.
extern "C" __declspec(noinline)
int SEH_GuardedCall(void (*fn)(void))
{
    __try { fn(); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
