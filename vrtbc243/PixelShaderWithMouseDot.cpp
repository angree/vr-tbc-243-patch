#include "cShaderData.h"

PixelShaderWithMouseDot::PixelShaderWithMouseDot()
{
    SetSource("FilteredTexturePixel", "ps_3_0", R"(
        sampler2D sourceTexture : register(s0);
        float4 main(float2 tex : TEXCOORD0) : COLOR0 {
            float2 dx = ddx(tex) * 0.25;
            float2 dy = ddy(tex) * 0.25;
            return (tex2D(sourceTexture, tex + dx + dy) + tex2D(sourceTexture, tex + dx - dy)
                + tex2D(sourceTexture, tex - dx + dy) + tex2D(sourceTexture, tex - dx - dy)) * 0.25;
        }
    )");
}
