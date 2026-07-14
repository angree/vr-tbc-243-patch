#include "cRenderObject.h"

#include <cmath>

// UI panel curvature knob (defined in game_extras.cpp): +1 concave (original), 0 flat,
// -1 convex. The z-curve below reproduces the original concave panel exactly at +1.0.
extern volatile float g_screenCurve;

RenderCurvedUI::RenderCurvedUI(IDirect3DDevice9* device) : RenderObject(device)
{
    if (!device) return;
    constexpr int strips = 17;
    constexpr float pi = 3.14159265358979323846f;
    const float curve = g_screenCurve;
    std::vector<float> vertices;
    for (int i = 0; i <= strips; ++i) {
        const float angle = (static_cast<float>(i) / strips - 0.5f) * (pi / 3.0f);
        const float x = 0.5f * std::sin(angle) / std::sin(pi / 6.0f);
        const float z = -0.098222f + curve * 0.243876f * (1.0f - std::cos(angle)) / (1.0f - std::cos(pi / 6.0f));
        const float u = static_cast<float>(i) / strips;
        vertices.insert(vertices.end(), {x,-0.5f,z,u,1.0f, x,0.5f,z,u,0.0f});
    }
    std::vector<short> indices;
    for (int i = 0; i < strips; ++i) {
        const short a = static_cast<short>(i * 2);
        indices.insert(indices.end(), {a,static_cast<short>(a+1),static_cast<short>(a+3),
            static_cast<short>(a+3),static_cast<short>(a+2),a});
    }
    SetVertexBuffer(vertices, 5);
    SetIndexBuffer(indices);
}
