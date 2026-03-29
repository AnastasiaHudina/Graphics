#pragma once

#include <d3d11_1.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <chrono>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ImGui Headers - добавляем файлы из папки "imgui"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

class Render
{
public:
    Render();
    ~Render();

    HRESULT Initialize(HWND hwnd);
    void Shutdown();
    void DrawScene();
    void HandleResize(HWND hwnd);

    void ToggleAutoRotate();   // переключение автоматического вращения

    // View / debug modes
    // 0 = PBR shaded, 1 = NDF, 2 = Geometry, 3 = Fresnel
    void SetViewMode(int mode);

    // Управление камерой
    void MoveView(float dx, float dy, float dz);
    void RotateView(float yaw, float pitch);
    void MoveForward(float distance);
    void RotateAroundTarget(float dx, float dy);   // вращение вокруг центра (для мыши)
    void Zoom(float delta);                         // изменение радиуса (для колёсика)

private:
    HRESULT SetupDevice(HWND hwnd);
    HRESULT SetupBackBuffer();
    HRESULT SetupDepthStencil(UINT width, UINT height);
    HRESULT CreateGeometry();
    HRESULT LoadShaders();
    void UpdateTransforms();
    void SetDebugNames();

    bool m_autoRotate;         // true - вращается, false - остановлен
    bool m_imguiInitialized = false;

    // D3D11 объекты
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTarget;
    ComPtr<ID3D11DepthStencilView> m_depthStencil;
    ComPtr<ID3D11Texture2D> m_depthBuffer;
    ComPtr<ID3DUserDefinedAnnotation> m_annotation;

    // Геометрия
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11Buffer> m_indexBuffer;

    // Шейдеры
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;

    // Константные буферы
    ComPtr<ID3D11Buffer> m_worldBuffer;
    ComPtr<ID3D11Buffer> m_viewProjBuffer;
    ComPtr<ID3D11Buffer> m_materialBuffer;

    // Параметры камеры
    XMFLOAT3 m_cameraPos;
    float m_yawAngle;
    float m_pitchAngle;
    float m_rotationAngle;

    HWND m_hwnd;

    // источники света
    struct Light
    {
        DirectX::XMFLOAT4 position;   // позиция (x,y,z,1)
        DirectX::XMFLOAT4 color;      // rgb, интенсивность в w
    };
    std::vector<Light> m_lights;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_lightBuffer;

    // Material / debug state
    DirectX::XMFLOAT3 m_baseColor = DirectX::XMFLOAT3(0.9f, 0.7f, 0.2f);
    float m_roughness = 0.35f;
    float m_metalness = 0.0f;
    int m_viewMode = 0;

    // Environment (cubemap) background
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_envCubemap;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_envSRV;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_environmentPS;
    HRESULT CreateEnvironmentResources();

    // IBL: HDRI → Cubemap → Irradiance
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_hdriTexture;          // исходная HDRI (2D)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_hdriSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_hdriCubemap;          // конвертированная в cubemap (512x512)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_hdriCubemapSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_irradianceMap;        // irradiance map (32x32)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_irradianceSRV;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_equirectToCubemapPS; // HDRI → cubemap
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_irradiancePS;        // cubemap → irradiance

    HRESULT LoadHDRI(const wchar_t* filename);
    HRESULT ConvertEquirectToCubemap();
    HRESULT ComputeIrradianceMap();

    // Skybox
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_skyboxVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_skyboxIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_skyboxVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_skyboxPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_skyboxInputLayout;
    UINT m_skyboxIndexCount;

    HRESULT CreateSkyboxResources();
    void DrawSkybox();

    // HDR render target
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_hdrTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_hdrRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_hdrSRV;

    // Downsampling chain (для вычисления средней яркости)
    struct DownsampleLevel {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        UINT width = 0;
        UINT height = 0;
    };
    std::vector<DownsampleLevel> m_downsampleChain;

    // Staging texture для чтения результата 1x1 на CPU
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_luminanceStaging;

    // Fullscreen quad
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_quadVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_quadIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_quadVS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_quadInputLayout;

    // Пост-процесс шейдеры
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_brightnessPS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_copyPS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_tonemapPS;

    // Сэмплер с линейной фильтрацией
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearSampler;

    // Константный буфер для tone mapping (экспозиция)
    struct TonemapConstants {
        float exposure;
        float pad[3];
    };
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_tonemapCB;

    // Вспомогательные функции
    HRESULT CreateHDRTarget(UINT width, UINT height);
    HRESULT CreateDownsampleChain(UINT width, UINT height);
    HRESULT CreateQuadResources();
    HRESULT CreatePostprocessShaders();
    void ComputeAverageLuminance();
    void ApplyTonemap();
    void DrawEnvironmentToCurrentTarget();

    struct QuadVertex
    {
        float x, y, z;  // позиция
        float u, v;     // текстурные координаты
    };

    float m_adaptedLuminance;
    float m_currentExposure;
    float m_eyeAdaptationSpeed;
    std::chrono::steady_clock::time_point m_lastFrameTime;
};
