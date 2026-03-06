struct VSIn
{
    float3 position : POSITION;
    float2 texcoord : TEXCOORD;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
};

VSOut main(VSIn input)
{
    VSOut output;
    output.position = float4(input.position, 1.0f);
    output.texcoord = input.texcoord;
    return output;
}