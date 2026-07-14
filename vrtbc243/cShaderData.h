#pragma once

#include <D3D9.h>
#include <D3DCompiler.h>
#include <string>
#include <sstream>
#include <vector>

// One compiled shader (vertex or pixel) together with its optional input
// layout. Concrete shaders are declared as small subclasses whose constructor
// supplies the HLSL text; CompileShaderFromString then turns that text into the
// live Direct3D 9 objects exposed through the public handles below.
class ShaderData
{
public:
    IDirect3DVertexShader9*      VS     = nullptr;
    IDirect3DPixelShader9*       PS     = nullptr;
    IDirect3DVertexDeclaration9* Layout = nullptr;

    void SetSource(std::string name, std::string version, std::string source);
    void ClearLayout();
    void AddLayout(D3DVERTEXELEMENT9 element);
    bool CompileShaderFromString(IDirect3DDevice9* dev);
    void Release();

private:
    std::string sName;
    std::string sVersion;
    std::string sSource;
    std::vector<D3DVERTEXELEMENT9> rawLayout;
};

// Vertex shader used for textured quads (UI plane, cursor).
class VertexShaderTexture : public ShaderData { public: VertexShaderTexture(); };

// Pixel shaders: plain texture, alpha mask, textured-with-mouse-dot.
class PixelShaderTexture      : public ShaderData { public: PixelShaderTexture(); };
class PixelShaderMask         : public ShaderData { public: PixelShaderMask(); };
class PixelShaderWithMouseDot : public ShaderData { public: PixelShaderWithMouseDot(); };
