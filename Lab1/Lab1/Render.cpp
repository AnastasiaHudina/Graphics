#include "AppFramework.h"
#include "Render.h"
#include <d3dcompiler.h>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

// Структура вершины
struct Vertex
{
    float pos[3];
    COLORREF color;
};

// Буферы констант
struct WorldMatrixBuffer
{
    XMMATRIX world;
};

struct ViewProjBuffer
{
    XMMATRIX viewProj;
};

Render::Render()
    : m_cameraPos(0.0f, 0.0f, -5.0f)
    , m_yawAngle(0.0f)
    , m_pitchAngle(0.0f)
    , m_rotationAngle(0.0f)
    , m_hwnd(nullptr)
{
}

Render::~Render()
{
    Shutdown();
}

HRESULT Render::Initialize(HWND hwnd)
{
    m_hwnd = hwnd;

    HRESULT hr = SetupDevice(hwnd);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания устройства\n");
        return hr;
    }

    RECT rect;
    GetClientRect(hwnd, &rect);
    UINT width = rect.right - rect.left;
    UINT height = rect.bottom - rect.top;

    hr = SetupDepthStencil(width, height);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания depth buffer\n");
        return hr;
    }

    hr = CreateGeometry();
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания геометрии\n");
        return hr;
    }

    hr = LoadShaders();
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка загрузки шейдеров\n");
        return hr;
    }

    SetDebugNames();

    return S_OK;
}

void Render::Shutdown()
{
    if (m_context)
    {
        m_context->ClearState();
    }
}

HRESULT Render::SetupDevice(HWND hwnd)
{
    // Поиск адаптера
    ComPtr<IDXGIFactory> factory;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), &factory);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания DXGI Factory\n");
        return hr;
    }

    ComPtr<IDXGIAdapter> adapter;
    UINT adapterIdx = 0;
    while (SUCCEEDED(factory->EnumAdapters(adapterIdx, &adapter)))
    {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);

        // Пропускаем программный рендерер
        if (wcscmp(desc.Description, L"Microsoft Basic Render Driver") != 0)
        {
            break;
        }
        adapter.Reset();
        ++adapterIdx;
    }

    // Создание устройства с debug layer
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;

    hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        featureLevels,
        1,
        D3D11_SDK_VERSION,
        &m_device,
        &featureLevel,
        &m_context
    );

    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания D3D11 устройства\n");
        return hr;
    }

    // Настройка swap chain
    DXGI_SWAP_CHAIN_DESC scDesc = {};
    scDesc.BufferCount = 2;
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow = hwnd;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.Windowed = TRUE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = factory->CreateSwapChain(m_device.Get(), &scDesc, &m_swapChain);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания swap chain\n");
        return hr;
    }

    hr = SetupBackBuffer();
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка настройки back buffer\n");
        return hr;
    }

    // Получение интерфейса для меток RenderDoc
    hr = m_context->QueryInterface(__uuidof(ID3DUserDefinedAnnotation), &m_annotation);
    if (FAILED(hr))
    {
        OutputDebugString(L"Предупреждение: ID3DUserDefinedAnnotation недоступен\n");
        m_annotation = nullptr;
    }

    return S_OK;
}

HRESULT Render::SetupBackBuffer()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка получения back buffer\n");
        return hr;
    }

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTarget);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания render target view\n");
        return hr;
    }

    return S_OK;
}

HRESULT Render::SetupDepthStencil(UINT width, UINT height)
{
    // Создание текстуры для depth buffer
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = m_device->CreateTexture2D(&depthDesc, nullptr, &m_depthBuffer);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания depth texture\n");
        return hr;
    }

    // Создание depth stencil view
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = depthDesc.Format;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;

    hr = m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, &m_depthStencil);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания depth stencil view\n");
        return hr;
    }

    m_context->OMSetRenderTargets(1, m_renderTarget.GetAddressOf(), m_depthStencil.Get());

    // Viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    m_context->RSSetViewports(1, &vp);

    return S_OK;
}

HRESULT Render::CreateGeometry()
{
    // Вершины куба
    static const Vertex cubeVertices[] =
    {
        { {-1.0f,  1.0f, -1.0f}, RGB(255, 20, 147) },   
        { { 1.0f,  1.0f, -1.0f}, RGB(0, 255, 127) },    
        { { 1.0f,  1.0f,  1.0f}, RGB(138, 43, 226) },  
        { {-1.0f,  1.0f,  1.0f}, RGB(255, 215, 0) },  
        { {-1.0f, -1.0f, -1.0f}, RGB(255, 69, 0) },     
        { { 1.0f, -1.0f, -1.0f}, RGB(0, 255, 255) }, 
        { { 1.0f, -1.0f,  1.0f}, RGB(186, 85, 211) },   
        { {-1.0f, -1.0f,  1.0f}, RGB(50, 205, 50) }     
    };

    // Индексы треугольников
    WORD cubeIndices[] =
    {
        3,1,0, 2,1,3,
        0,5,4, 1,5,0,
        3,4,7, 0,4,3,
        1,6,5, 2,6,1,
        2,7,6, 3,7,2,
        6,4,5, 7,4,6,
    };

    // Создание вершинного буфера
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(cubeVertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = cubeVertices;

    HRESULT hr = m_device->CreateBuffer(&vbDesc, &vbData, &m_vertexBuffer);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания вершинного буфера\n");
        return hr;
    }

    // Создание индексного буфера
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = sizeof(cubeIndices);
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = cubeIndices;

    hr = m_device->CreateBuffer(&ibDesc, &ibData, &m_indexBuffer);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания индексного буфера\n");
        return hr;
    }

    // Константный буфер для world матрицы
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(WorldMatrixBuffer);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_worldBuffer);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания world buffer\n");
        return hr;
    }

    // Константный буфер для view-projection
    cbDesc.ByteWidth = sizeof(ViewProjBuffer);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_viewProjBuffer);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания view-proj buffer\n");
        return hr;
    }

    return S_OK;
}

HRESULT Render::LoadShaders()
{
    // Компиляция вершинного шейдера
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompileFromFile(
        L"VertexShader.vs",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main",
        "vs_5_0",
        compileFlags,
        0,
        &vsBlob,
        &errorBlob
    );

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        OutputDebugString(L"Ошибка компиляции вершинного шейдера\n");
        return hr;
    }

    hr = m_device->CreateVertexShader(
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        nullptr,
        &m_vertexShader
    );

    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания вершинного шейдера\n");
        return hr;
    }

    // Компиляция пиксельного шейдера
    ComPtr<ID3DBlob> psBlob;
    hr = D3DCompileFromFile(
        L"PixelShader.ps",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main",
        "ps_5_0",
        compileFlags,
        0,
        &psBlob,
        &errorBlob
    );

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        OutputDebugString(L"Ошибка компиляции пиксельного шейдера\n");
        return hr;
    }

    hr = m_device->CreatePixelShader(
        psBlob->GetBufferPointer(),
        psBlob->GetBufferSize(),
        nullptr,
        &m_pixelShader
    );

    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания пиксельного шейдера\n");
        return hr;
    }

    // Input layout
    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = m_device->CreateInputLayout(
        layout,
        2,
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        &m_inputLayout
    );

    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания input layout\n");
        return hr;
    }

    return S_OK;
}

void Render::SetDebugNames()
{
#ifdef _DEBUG
    // Установка имен для RenderDoc
    if (m_device)
        m_device->SetPrivateData(WKPDID_D3DDebugObjectName, 11, "MainDevice");
    if (m_context)
        m_context->SetPrivateData(WKPDID_D3DDebugObjectName, 14, "MainContext");
    if (m_swapChain)
        m_swapChain->SetPrivateData(WKPDID_D3DDebugObjectName, 13, "MainSwapChain");
    if (m_renderTarget)
        m_renderTarget->SetPrivateData(WKPDID_D3DDebugObjectName, 16, "RenderTargetView");
    if (m_depthStencil)
        m_depthStencil->SetPrivateData(WKPDID_D3DDebugObjectName, 17, "DepthStencilView");
    if (m_vertexBuffer)
        m_vertexBuffer->SetPrivateData(WKPDID_D3DDebugObjectName, 12, "VertexBuffer");
    if (m_indexBuffer)
        m_indexBuffer->SetPrivateData(WKPDID_D3DDebugObjectName, 11, "IndexBuffer");
    if (m_worldBuffer)
        m_worldBuffer->SetPrivateData(WKPDID_D3DDebugObjectName, 11, "WorldBuffer");
    if (m_viewProjBuffer)
        m_viewProjBuffer->SetPrivateData(WKPDID_D3DDebugObjectName, 14, "ViewProjBuffer");
    if (m_vertexShader)
        m_vertexShader->SetPrivateData(WKPDID_D3DDebugObjectName, 12, "VertexShader");
    if (m_pixelShader)
        m_pixelShader->SetPrivateData(WKPDID_D3DDebugObjectName, 11, "PixelShader");
#endif
}

void Render::DrawScene()
{
    // Маркер для RenderDoc
    if (m_annotation)
        m_annotation->BeginEvent(L"DrawScene");

    // Очистка буферов (темно-фиолетовый космический фон)
    float clearColor[4] = { 0.1f, 0.05f, 0.2f, 1.0f };
    m_context->ClearRenderTargetView(m_renderTarget.Get(), clearColor);
    m_context->ClearDepthStencilView(m_depthStencil.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    UpdateTransforms();

    // Настройка pipeline
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(m_inputLayout.Get());

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

    // Рендеринг
    if (m_annotation)
        m_annotation->BeginEvent(L"DrawCube");
    m_context->DrawIndexed(36, 0, 0);
    if (m_annotation)
        m_annotation->EndEvent();

    if (m_annotation)
        m_annotation->EndEvent();

    m_swapChain->Present(1, 0);
}

void Render::UpdateTransforms()
{
    // Обновление угла вращения куба
    m_rotationAngle += 0.005f;
    if (m_rotationAngle > XM_2PI)
        m_rotationAngle -= XM_2PI;

    XMMATRIX world = XMMatrixRotationY(m_rotationAngle);

    // Обновление матрицы мира
    XMMATRIX worldTranspose = XMMatrixTranspose(world);
    m_context->UpdateSubresource(m_worldBuffer.Get(), 0, nullptr, &worldTranspose, 0, 0);

    // Вычисление направления взгляда
    XMVECTOR direction = XMVectorSet(
        cosf(m_pitchAngle) * sinf(m_yawAngle),
        sinf(m_pitchAngle),
        cosf(m_pitchAngle) * cosf(m_yawAngle),
        0.0f
    );

    XMVECTOR eye = XMLoadFloat3(&m_cameraPos);
    XMVECTOR focus = XMVectorAdd(eye, direction);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(eye, focus, up);

    // Перспективная проекция
    RECT rect;
    GetClientRect(m_hwnd, &rect);
    float aspect = static_cast<float>(rect.right - rect.left) / static_cast<float>(rect.bottom - rect.top);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);

    XMMATRIX viewProj = view * proj;
    XMMATRIX vpTranspose = XMMatrixTranspose(viewProj);

    // Обновление view-projection буфера
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_viewProjBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        memcpy(mapped.pData, &vpTranspose, sizeof(XMMATRIX));
        m_context->Unmap(m_viewProjBuffer.Get(), 0);
    }

    m_context->VSSetConstantBuffers(0, 1, m_worldBuffer.GetAddressOf());
    m_context->VSSetConstantBuffers(1, 1, m_viewProjBuffer.GetAddressOf());
}

void Render::HandleResize(HWND hwnd)
{
    if (!m_swapChain)
        return;

    // Освобождение старых ресурсов
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_renderTarget.Reset();
    m_depthStencil.Reset();
    m_depthBuffer.Reset();

    // Изменение размера swap chain
    RECT rect;
    GetClientRect(hwnd, &rect);
    UINT width = rect.right - rect.left;
    UINT height = rect.bottom - rect.top;

    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка изменения размера swap chain\n");
        return;
    }

    // Пересоздание back buffer
    hr = SetupBackBuffer();
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка пересоздания back buffer\n");
        return;
    }

    // Пересоздание depth buffer
    hr = SetupDepthStencil(width, height);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка пересоздания depth buffer\n");
        return;
    }
}

void Render::MoveView(float dx, float dy, float dz)
{
    m_cameraPos.x += dx;
    m_cameraPos.y += dy;
    m_cameraPos.z += dz;
}

void Render::RotateView(float yaw, float pitch)
{
    m_yawAngle += yaw;
    m_pitchAngle += pitch;

    // Ограничение углов
    if (m_yawAngle > XM_2PI)
        m_yawAngle -= XM_2PI;
    if (m_yawAngle < -XM_2PI)
        m_yawAngle += XM_2PI;

    if (m_pitchAngle > XM_PIDIV2)
        m_pitchAngle = XM_PIDIV2;
    if (m_pitchAngle < -XM_PIDIV2)
        m_pitchAngle = -XM_PIDIV2;
}
