// Вершинный шейдер для рендеринга cubemap граней

cbuffer CubemapFaceConstants : register(b0)
{
    matrix viewProj;  // view * projection для конкретной грани
};

struct VSIn
{
    float3 pos : POSITION;
    float2 uv : TEXCOORD;
};

struct VSOut
{
    float4 svPos : SV_Position;
    float3 worldPos : POSITION;
};

VSOut main(VSIn input)
{
    VSOut output;

    // Позиция в мировом пространстве (относительно центра куба)
    output.worldPos = input.pos;

    // Трансформируем в clip space
    output.svPos = mul(float4(input.pos, 1.0), viewProj);

    return output;
}
