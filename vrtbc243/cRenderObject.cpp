#include "cRenderObject.h"

#include <cstring>

RenderObject::RenderObject() : dev(nullptr) {}

RenderObject::RenderObject(IDirect3DDevice9* device) : dev(device) {}

bool RenderObject::SetVertexBuffer(std::vector<float> vertices, int itemStride,
    bool keepBuffer, D3DPOOL pool)
{
    vertexList = std::move(vertices);
    stride = itemStride > 0 ? static_cast<unsigned int>(itemStride) : 0;
    byteStride = stride * sizeof(float);
    vertexCount = stride == 0 ? 0 : static_cast<int>(vertexList.size() / stride);
    vertexSet = false;
    if (vertexList.empty() || stride == 0) return false;

    if (!keepBuffer) {
        if (vertexBuffer) { vertexBuffer->Release(); vertexBuffer = nullptr; }
        if (!dev) return false;
        if (FAILED(dev->CreateVertexBuffer(static_cast<UINT>(vertexList.size() * sizeof(float)),
            D3DUSAGE_WRITEONLY, 0, pool, &vertexBuffer, nullptr))) return false;
    }
    MapResourceVertex(vertexList.data(), static_cast<int>(vertexList.size() * sizeof(float)));
    vertexSet = vertexBuffer != nullptr;
    return vertexSet;
}

int RenderObject::GetVertexCount() { return vertexCount; }

bool RenderObject::SetIndexBuffer(std::vector<short> indices, bool keepBuffer, D3DPOOL pool)
{
    indexList = std::move(indices);
    indexCount = static_cast<int>(indexList.size());
    indexSet = false;
    if (indexList.empty()) return false;

    if (!keepBuffer) {
        if (indexBuffer) { indexBuffer->Release(); indexBuffer = nullptr; }
        if (!dev) return false;
        if (FAILED(dev->CreateIndexBuffer(static_cast<UINT>(indexList.size() * sizeof(short)),
            D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, pool, &indexBuffer, nullptr))) return false;
    }
    MapResourceIndex(indexList.data(), static_cast<int>(indexList.size() * sizeof(short)));
    indexSet = indexBuffer != nullptr;
    return indexSet;
}

void RenderObject::SetShadersLayout(IDirect3DVertexDeclaration9* layout,
    IDirect3DVertexShader9* vertex, IDirect3DPixelShader9* pixel)
{
    structLayout = layout;
    vertexShader = vertex;
    pixelShader = pixel;
}

void RenderObject::MapResourceVertex(void* data, int size)
{
    if (!vertexBuffer || !data || size <= 0) return;
    void* mapped = nullptr;
    if (SUCCEEDED(vertexBuffer->Lock(0, 0, &mapped, 0))) {
        std::memcpy(mapped, data, static_cast<size_t>(size));
        vertexBuffer->Unlock();
    }
}

void RenderObject::MapResourceIndex(void* data, int size)
{
    if (!indexBuffer || !data || size <= 0) return;
    void* mapped = nullptr;
    if (SUCCEEDED(indexBuffer->Lock(0, 0, &mapped, 0))) {
        std::memcpy(mapped, data, static_cast<size_t>(size));
        indexBuffer->Unlock();
    }
}

bool RenderObject::RayTest(XMVECTOR origin, XMVECTOR direction, XMVECTOR v0,
    XMVECTOR v1, XMVECTOR v2, float* u, float* v, float* w, float* distance,
    std::stringstream*)
{
    const XMVECTOR edge1 = v1 - v0;
    const XMVECTOR edge2 = v2 - v0;
    const XMVECTOR p = XMVector3Cross(direction, edge2);
    float determinant = XMVectorGetX(XMVector3Dot(edge1, p));
    if (determinant > -0.0001f && determinant < 0.0001f) return false;

    const float inverse = 1.0f / determinant;
    const XMVECTOR offset = origin - v0;
    *u = XMVectorGetX(XMVector3Dot(offset, p)) * inverse;
    if (*u < 0.0f || *u > 1.0f) return false;
    const XMVECTOR q = XMVector3Cross(offset, edge1);
    *v = XMVectorGetX(XMVector3Dot(direction, q)) * inverse;
    if (*v < 0.0f || *u + *v > 1.0f) return false;
    *distance = XMVectorGetX(XMVector3Dot(edge2, q)) * inverse;
    if (*distance > 0.0f) return false;
    *w = 1.0f - *u - *v;
    return true;
}

bool RenderObject::RayIntersection(XMVECTOR origin, XMVECTOR direction,
    std::vector<intersectPoint>* hits, std::vector<bool> interactable,
    std::stringstream* logError)
{
    if (!hits) return false;
    const int triangleCount = indexSet ? indexCount / 3 : vertexCount / 3;
    bool found = false;
    for (int triangle = 0; triangle < triangleCount; ++triangle) {
        const int base = triangle * 3;
        const int i0 = indexSet ? indexList[base] : base;
        const int i1 = indexSet ? indexList[base + 1] : base + 1;
        const int i2 = indexSet ? indexList[base + 2] : base + 2;
        const bool enabled = interactable.empty() ||
            (triangle < static_cast<int>(interactable.size()) && interactable[triangle]);
        if (!enabled || stride < 5) continue;

        XMVECTOR p0 = XMVectorSet(vertexList[i0 * stride], vertexList[i0 * stride + 1], vertexList[i0 * stride + 2], 1.0f);
        XMVECTOR p1 = XMVectorSet(vertexList[i1 * stride], vertexList[i1 * stride + 1], vertexList[i1 * stride + 2], 1.0f);
        XMVECTOR p2 = XMVectorSet(vertexList[i2 * stride], vertexList[i2 * stride + 1], vertexList[i2 * stride + 2], 1.0f);
        p0 = XMVector3Transform(p0, objMatrix);
        p1 = XMVector3Transform(p1, objMatrix);
        p2 = XMVector3Transform(p2, objMatrix);
        float u = 0, v = 0, w = 0, distance = 0;
        if (RayTest(origin, direction, p0, p1, p2, &u, &v, &w, &distance, logError)) {
            XMVECTOR uv0 = XMVectorSet(vertexList[i0 * stride + 3], vertexList[i0 * stride + 4], 1.0f, 0.0f);
            XMVECTOR uv1 = XMVectorSet(vertexList[i1 * stride + 3], vertexList[i1 * stride + 4], 1.0f, 0.0f);
            XMVECTOR uv2 = XMVectorSet(vertexList[i2 * stride + 3], vertexList[i2 * stride + 4], 1.0f, 0.0f);
            hits->emplace_back(distance, u * uv1 + v * uv2 + w * uv0);
            found = true;
        }
    }
    return found;
}

void RenderObject::Render(D3DPRIMITIVETYPE type, int start, int count)
{
    if (!dev) return;
    // safety: if shaders/declaration failed to build, draw nothing instead of feeding a
    // NULL vertex declaration to the device (that caused ERROR #132 in real d3d9).
    if (!structLayout || !vertexShader) return;
    dev->SetVertexDeclaration(structLayout);
    dev->SetVertexShader(vertexShader);
    dev->SetPixelShader(pixelShader);
    if (vertexSet) dev->SetStreamSource(0, vertexBuffer, 0, byteStride);

    int primitives = type == D3DPT_LINELIST ? vertexCount / 2 :
        (indexSet ? indexCount / 3 : vertexCount / 3);
    if (count > 0) primitives = count;
    if (indexSet) {
        dev->SetIndices(indexBuffer);
        dev->DrawIndexedPrimitive(type, 0, 0, vertexCount, start, primitives);
    } else {
        dev->DrawPrimitive(type, start, primitives);
    }
}

void RenderObject::SetObjectMatrix(XMMATRIX matrix) { objMatrix = matrix; }

XMMATRIX RenderObject::GetObjectMatrix(bool inverse, bool transpose)
{
    XMMATRIX result = inverse ? XMMatrixInverse(nullptr, objMatrix) : objMatrix;
    return transpose ? XMMatrixTranspose(result) : result;
}

void RenderObject::Release()
{
    if (vertexBuffer) { vertexBuffer->Release(); vertexBuffer = nullptr; }
    if (indexBuffer) { indexBuffer->Release(); indexBuffer = nullptr; }
    structLayout = nullptr;
    vertexShader = nullptr;
    pixelShader = nullptr;
    vertexCount = indexCount = 0;
    vertexSet = indexSet = false;
    dev = nullptr;
}
