#pragma once

#include <D3D9.h>
#include <DirectXMath.h>
#include <vector>
#include <sstream>
#include <iostream>

using namespace DirectX;

// One ray hit against the mesh: distance along the ray and the point in
// object space where it landed. Used to place the UI cursor under the gaze.
struct intersectPoint
{
    float    distance;
    XMVECTOR point;

    intersectPoint(float distance, XMVECTOR point)
        : distance(distance), point(point)
    {
    }
};

// A drawable quad / line mesh with its own vertex and index buffers, a shader
// set and a world matrix. This backs the curved UI plane, the VR mouse cursor
// and the UI alpha mask. RayIntersection projects the gaze ray onto the mesh so
// the flat-screen mouse can be driven from where the player is looking.
class RenderObject
{
public:
    RenderObject();
    RenderObject(IDirect3DDevice9* tdev);

    bool SetVertexBuffer(std::vector<float> vertices, int itmStride, bool ignoreCreateBuffer = false, D3DPOOL usage = D3DPOOL_DEFAULT);
    bool SetIndexBuffer(std::vector<short> indices, bool ignoreCreateBuffer = false, D3DPOOL usage = D3DPOOL_DEFAULT);
    int  GetVertexCount();
    void SetShadersLayout(IDirect3DVertexDeclaration9* layout, IDirect3DVertexShader9* vertex, IDirect3DPixelShader9* pixel);
    void MapResourceVertex(void* data, int size);
    void MapResourceIndex(void* data, int size);

    void     SetObjectMatrix(DirectX::XMMATRIX matrix);
    XMMATRIX GetObjectMatrix(bool inverse = false, bool transpose = false);

    bool RayIntersection(XMVECTOR origin, XMVECTOR direction, std::vector<intersectPoint>* intersection, std::vector<bool> interactable, std::stringstream* logError);
    bool RayTest(XMVECTOR origin, XMVECTOR direction, XMVECTOR v0, XMVECTOR v1, XMVECTOR v2, float* barycentricU, float* barycentricV, float* barycentricW, float* distance, std::stringstream* logError);

    void Render(D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST, int start = 0, int count = 0);
    void Release();

    // GPU resources for this mesh.
    IDirect3DVertexDeclaration9* structLayout = nullptr;
    IDirect3DVertexShader9*      vertexShader = nullptr;
    IDirect3DPixelShader9*       pixelShader  = nullptr;
    IDirect3DVertexBuffer9*      vertexBuffer = nullptr;
    IDirect3DIndexBuffer9*       indexBuffer  = nullptr;

    // CPU-side copies and geometry counts.
    std::vector<float> vertexList;
    std::vector<short> indexList;

    XMMATRIX     objMatrix   = XMMatrixIdentity();
    unsigned int stride      = 0;
    unsigned int byteStride  = 0;
    int          vertexCount = 0;
    int          indexCount  = 0;

    bool vertexSet = false;
    bool indexSet  = false;

private:
    IDirect3DDevice9* dev;
};

// Concrete meshes built on top of RenderObject.
class RenderCurvedUI : public RenderObject { public: RenderCurvedUI(IDirect3DDevice9* tdev); };
class RenderSquare   : public RenderObject { public: RenderSquare(IDirect3DDevice9* tdev); };
class RenderMaskUI   : public RenderObject { public: RenderMaskUI(IDirect3DDevice9* tdev); };
