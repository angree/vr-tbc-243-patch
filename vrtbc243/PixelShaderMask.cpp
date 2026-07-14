#include "cShaderData.h"

PixelShaderMask::PixelShaderMask()
{
    SetSource("MaskPixel", "ps_3_0", R"(
        sampler2D sourceTexture : register(s0);
        float4 main(float2 tex : TEXCOORD0) : COLOR0 {
            float4 sampleValue = tex2D(sourceTexture, tex);
            return sampleValue.a <= 0.05 ? float4(0,0,0,1) : float4(1,1,1,1);
        }
    )");
}
