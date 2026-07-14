#include "cShaderData.h"

PixelShaderTexture::PixelShaderTexture()
{
    SetSource("TexturePixel", "ps_3_0", R"(
        sampler2D sourceTexture : register(s0);
        float4 main(float2 tex : TEXCOORD0) : COLOR0 { return tex2D(sourceTexture, tex); }
    )");
}
