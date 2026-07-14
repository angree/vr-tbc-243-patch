#include "cShaderData.h"

VertexShaderTexture::VertexShaderTexture()
{
    AddLayout({0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0});
    AddLayout({0,12,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0});
    AddLayout(D3DDECL_END());
    SetSource("TextureVertex", "vs_3_0", R"(
        struct Input { float4 position : POSITION; float2 tex : TEXCOORD0; };
        struct Output { float4 position : POSITION; float2 tex : TEXCOORD0; };
        float4x4 projectionMatrix : register(c0);
        float4x4 viewMatrix : register(c4);
        float4x4 worldMatrix : register(c8);
        Output main(Input input) {
            Output output;
            output.position = mul(mul(mul(float4(input.position.xyz, 1), worldMatrix), viewMatrix), projectionMatrix);
            output.tex = input.tex;
            return output;
        }
    )");
}
