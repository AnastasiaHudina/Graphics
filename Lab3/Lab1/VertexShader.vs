cbuffer WorldMatrix : register(b0)
{
    matrix world;
};

cbuffer FrameConstants : register(b1)
{
    matrix viewProj;
    matrix invViewProj;
    float4 cameraPosAndMode; // xyz = camera pos, w = view mode
};

struct VSIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
};

struct VSOut
{
    float4 position : SV_Position;
    float4 worldPos : POSITION;  
    float3 normal : NORMAL;
    float4 color : COLOR;
};

VSOut main(VSIn input)
{
    VSOut output;
    float4 worldPos = mul(float4(input.position, 1.0f), world);
    output.position = mul(worldPos, viewProj);
    output.worldPos = worldPos;
    output.normal = mul(input.normal, (float3x3)world);
    output.color = input.color;
    return output;
}