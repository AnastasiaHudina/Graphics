#include "AppFramework.h"
#include "Render.h"
#include <d3dcompiler.h>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

// Структура вершины с позицией, нормалью, цветом
struct Vertex
{
    float pos[3];
    float normal[3];
    float color[4];
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

struct LightBufferData
{
    DirectX::XMINT4  lightCount;   // x = количество источников
    DirectX::XMFLOAT4 lightPos[10];
    DirectX::XMFLOAT4 lightColor[10];
    DirectX::XMFLOAT4 ambient;
};

Render::Render()
    : m_cameraPos(0.0f, 0.0f, -7.0f)
    , m_yawAngle(0.0f)
    , m_pitchAngle(0.0f)
    , m_rotationAngle(0.0f)
    , m_autoRotate(true)
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

    // Инициализация ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd))
    {
        OutputDebugString(L"Ошибка инициализации ImGui Win32\n");
        ImGui::DestroyContext();
        return E_FAIL;
    }
    if (!ImGui_ImplDX11_Init(m_device.Get(), m_context.Get()))
    {
        OutputDebugString(L"Ошибка инициализации ImGui DX11\n");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return E_FAIL;
    }
    m_imguiInitialized = true;

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

    m_lights.resize(3);
    // Позиции
    m_lights[0].position = XMFLOAT4(0.0f, 1.5f, 0.8f, 1.0f);
    m_lights[0].color = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f); // красный
    m_lights[1].position = XMFLOAT4(0.8f, 1.5f, 0.0f, 1.0f);
    m_lights[1].color = XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f); // зелёный
    m_lights[2].position = XMFLOAT4(0.0f, 1.5f, -0.8f, 1.0f);
    m_lights[2].color = XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f); // синий

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
    if (m_imguiInitialized)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_imguiInitialized = false;
    }
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
    // Нижняя грань (y = -1)
    Vertex vertices[24] =
    {
        // Нижняя грань (y = -1)
        { {-1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { {-1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },

        // Верхняя грань (y = 1)
        { {-1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { {-1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },

        // Правая грань (x = 1)
        { { 1.0f, -1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f, -1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f,  1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f,  1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },

        // Левая грань (x = -1)
        { {-1.0f, -1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { {-1.0f, -1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { {-1.0f,  1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { {-1.0f,  1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },

        // Передняя грань (z = 1)
        { { 1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { {-1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { {-1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },

        // Задняя грань (z = -1)
        { {-1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { { 1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f} },
        { {-1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f} }
    };

    WORD indices[36] =
    {
        0,2,1, 0,3,2,          // bottom
        4,6,5, 4,7,6,          // top
        8,10,9, 8,11,10,       // right
        12,14,13, 12,15,14,    // left
        16,18,17, 16,19,18,    // front
        20,22,21, 20,23,22     // back
    };

    // Создание вершинного буфера
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertices;

    HRESULT hr = m_device->CreateBuffer(&vbDesc, &vbData, &m_vertexBuffer);
    if (FAILED(hr)) return hr;

    // Индексный буфер
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = sizeof(indices);
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = indices;

    hr = m_device->CreateBuffer(&ibDesc, &ibData, &m_indexBuffer);
    if (FAILED(hr)) return hr;

    // Константный буфер для world матрицы 
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(WorldMatrixBuffer);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_worldBuffer);
    if (FAILED(hr)) return hr;

    // Константный буфер для view-proj 
    cbDesc.ByteWidth = sizeof(ViewProjBuffer);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_viewProjBuffer);
    if (FAILED(hr)) return hr;

    // Константный буфер для источников света
    cbDesc.ByteWidth = sizeof(LightBufferData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_lightBuffer);
    if (FAILED(hr)) return hr;

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
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    UINT numElements = ARRAYSIZE(layout);

    hr = m_device->CreateInputLayout(
        layout,
        3,
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
    if (m_annotation) m_annotation->BeginEvent(L"DrawScene");

    float clearColor[4] = { 0.1f, 0.05f, 0.2f, 1.0f };
    m_context->ClearRenderTargetView(m_renderTarget.Get(), clearColor);
    m_context->ClearDepthStencilView(m_depthStencil.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    UpdateTransforms();

    // Установка константных буферов
    ID3D11Buffer* vsConstantBuffers[] = { m_worldBuffer.Get(), m_viewProjBuffer.Get(), m_lightBuffer.Get() };
    m_context->VSSetConstantBuffers(0, 3, vsConstantBuffers);
    m_context->PSSetConstantBuffers(2, 1, m_lightBuffer.GetAddressOf());

    // ===== БЛОК ImGui =====
    if (m_imguiInitialized)
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Light Intensity");
        for (int i = 0; i < 3; ++i)
        {
            ImGui::Text("Light %d", i);
            ImGui::SameLine();
            if (ImGui::Button(("1##l" + std::to_string(i)).c_str())) { m_lights[i].color.w = 1.0f; }
            ImGui::SameLine();
            if (ImGui::Button(("10##l" + std::to_string(i)).c_str())) { m_lights[i].color.w = 10.0f; }
            ImGui::SameLine();
            if (ImGui::Button(("100##l" + std::to_string(i)).c_str())) { m_lights[i].color.w = 100.0f; }
            ImGui::Text("Intensity: %.0f", m_lights[i].color.w);
        }
        ImGui::End();

        ImGui::Render();
    }
    // ===== КОНЕЦ БЛОКА ImGui =====

    

    // Настройка pipeline
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(m_inputLayout.Get());

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

    if (m_annotation) m_annotation->BeginEvent(L"DrawCube");
    m_context->DrawIndexed(36, 0, 0);
    if (m_annotation) m_annotation->EndEvent();

    if (m_annotation) m_annotation->EndEvent();

    if (m_imguiInitialized)
    {
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    m_swapChain->Present(1, 0);
}

void Render::ToggleAutoRotate()
{
    m_autoRotate = !m_autoRotate;
}

void Render::UpdateTransforms()
{
    // Обновление угла вращения куба если autoRotate включён
    if (m_autoRotate)
    {
        m_rotationAngle += 0.005f;
        if (m_rotationAngle > XM_2PI)
            m_rotationAngle -= XM_2PI;
    }

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

    // Обновление буфера источников света
    LightBufferData lightData;
    lightData.lightCount = XMINT4((int)m_lights.size(), 0, 0, 0);
    for (size_t i = 0; i < m_lights.size(); ++i)
    {
        lightData.lightPos[i] = m_lights[i].position;
        lightData.lightColor[i] = m_lights[i].color;
    }
    lightData.ambient = XMFLOAT4(0.1f, 0.1f, 0.1f, 1.0f);

    if (SUCCEEDED(m_context->Map(m_lightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &lightData, sizeof(lightData));
        m_context->Unmap(m_lightBuffer.Get(), 0);
    }
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

void Render::MoveForward(float distance)
{
    // Вычисляем вектор направления взгляда на основе текущих углов
    XMVECTOR dir = XMVectorSet(
        cosf(m_pitchAngle) * sinf(m_yawAngle),
        sinf(m_pitchAngle),
        cosf(m_pitchAngle) * cosf(m_yawAngle),
        0.0f
    );

    XMVECTOR pos = XMLoadFloat3(&m_cameraPos);
    pos = XMVectorAdd(pos, dir * distance);
    XMStoreFloat3(&m_cameraPos, pos);
}

void Render::RotateAroundTarget(float dx, float dy)
{
    // Центр сцены (кубик)
    XMVECTOR target = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMVECTOR pos = XMLoadFloat3(&m_cameraPos);

    // Вектор от центра к камере
    XMVECTOR toCamera = pos - target;
    float radius = XMVectorGetX(XMVector3Length(toCamera));
    if (radius < 0.001f) radius = 0.001f;

    // Направление на камеру
    XMVECTOR dir = toCamera / radius;

    // Извлекаем сферические углы
    float yComp = XMVectorGetY(dir);
    if (yComp > 1.0f) yComp = 1.0f;
    if (yComp < -1.0f) yComp = -1.0f;
    float theta = asin(yComp);                // вертикальный угол
    float phi = atan2(XMVectorGetZ(dir), XMVectorGetX(dir)); // горизонтальный

    // Применяем приращения от мыши
    phi += dx;
    theta += dy;

    // Ограничиваем вертикальный угол, чтобы камера не переворачивалась
    const float maxTheta = XM_PIDIV2 - 0.001f;
    if (theta > maxTheta) theta = maxTheta;
    if (theta < -maxTheta) theta = -maxTheta;

    // Вычисляем новую позицию на сфере
    float cosTheta = cosf(theta);
    XMVECTOR newPos = target + radius * XMVectorSet(
        cosTheta * cosf(phi),
        sinf(theta),
        cosTheta * sinf(phi),
        0.0f
    );
    XMStoreFloat3(&m_cameraPos, newPos);
}

void Render::Zoom(float delta)
{
    XMVECTOR target = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMVECTOR pos = XMLoadFloat3(&m_cameraPos);
    XMVECTOR toCamera = pos - target;
    float radius = XMVectorGetX(XMVector3Length(toCamera));

    radius += delta;               // положительная delta – удаление
    if (radius < 0.5f) radius = 0.5f; // минимальное расстояние

    XMVECTOR dir = toCamera / XMVector3Length(toCamera);
    XMVECTOR newPos = target + radius * dir;
    XMStoreFloat3(&m_cameraPos, newPos);

}

