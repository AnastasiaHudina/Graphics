#pragma once

#include <d3d11_1.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <vector>

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
};
