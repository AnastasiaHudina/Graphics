// Константные буферы
cbuffer WorldMatrix : register(b0)
{
    matrix world;
};

cbuffer ViewProjMatrix : register(b1)
{
    matrix viewProj;
};

// Входная структура
struct VSIn
{
    float3 position : POSITION;
    float4 color : COLOR;
};

// Выходная структура
struct VSOut
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

VSOut main(VSIn input)
{
    VSOut output;
    
    // Преобразование позиции
    float4 worldPos = mul(float4(input.position, 1.0f), world);
    output.position = mul(worldPos, viewProj);
    
    // Передача цвета
    output.color = input.color;
    
    return output;
}
