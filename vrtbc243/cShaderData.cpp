#include "cShaderData.h"

#include <cstring>

void ShaderData::SetSource(std::string name, std::string version, std::string source)
{
    sName = std::move(name);
    sVersion = std::move(version);
    sSource = std::move(source);
}

void ShaderData::ClearLayout() { rawLayout.clear(); }

void ShaderData::AddLayout(D3DVERTEXELEMENT9 element) { rawLayout.push_back(element); }

bool ShaderData::CompileShaderFromString(IDirect3DDevice9* device)
{
    if (!device || sSource.empty() || sVersion.empty()) return false;
    ID3DBlob* bytecode = nullptr;
    ID3DBlob* diagnostics = nullptr;
    UINT flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    // fix (crash #1 of Codex rewrite): the rewritten shaders define their HLSL entry as
    // "main", but this passed sName ("TextureVertex" etc.) as the entrypoint -> D3DCompile
    // "entrypoint not found" -> all 6 shaders null -> RenderObject draws with a NULL vertex
    // declaration -> real d3d9 reads +0x24 off null -> ERROR #132. Entrypoint is "main".
    const HRESULT compile = D3DCompile(sSource.data(), sSource.size(), sName.c_str(),
        nullptr, nullptr, "main", sVersion.c_str(), flags, 0, &bytecode, &diagnostics);
    if (FAILED(compile) || !bytecode) {
        if (diagnostics) diagnostics->Release();
        return false;
    }

    HRESULT created = E_FAIL;
    if (sVersion.compare(0, 3, "vs_") == 0) {
        if (rawLayout.empty()) { bytecode->Release(); if (diagnostics) diagnostics->Release(); return false; }
        created = device->CreateVertexDeclaration(rawLayout.data(), &Layout);
        if (SUCCEEDED(created)) created = device->CreateVertexShader(
            static_cast<const DWORD*>(bytecode->GetBufferPointer()), &VS);
    } else if (sVersion.compare(0, 3, "ps_") == 0) {
        created = device->CreatePixelShader(
            static_cast<const DWORD*>(bytecode->GetBufferPointer()), &PS);
    }
    bytecode->Release();
    if (diagnostics) diagnostics->Release();
    if (FAILED(created)) {
        if (VS) { VS->Release(); VS = nullptr; }
        if (PS) { PS->Release(); PS = nullptr; }
        if (Layout) { Layout->Release(); Layout = nullptr; }
        return false;
    }
    return true;
}

void ShaderData::Release()
{
    if (VS) { VS->Release(); VS = nullptr; }
    if (PS) { PS->Release(); PS = nullptr; }
    if (Layout) { Layout->Release(); Layout = nullptr; }
}
