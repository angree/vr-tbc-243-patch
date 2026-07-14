#include "cRenderObject.h"

RenderSquare::RenderSquare(IDirect3DDevice9* device) : RenderObject(device)
{
    if (!device) return;
    SetVertexBuffer({-1,-1,0,0,1, -1,1,0,0,0, 1,1,0,1,0, 1,-1,0,1,1}, 5);
    SetIndexBuffer({0,1,2, 0,2,3});
}
