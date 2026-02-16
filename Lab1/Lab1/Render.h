#pragma once

#include <d3d11_1.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class Render
{
public:
    Render();
    ~Render();

    HRESULT Initialize(HWND hwnd);
    void Shutdown();
    void DrawScene();
    void HandleResize(HWND hwnd);

    // Управление камерой
    void MoveView(float dx, float dy, float dz);
    void RotateView(float yaw, float pitch);

private:
    HRESULT SetupDevice(HWND hwnd);
    HRESULT SetupBackBuffer();
    HRESULT SetupDepthStencil(UINT width, UINT height);
    HRESULT CreateGeometry();
    HRESULT LoadShaders();
    void UpdateTransforms();
    void SetDebugNames();

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
};
