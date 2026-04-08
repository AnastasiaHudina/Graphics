// Вершинный шейдер для skybox

cbuffer WorldMatrixBuffer : register(b0)
{
    matrix world;
};

cbuffer SceneMatrixBuffer : register(b1)
{
    matrix viewProj;
    matrix invViewProj;
    float4 cameraPosAndMode;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
};

struct VSOut
{
    float4 position : SV_Position;
    float3 localPos : POSITION1;
};

VSOut main(VSIn input)
{
    VSOut output;
    
    // Привязываем skybox к позиции камеры
    float3 worldPos = cameraPosAndMode.xyz + input.pos * 50.0f; // размер skybox
    
    // Трансформируем в clip space
    output.position = mul(float4(worldPos, 1.0), viewProj);
    
    // Локальная позиция для сэмплирования cubemap
    output.localPos = input.pos;
    
    return output;
}
