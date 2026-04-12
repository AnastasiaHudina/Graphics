#include "AppFramework.h"
#include "Render.h"
#include <d3dcompiler.h>
#include <string>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

// Параметры prefiltered cubemap
static const UINT PREFILTERED_SIZE = 128;          // размер грани
static const UINT PREFILTERED_MIP_LEVELS = 5;      // количество mip-уровней (roughness 0..1)
static const UINT PREFILTERED_SAMPLE_COUNT = 1024; // число сэмплов для importance sampling

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
    XMMATRIX invViewProj;
    XMFLOAT4 cameraPosAndMode; // xyz = camera pos, w = view mode
};

struct LightBufferData
{
    DirectX::XMINT4  lightCount;   // x = количество источников
    DirectX::XMFLOAT4 lightPos[10];
    DirectX::XMFLOAT4 lightColor[10];
    DirectX::XMFLOAT4 ambient;
};

struct MaterialBufferData
{
    DirectX::XMFLOAT4 baseColor;     // rgb
    DirectX::XMFLOAT4 roughMetalPad; // x=roughness, y=metalness
};

Render::Render()
    : m_cameraPos(0.0f, 0.0f, -7.0f)
    , m_yawAngle(0.0f)
    , m_pitchAngle(0.0f)
    , m_rotationAngle(0.0f)
    , m_autoRotate(true)
    , m_hwnd(nullptr)
    , m_adaptedLuminance(0.0f)               // начальное значение
    , m_eyeAdaptationSpeed(0.3f)              // скорость адаптации (можно регулировать)
    , m_lastFrameTime(std::chrono::steady_clock::now())
    , m_currentExposure(1.0f)
    , m_prefilteredSize(PREFILTERED_SIZE)
    , m_prefilteredMipLevels(PREFILTERED_MIP_LEVELS)
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

    hr = CreateQuadResources();
    if (FAILED(hr)) return hr;
    hr = CreatePostprocessShaders();
    if (FAILED(hr)) return hr;

    hr = CreateHDRTarget(width, height);
    if (FAILED(hr)) return hr;
    hr = CreateDownsampleChain(width, height);
    if (FAILED(hr)) return hr;

    hr = CreateEnvironmentResources();
    if (FAILED(hr)) return hr;

    // Загрузка и обработка HDRI для IBL
    hr = LoadHDRI(L"pathway_morning_4k.hdr");
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка загрузки HDRI, IBL будет отключен\n");
    }
    else
    {
        hr = ConvertEquirectToCubemap();
        if (FAILED(hr))
        {
            OutputDebugString(L"Ошибка конвертации HDRI в cubemap\n");
        }
        else
        {
            hr = ComputeIrradianceMap();
            if (FAILED(hr))
            {
                OutputDebugString(L"Ошибка вычисления irradiance map\n");
            }

            // ---- Specular IBL (prefiltered map) ----
            hr = CreatePrefilteredMap();
            if (FAILED(hr))
            {
                wchar_t buf[256];
                swprintf_s(buf, L"CreatePrefilteredMap failed (0x%08X), specular IBL disabled\n", hr);
                OutputDebugString(buf);
            }
            else
            {
                OutputDebugString(L"CreatePrefilteredMap succeeded\n");
            }

            // ---- BRDF LUT (необходим для specular IBL) ----
            hr = CreateBRDFLUT();
            if (FAILED(hr))
            {
                OutputDebugString(L"Warning: CreateBRDFLUT failed, specular IBL will be incomplete\n");
            }
            else
            {
                OutputDebugString(L"CreateBRDFLUT succeeded\n");
            }
        }
    }

    // Создание skybox
    hr = CreateSkyboxResources();
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания skybox, будет использован fallback\n");
    }

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

    // Освобождение HDR-ресурсов
    m_hdrTexture.Reset();
    m_hdrRTV.Reset();
    m_hdrSRV.Reset();

    // Очистка цепочки downsampling
    m_downsampleChain.clear(); 

    // Освобождение staging-текстуры
    m_luminanceStaging.Reset();

    // Освобождение ресурсов quad
    m_quadVertexBuffer.Reset();
    m_quadIndexBuffer.Reset();
    m_quadVS.Reset();
    m_quadInputLayout.Reset();

    // Освобождение пост-процесс шейдеров и сэмплера
    m_brightnessPS.Reset();
    m_copyPS.Reset();
    m_tonemapPS.Reset();
    m_linearSampler.Reset();
    m_tonemapCB.Reset();

    // Environment
    m_envSRV.Reset();
    m_envCubemap.Reset();
    m_environmentPS.Reset();

    // IBL ресурсы
    m_hdriTexture.Reset();
    m_hdriSRV.Reset();
    m_hdriCubemap.Reset();
    m_hdriCubemapSRV.Reset();
    m_irradianceMap.Reset();
    m_irradianceSRV.Reset();
    m_equirectToCubemapPS.Reset();
    m_irradiancePS.Reset();
    m_prefilteredSRV.Reset();
    m_prefilteredCubemap.Reset();
    m_prefilterPS.Reset();

    m_brdfSRV.Reset();
    m_brdfLUT.Reset();
    m_brdfPS.Reset();
    m_brdfSampler.Reset();

    // Skybox
    m_skyboxVertexBuffer.Reset();
    m_skyboxIndexBuffer.Reset();
    m_skyboxVS.Reset();
    m_skyboxPS.Reset();
    m_skyboxInputLayout.Reset();

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
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
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
    // UV sphere
    const int slices = 64;
    const int stacks = 32;
    const float radius = 1.0f;

    std::vector<Vertex> vertices;
    std::vector<WORD> indices;
    vertices.reserve((stacks + 1) * (slices + 1));
    indices.reserve(stacks * slices * 6);

    for (int stack = 0; stack <= stacks; ++stack)
    {
        float v = (float)stack / (float)stacks;       // 0..1
        float phi = v * XM_PI;                        // 0..pi
        float y = cosf(phi);
        float r = sinf(phi);

        for (int slice = 0; slice <= slices; ++slice)
        {
            float u = (float)slice / (float)slices;   // 0..1
            float theta = u * XM_2PI;                 // 0..2pi

            float x = r * cosf(theta);
            float z = r * sinf(theta);

            Vertex vert{};
            vert.pos[0] = radius * x;
            vert.pos[1] = radius * y;
            vert.pos[2] = radius * z;

            // Normal is position normalized (unit sphere)
            vert.normal[0] = x;
            vert.normal[1] = y;
            vert.normal[2] = z;

            vert.color[0] = 1.0f;
            vert.color[1] = 1.0f;
            vert.color[2] = 1.0f;
            vert.color[3] = 1.0f;

            vertices.push_back(vert);
        }
    }

    auto idx = [slices](int stack, int slice) -> WORD {
        return (WORD)(stack * (slices + 1) + slice);
    };

    for (int stack = 0; stack < stacks; ++stack)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            WORD i0 = idx(stack, slice);
            WORD i1 = idx(stack + 1, slice);
            WORD i2 = idx(stack + 1, slice + 1);
            WORD i3 = idx(stack, slice + 1);

            // Two triangles
            indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i3);
        }
    }

    // Создание вершинного буфера
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = (UINT)(sizeof(Vertex) * vertices.size());
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertices.data();

    HRESULT hr = m_device->CreateBuffer(&vbDesc, &vbData, &m_vertexBuffer);
    if (FAILED(hr)) return hr;

    // Индексный буфер
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = (UINT)(sizeof(WORD) * indices.size());
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = indices.data();

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

    // Material buffer
    cbDesc.ByteWidth = sizeof(MaterialBufferData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_materialBuffer);
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

    const bool debugNoTonemap = (m_viewMode != 0);
    if (debugNoTonemap)
    {
        // Direct draw to back buffer (no tonemapping path)
        m_context->ClearRenderTargetView(m_renderTarget.Get(), clearColor);
        m_context->ClearDepthStencilView(m_depthStencil.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        m_context->OMSetRenderTargets(1, m_renderTarget.GetAddressOf(), m_depthStencil.Get());
    }
    else
    {
        // HDR path
        m_context->ClearRenderTargetView(m_hdrRTV.Get(), clearColor);
        m_context->ClearDepthStencilView(m_depthStencil.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        ID3D11RenderTargetView* rtvs[] = { m_hdrRTV.Get() };
        m_context->OMSetRenderTargets(1, rtvs, m_depthStencil.Get());
    }


    RECT rect;
    GetClientRect(m_hwnd, &rect);
    D3D11_VIEWPORT vp = { 0, 0, (float)(rect.right - rect.left), (float)(rect.bottom - rect.top), 0, 1 };
    m_context->RSSetViewports(1, &vp);

    // Обновление трансформаций и установка константных буферов
    UpdateTransforms();
    ID3D11Buffer* vsCB[] = { m_worldBuffer.Get(), m_viewProjBuffer.Get() };
    m_context->VSSetConstantBuffers(0, 2, vsCB);
    m_context->PSSetConstantBuffers(1, 1, m_viewProjBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(2, 1, m_lightBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(3, 1, m_materialBuffer.GetAddressOf());

    DrawSkybox();

    // Настройка pipeline для куба
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

    // Установка IBL ресурсов (irradiance map)
    if (m_irradianceSRV)
    {
        m_context->PSSetShaderResources(1, 1, m_irradianceSRV.GetAddressOf());
        m_context->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());
    }

    if (m_annotation) m_annotation->BeginEvent(L"DrawCube");
    // Sphere indices count = buffer size / sizeof(WORD) computed from CreateGeometry
    D3D11_BUFFER_DESC ibd{};
    m_indexBuffer->GetDesc(&ibd);
    UINT indexCount = ibd.ByteWidth / sizeof(WORD);
    m_context->DrawIndexed(indexCount, 0, 0);
    if (m_annotation) m_annotation->EndEvent();

    if (!debugNoTonemap)
    {
        // Пост-обработка: вычисление яркости и tone mapping
        ComputeAverageLuminance();

        ApplyTonemap();
    }

    if (m_annotation) m_annotation->EndEvent(); // End DrawScene

    // Отрисовка ImGui
    if (m_imguiInitialized)
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("PBR / Debug");
        ImGui::Text("View mode (no tonemapping for NDF/G/F):");
        ImGui::RadioButton("PBR shaded##vm0", &m_viewMode, 0); ImGui::SameLine();
        ImGui::RadioButton("NDF##vm1", &m_viewMode, 1); ImGui::SameLine();
        ImGui::RadioButton("Geometry##vm2", &m_viewMode, 2); ImGui::SameLine();
        ImGui::RadioButton("Fresnel##vm3", &m_viewMode, 3);
        ImGui::Separator();

        ImGui::ColorEdit3("Base color", (float*)&m_baseColor);
        ImGui::SliderFloat("Roughness", &m_roughness, 0.0f, 1.0f);
        ImGui::SliderFloat("Metalness", &m_metalness, 0.0f, 1.0f);
        ImGui::End();

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

        ImGui::Separator();
        ImGui::Text("Adapted Luminance: %.3f", m_adaptedLuminance);
        ImGui::Text("Exposure: %.3f", m_currentExposure);
        ImGui::End();

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    m_swapChain->Present(1, 0);
}

void Render::ToggleAutoRotate()
{
    m_autoRotate = !m_autoRotate;
}

void Render::SetViewMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    m_viewMode = mode;
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
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    // Обновление view-projection буфера
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_viewProjBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        ViewProjBuffer data{};
        data.viewProj = XMMatrixTranspose(viewProj);
        data.invViewProj = XMMatrixTranspose(invViewProj);
        data.cameraPosAndMode = XMFLOAT4(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, (float)m_viewMode);
        memcpy(mapped.pData, &data, sizeof(data));
        m_context->Unmap(m_viewProjBuffer.Get(), 0);
    }

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

    // Material params
    float r = m_roughness;
    float m = m_metalness;
    if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    if (m < 0.0f) m = 0.0f; if (m > 1.0f) m = 1.0f;
    MaterialBufferData mat{};
    mat.baseColor = XMFLOAT4(m_baseColor.x, m_baseColor.y, m_baseColor.z, 1.0f);
    mat.roughMetalPad = XMFLOAT4(r, m, 0.0f, 0.0f);
    if (SUCCEEDED(m_context->Map(m_materialBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &mat, sizeof(mat));
        m_context->Unmap(m_materialBuffer.Get(), 0);
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

    m_hdrTexture.Reset();
    m_hdrRTV.Reset();
    m_hdrSRV.Reset();
    m_downsampleChain.clear();
    m_luminanceStaging.Reset();

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

    hr = CreateHDRTarget(width, height);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка пересоздания HDR Target\n");
        return;
    }

    hr = CreateDownsampleChain(width, height);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка пересоздания Downsample Chain\n");
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
    float theta = asinf(yComp);                // вертикальный угол
    float phi = atan2f(XMVectorGetZ(dir), XMVectorGetX(dir)); // горизонтальный

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

HRESULT Render::CreateHDRTarget(UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // или R32G32B32A32_FLOAT
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_hdrTexture);
    if (FAILED(hr)) return hr;

    hr = m_device->CreateRenderTargetView(m_hdrTexture.Get(), nullptr, &m_hdrRTV);
    if (FAILED(hr)) return hr;

    hr = m_device->CreateShaderResourceView(m_hdrTexture.Get(), nullptr, &m_hdrSRV);
    if (FAILED(hr)) return hr;

    return S_OK;
}

HRESULT Render::CreateDownsampleChain(UINT width, UINT height)
{
    m_downsampleChain.clear();

    UINT size = min(width, height);

    // Квадратные текстуры от min(w,h) до 1x1, формат RGBA32F (как в референсе)
    while (size >= 1)
    {
        DownsampleLevel lvl;
        lvl.width = size;
        lvl.height = size;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = size;
        desc.Height = size;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &lvl.texture);
        if (FAILED(hr)) return hr;
        hr = m_device->CreateRenderTargetView(lvl.texture.Get(), nullptr, &lvl.rtv);
        if (FAILED(hr)) return hr;
        hr = m_device->CreateShaderResourceView(lvl.texture.Get(), nullptr, &lvl.srv);
        if (FAILED(hr)) return hr;

        m_downsampleChain.push_back(std::move(lvl));

        if (size == 1) break;
        size /= 2;
        if (size < 1) size = 1;
    }

    // Staging текстура для чтения последнего уровня (1x1)
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = 1;
    stagingDesc.Height = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &m_luminanceStaging);
    if (FAILED(hr)) return hr;

    return S_OK;
}

HRESULT Render::CreateQuadResources()
{
    // Компиляция вершинного шейдера для quad
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(L"QuadVS.vs", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "vs_5_0", flags, 0, &vsBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        return hr;
    }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_quadVS);
    if (FAILED(hr)) return hr;

    // Input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    hr = m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_quadInputLayout);
    if (FAILED(hr)) return hr;

    // Вершинный буфер (порядок для TRIANGLE_STRIP, как в референсе)
    QuadVertex vertices[4] = {
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f }
    };
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = { vertices };
    hr = m_device->CreateBuffer(&vbDesc, &vbData, &m_quadVertexBuffer);
    if (FAILED(hr)) return hr;

    return S_OK;
}

HRESULT Render::CreatePostprocessShaders()
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> psBlob;

    // Brightness shader
    HRESULT hr = D3DCompileFromFile(L"Brightness.ps", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "ps_5_0", flags, 0, &psBlob, nullptr);
    if (FAILED(hr)) return hr;
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_brightnessPS);
    if (FAILED(hr)) return hr;

    // Copy shader
    hr = D3DCompileFromFile(L"Copy.ps", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "ps_5_0", flags, 0, &psBlob, nullptr);
    if (FAILED(hr)) return hr;
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_copyPS);
    if (FAILED(hr)) return hr;

    // Tonemap shader
    hr = D3DCompileFromFile(L"Tonemap.ps", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "ps_5_0", flags, 0, &psBlob, nullptr);
    if (FAILED(hr)) return hr;
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_tonemapPS);
    if (FAILED(hr)) return hr;

    // Линейный сэмплер
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_device->CreateSamplerState(&sampDesc, &m_linearSampler);
    if (FAILED(hr)) return hr;

    // Константный буфер для экспозиции
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(TonemapConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_tonemapCB);
    if (FAILED(hr)) return hr;

    return S_OK;
}

void Render::ComputeAverageLuminance()
{
    // Сохраняем текущее состояние
    ComPtr<ID3D11RenderTargetView> oldRTV;
    ComPtr<ID3D11DepthStencilView> oldDSV;
    m_context->OMGetRenderTargets(1, &oldRTV, &oldDSV);

    ComPtr<ID3D11VertexShader> oldVS;
    ComPtr<ID3D11PixelShader> oldPS;
    ComPtr<ID3D11InputLayout> oldLayout;
    m_context->VSGetShader(&oldVS, nullptr, nullptr);
    m_context->PSGetShader(&oldPS, nullptr, nullptr);
    m_context->IAGetInputLayout(&oldLayout);

    D3D11_VIEWPORT oldVP = {};
    UINT numVP = 1;
    m_context->RSGetViewports(&numVP, &oldVP);

    // Явное отключение depth testing (без этого quad может быть отсечён)
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.StencilEnable = FALSE;
    ComPtr<ID3D11DepthStencilState> noDepthState;
    m_device->CreateDepthStencilState(&dsDesc, &noDepthState);
    m_context->OMSetDepthStencilState(noDepthState.Get(), 0);

    // Настройка fullscreen quad (TRIANGLE_STRIP, как в референсе)
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_quadVertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetInputLayout(m_quadInputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->VSSetShader(m_quadVS.Get(), nullptr, 0);

    D3D11_VIEWPORT vp = {};
    vp.MinDepth = 0;
    vp.MaxDepth = 1;

    // Шаг 1: преобразование HDR -> яркость (log)
    {
        auto& target = m_downsampleChain[0];
        vp.Width = (float)target.width;
        vp.Height = (float)target.height;
        m_context->RSSetViewports(1, &vp);

        m_context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);

        m_context->PSSetShader(m_brightnessPS.Get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, m_hdrSRV.GetAddressOf());
        m_context->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());

        m_context->Draw(4, 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Шаг 2: последовательное уменьшение (downsample chain)
    m_context->PSSetShader(m_copyPS.Get(), nullptr, 0);
    for (size_t i = 1; i < m_downsampleChain.size(); ++i)
    {
        auto& src = m_downsampleChain[i - 1];
        auto& dst = m_downsampleChain[i];
        vp.Width = (float)dst.width;
        vp.Height = (float)dst.height;
        m_context->RSSetViewports(1, &vp);
        m_context->OMSetRenderTargets(1, dst.rtv.GetAddressOf(), nullptr);

        m_context->PSSetShaderResources(0, 1, src.srv.GetAddressOf());

        m_context->Draw(4, 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Копирование последнего уровня в staging текстуру
    auto& last = m_downsampleChain.back();
    m_context->CopyResource(m_luminanceStaging.Get(), last.texture.Get());

    // Чтение данных на CPU
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_luminanceStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        float logLum = ((float*)mapped.pData)[0];
        m_context->Unmap(m_luminanceStaging.Get(), 0);

        float avgLum = expf(logLum) - 1.0f;
        if (avgLum < 0.001f) avgLum = 0.001f;

        auto now = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(now - m_lastFrameTime).count();
        m_lastFrameTime = now;
        if (deltaTime > 0.1f) deltaTime = 0.1f;

        float expGain = 1.0f - expf(-deltaTime / m_eyeAdaptationSpeed);
        m_adaptedLuminance += (avgLum - m_adaptedLuminance) * expGain;
        if (m_adaptedLuminance < 0.001f) m_adaptedLuminance = 0.001f;

        // Обновляем debug-значение экспозиции (как в шейдере: exp(adapted) - 1)
        float shaderLum = expf(m_adaptedLuminance) - 1.0f;
        if (shaderLum < 0.001f) shaderLum = 0.001f;
        float keyValue = 1.03f - 2.0f / (2.0f + log10f(shaderLum + 1.0f));
        m_currentExposure = keyValue / shaderLum;
    }

    // Восстановление старого состояния
    m_context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
    m_context->OMSetDepthStencilState(nullptr, 0);
    m_context->VSSetShader(oldVS.Get(), nullptr, 0);
    m_context->PSSetShader(oldPS.Get(), nullptr, 0);
    m_context->IASetInputLayout(oldLayout.Get());
    m_context->RSSetViewports(1, &oldVP);
}

void Render::ApplyTonemap()
{
    if (!m_hdrSRV || !m_tonemapPS || !m_tonemapCB) return;

    m_context->OMSetRenderTargets(1, m_renderTarget.GetAddressOf(), nullptr);

    RECT rect;
    GetClientRect(m_hwnd, &rect);
    D3D11_VIEWPORT vp = {};
    vp.Width = float(rect.right - rect.left);
    vp.Height = float(rect.bottom - rect.top);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_quadVertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->IASetInputLayout(m_quadInputLayout.Get());
    m_context->VSSetShader(m_quadVS.Get(), nullptr, 0);
    m_context->PSSetShader(m_tonemapPS.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, m_hdrSRV.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());

    // Передаём adapted luminance в шейдер (как в референсе)
    D3D11_MAPPED_SUBRESOURCE cbMap = {};
    if (SUCCEEDED(m_context->Map(m_tonemapCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cbMap)))
    {
        TonemapConstants* cb = (TonemapConstants*)cbMap.pData;
        cb->Params = XMFLOAT4(m_adaptedLuminance, 0, 0, 0);
        m_context->Unmap(m_tonemapCB.Get(), 0);
    }
    m_context->PSSetConstantBuffers(0, 1, m_tonemapCB.GetAddressOf());

    m_context->Draw(4, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);
}

HRESULT Render::CreateEnvironmentResources()
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 16;
    desc.Height = 16;
    desc.MipLevels = 1;
    desc.ArraySize = 6;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    std::vector<std::vector<uint32_t>> faceData(6);
    for (int f = 0; f < 6; ++f) faceData[f].resize(desc.Width * desc.Height);

    auto pack = [](uint8_t r, uint8_t g, uint8_t b, uint8_t a) -> uint32_t {
        return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
    };

    const uint32_t faceColors[6] = {
        pack(255, 120, 120, 255), // +X
        pack(120, 255, 120, 255), // -X
        pack(120, 120, 255, 255), // +Y
        pack(255, 255, 120, 255), // -Y
        pack(120, 255, 255, 255), // +Z
        pack(255, 120, 255, 255)  // -Z
    };

    for (int f = 0; f < 6; ++f)
    {
        for (UINT y = 0; y < desc.Height; ++y)
        {
            for (UINT x = 0; x < desc.Width; ++x)
            {
                float fx = (float)x / (float)(desc.Width - 1);
                float fy = (float)y / (float)(desc.Height - 1);
                uint8_t shade = (uint8_t)(64 + 191 * (0.5f * fx + 0.5f * (1.0f - fy)));
                uint32_t base = faceColors[f];
                uint8_t br = (uint8_t)(base & 0xFF);
                uint8_t bg = (uint8_t)((base >> 8) & 0xFF);
                uint8_t bb = (uint8_t)((base >> 16) & 0xFF);
                faceData[f][y * desc.Width + x] = pack(
                    (uint8_t)((br * shade) / 255),
                    (uint8_t)((bg * shade) / 255),
                    (uint8_t)((bb * shade) / 255),
                    255
                );
            }
        }
    }

    std::vector<D3D11_SUBRESOURCE_DATA> sub(6);
    for (int f = 0; f < 6; ++f)
    {
        sub[f].pSysMem = faceData[f].data();
        sub[f].SysMemPitch = desc.Width * sizeof(uint32_t);
        sub[f].SysMemSlicePitch = 0;
    }

    HRESULT hr = m_device->CreateTexture2D(&desc, sub.data(), &m_envCubemap);
    if (FAILED(hr)) return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;
    hr = m_device->CreateShaderResourceView(m_envCubemap.Get(), &srvDesc, &m_envSRV);
    if (FAILED(hr)) return hr;

    // Compile Environment.ps
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> errBlob;
    hr = D3DCompileFromFile(L"Environment.ps", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "ps_5_0", flags, 0, &psBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        return hr;
    }
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_environmentPS);
    return hr;
}

HRESULT Render::LoadHDRI(const wchar_t* filename)
{
    // Конвертируем wchar_t* в char*
    char filenameA[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, filename, -1, filenameA, MAX_PATH, nullptr, nullptr);

    int width, height, channels;
    float* data = stbi_loadf(filenameA, &width, &height, &channels, 4); // принудительно 4 канала

    if (!data)
    {
        OutputDebugString(L"Ошибка загрузки HDRI файла\n");
        return E_FAIL;
    }

    // Создание 2D текстуры формата R32G32B32A32_FLOAT
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = data;
    initData.SysMemPitch = width * 4 * sizeof(float);

    HRESULT hr = m_device->CreateTexture2D(&desc, &initData, &m_hdriTexture);
    stbi_image_free(data);

    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания HDRI текстуры\n");
        return hr;
    }

    // Создание SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(m_hdriTexture.Get(), &srvDesc, &m_hdriSRV);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания HDRI SRV\n");
        return hr;
    }

    return S_OK;
}

HRESULT Render::ConvertEquirectToCubemap()
{
    const UINT cubemapSize = 512;

    // Создание cubemap текстуры
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = cubemapSize;
    desc.Height = cubemapSize;
    desc.MipLevels = 1;
    desc.ArraySize = 6;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_hdriCubemap);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания cubemap текстуры\n");
        return hr;
    }

    // Создание SRV для cubemap
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(m_hdriCubemap.Get(), &srvDesc, &m_hdriCubemapSRV);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания cubemap SRV\n");
        return hr;
    }

    // Компиляция шейдеров для конвертации
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    #ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    #endif

    // Вершинный шейдер
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    hr = D3DCompileFromFile(L"CubemapFace.vs", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "vs_5_0", flags, 0, &vsBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        OutputDebugString(L"Ошибка компиляции CubemapFace.vs\n");
        return hr;
    }

    ComPtr<ID3D11VertexShader> cubemapVS;
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &cubemapVS);
    if (FAILED(hr)) return hr;

    // Пиксельный шейдер
    hr = D3DCompileFromFile(L"EquirectToCubemap.ps", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "ps_5_0", flags, 0, &psBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        OutputDebugString(L"Ошибка компиляции EquirectToCubemap.ps\n");
        return hr;
    }

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_equirectToCubemapPS);
    if (FAILED(hr)) return hr;

    // Создание input layout для cubemap vertex shader
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    ComPtr<ID3D11InputLayout> cubemapInputLayout;
    hr = m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &cubemapInputLayout);
    if (FAILED(hr)) return hr;

    // Создание константного буфера для ViewProj матрицы каждой грани
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(XMMATRIX);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Buffer> viewProjCB;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &viewProjCB);
    if (FAILED(hr)) return hr;

    // Камеры для каждой грани cubemap 
    XMVECTOR eye = XMVectorSet(0, 0, 0, 1);

    XMVECTOR directions[6] = {
        XMVectorSet(1, 0, 0, 0),    // +X
        XMVectorSet(-1, 0, 0, 0),   // -X
        XMVectorSet(0, 1, 0, 0),    // +Y
        XMVectorSet(0, -1, 0, 0),   // -Y
        XMVectorSet(0, 0, 1, 0),    // +Z
        XMVectorSet(0, 0, -1, 0)    // -Z
    };

    XMVECTOR ups[6] = {
        XMVectorSet(0, 1, 0, 0),    // +X
        XMVectorSet(0, 1, 0, 0),    // -X
        XMVectorSet(0, 0, -1, 0),   // +Y
        XMVectorSet(0, 0, 1, 0),    // -Y
        XMVectorSet(0, 1, 0, 0),    // +Z
        XMVectorSet(0, 1, 0, 0)     // -Z
    };

    // Создание проекционной матрицы (90 градусов FOV)
    const float nearp = 0.1f;
    const float farp = 10.0f;
    const float fov = XM_PIDIV2;
    const float width = nearp / tanf(fov / 2.0f);
    const float height = width;
    XMMATRIX proj = XMMatrixPerspectiveLH(2.0f * width, 2.0f * height, nearp, farp);

    // Создание quad геометрии 
    struct CubemapVertex {
        float pos[3];
        float uv[2];
    };

    // Quad перед камерой на расстоянии 0.5 
    CubemapVertex quadVerts[4] = {
        { {-0.5f, -0.5f, 0.5f}, {0, 1} },
        { {-0.5f,  0.5f, 0.5f}, {0, 0} },
        { { 0.5f,  0.5f, 0.5f}, {1, 0} },
        { { 0.5f, -0.5f, 0.5f}, {1, 1} }
    };

    WORD quadIndices[6] = { 0, 1, 2, 0, 2, 3 };

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(quadVerts);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = quadVerts;

    ComPtr<ID3D11Buffer> quadVB;
    hr = m_device->CreateBuffer(&vbDesc, &vbData, &quadVB);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC ibDesc{};
    ibDesc.ByteWidth = sizeof(quadIndices);
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData{};
    ibData.pSysMem = quadIndices;

    ComPtr<ID3D11Buffer> quadIB;
    hr = m_device->CreateBuffer(&ibDesc, &ibData, &quadIB);
    if (FAILED(hr)) return hr;

    // Рендеринг каждой грани
    for (UINT face = 0; face < 6; ++face)
    {
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(ups[face], directions[face]));
        XMVECTOR up = XMVector3Normalize(XMVector3Cross(directions[face], right));

        XMVECTOR faceCorners[4] = {
            XMVector3Normalize(directions[face] - right - up),
            XMVector3Normalize(directions[face] - right + up),
            XMVector3Normalize(directions[face] + right + up),
            XMVector3Normalize(directions[face] + right - up)
        };

        CubemapVertex faceVerts[4] = {
            { { 0.0f, 0.0f, 0.0f }, {0, 1} },
            { { 0.0f, 0.0f, 0.0f }, {0, 0} },
            { { 0.0f, 0.0f, 0.0f }, {1, 0} },
            { { 0.0f, 0.0f, 0.0f }, {1, 1} }
        };

        for (int i = 0; i < 4; ++i)
        {
            XMFLOAT3 pos;
            XMStoreFloat3(&pos, faceCorners[i]);
            faceVerts[i].pos[0] = pos.x;
            faceVerts[i].pos[1] = pos.y;
            faceVerts[i].pos[2] = pos.z;
        }

        m_context->UpdateSubresource(quadVB.Get(), 0, nullptr, faceVerts, 0, 0);

        // Создание RTV для текущей грани
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = desc.Format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.ArraySize = 1;
        rtvDesc.Texture2DArray.FirstArraySlice = face;
        rtvDesc.Texture2DArray.MipSlice = 0;

        ComPtr<ID3D11RenderTargetView> rtv;
        hr = m_device->CreateRenderTargetView(m_hdriCubemap.Get(), &rtvDesc, &rtv);
        if (FAILED(hr)) return hr;

        // Настройка view матрицы для грани (используем LookToLH)
        XMMATRIX view = XMMatrixLookToLH(eye, directions[face], ups[face]);
        XMMATRIX viewProj = XMMatrixTranspose(view * proj);

        // Обновление константного буфера
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = m_context->Map(viewProjCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            memcpy(mapped.pData, &viewProj, sizeof(XMMATRIX));
            m_context->Unmap(viewProjCB.Get(), 0);
        }

        // Установка render target
        m_context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

        // Viewport
        D3D11_VIEWPORT viewport{};
        viewport.Width = (float)cubemapSize;
        viewport.Height = (float)cubemapSize;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);

        // Clear
        float clearColor[4] = { 0, 0, 0, 1 };
        m_context->ClearRenderTargetView(rtv.Get(), clearColor);

        // Настройка rasterizer state
        D3D11_RASTERIZER_DESC rsDesc = {};
        rsDesc.FillMode = D3D11_FILL_SOLID;
        rsDesc.CullMode = D3D11_CULL_NONE;
        rsDesc.FrontCounterClockwise = FALSE;
        rsDesc.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rsState;
        m_device->CreateRasterizerState(&rsDesc, &rsState);
        m_context->RSSetState(rsState.Get());

        // Рендеринг quad
        m_context->IASetInputLayout(cubemapInputLayout.Get());
        UINT stride = sizeof(CubemapVertex);
        UINT offset = 0;
        m_context->IASetVertexBuffers(0, 1, quadVB.GetAddressOf(), &stride, &offset);
        m_context->IASetIndexBuffer(quadIB.Get(), DXGI_FORMAT_R16_UINT, 0);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        m_context->VSSetShader(cubemapVS.Get(), nullptr, 0);
        m_context->VSSetConstantBuffers(0, 1, viewProjCB.GetAddressOf());

        m_context->PSSetShader(m_equirectToCubemapPS.Get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, m_hdriSRV.GetAddressOf());
        m_context->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());

        m_context->DrawIndexed(6, 0, 0);

        // Очистка
        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Восстановление rasterizer state
    m_context->RSSetState(nullptr);

    // Восстановление render targets
    m_context->OMSetRenderTargets(1, m_hdrRTV.GetAddressOf(), m_depthStencil.Get());

    // Восстановление viewport к размерам экрана
    RECT rect;
    GetClientRect(m_hwnd, &rect);
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)(rect.right - rect.left);
    vp.Height = (float)(rect.bottom - rect.top);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    return S_OK;
}

HRESULT Render::ComputeIrradianceMap()
{
    const UINT irradianceSize = 32;

    // Создание irradiance cubemap
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = irradianceSize;
    desc.Height = irradianceSize;
    desc.MipLevels = 1;
    desc.ArraySize = 6;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_irradianceMap);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания irradiance текстуры\n");
        return hr;
    }

    // Создание SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(m_irradianceMap.Get(), &srvDesc, &m_irradianceSRV);
    if (FAILED(hr))
    {
        OutputDebugString(L"Ошибка создания irradiance SRV\n");
        return hr;
    }

    // Компиляция irradiance shader
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    #ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    #endif

    ComPtr<ID3DBlob> psBlob, errBlob;
    hr = D3DCompileFromFile(L"Irradiance.ps", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "ps_5_0", flags, 0, &psBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        OutputDebugString(L"Ошибка компиляции Irradiance.ps\n");
        return hr;
    }

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_irradiancePS);
    if (FAILED(hr)) return hr;

    // Используем уже существующие ресурсы для рендеринга (cubemap VS, quad geometry)
    // Компилируем вершинный шейдер снова для input layout
    ComPtr<ID3DBlob> vsBlob;
    hr = D3DCompileFromFile(L"CubemapFace.vs", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "vs_5_0", flags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11VertexShader> cubemapVS;
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &cubemapVS);
    if (FAILED(hr)) return hr;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    ComPtr<ID3D11InputLayout> cubemapInputLayout;
    hr = m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &cubemapInputLayout);
    if (FAILED(hr)) return hr;

    // Константный буфер
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(XMMATRIX);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Buffer> viewProjCB;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &viewProjCB);
    if (FAILED(hr)) return hr;

    // Quad geometry
    struct CubemapVertex {
        float pos[3];
        float uv[2];
    };

    CubemapVertex quadVerts[4] = {
        { {-0.5f, -0.5f, 0.5f}, {0, 1} },
        { {-0.5f,  0.5f, 0.5f}, {0, 0} },
        { { 0.5f,  0.5f, 0.5f}, {1, 0} },
        { { 0.5f, -0.5f, 0.5f}, {1, 1} }
    };

    WORD quadIndices[6] = { 0, 1, 2, 0, 2, 3 };

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(quadVerts);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = quadVerts;

    ComPtr<ID3D11Buffer> quadVB;
    hr = m_device->CreateBuffer(&vbDesc, &vbData, &quadVB);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC ibDesc{};
    ibDesc.ByteWidth = sizeof(quadIndices);
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData{};
    ibData.pSysMem = quadIndices;

    ComPtr<ID3D11Buffer> quadIB;
    hr = m_device->CreateBuffer(&ibDesc, &ibData, &quadIB);
    if (FAILED(hr)) return hr;

    // Camera setup для каждой грани
    XMVECTOR eye = XMVectorSet(0, 0, 0, 1);

    XMVECTOR directions[6] = {
        XMVectorSet(1, 0, 0, 0),
        XMVectorSet(-1, 0, 0, 0),
        XMVectorSet(0, 1, 0, 0),
        XMVectorSet(0, -1, 0, 0),
        XMVectorSet(0, 0, 1, 0),
        XMVectorSet(0, 0, -1, 0)
    };

    XMVECTOR ups[6] = {
        XMVectorSet(0, 1, 0, 0),
        XMVectorSet(0, 1, 0, 0),
        XMVectorSet(0, 0, -1, 0),
        XMVectorSet(0, 0, 1, 0),
        XMVectorSet(0, 1, 0, 0),
        XMVectorSet(0, 1, 0, 0)
    };

    const float nearp = 0.1f;
    const float farp = 10.0f;
    const float fov = XM_PIDIV2;
    const float width = nearp / tanf(fov / 2.0f);
    const float height = width;
    XMMATRIX proj = XMMatrixPerspectiveLH(2.0f * width, 2.0f * height, nearp, farp);

    // Рендеринг каждой грани
    for (UINT face = 0; face < 6; ++face)
    {
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(ups[face], directions[face]));
        XMVECTOR up = XMVector3Normalize(XMVector3Cross(directions[face], right));

        XMVECTOR faceCorners[4] = {
            XMVector3Normalize(directions[face] - right - up),
            XMVector3Normalize(directions[face] - right + up),
            XMVector3Normalize(directions[face] + right + up),
            XMVector3Normalize(directions[face] + right - up)
        };

        CubemapVertex faceVerts[4] = {
            { { 0.0f, 0.0f, 0.0f }, {0, 1} },
            { { 0.0f, 0.0f, 0.0f }, {0, 0} },
            { { 0.0f, 0.0f, 0.0f }, {1, 0} },
            { { 0.0f, 0.0f, 0.0f }, {1, 1} }
        };

        for (int i = 0; i < 4; ++i)
        {
            XMFLOAT3 pos;
            XMStoreFloat3(&pos, faceCorners[i]);
            faceVerts[i].pos[0] = pos.x;
            faceVerts[i].pos[1] = pos.y;
            faceVerts[i].pos[2] = pos.z;
        }

        m_context->UpdateSubresource(quadVB.Get(), 0, nullptr, faceVerts, 0, 0);

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = desc.Format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.ArraySize = 1;
        rtvDesc.Texture2DArray.FirstArraySlice = face;
        rtvDesc.Texture2DArray.MipSlice = 0;

        ComPtr<ID3D11RenderTargetView> rtv;
        hr = m_device->CreateRenderTargetView(m_irradianceMap.Get(), &rtvDesc, &rtv);
        if (FAILED(hr)) return hr;

        XMMATRIX view = XMMatrixLookToLH(eye, directions[face], ups[face]);
        XMMATRIX viewProj = XMMatrixTranspose(view * proj);

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = m_context->Map(viewProjCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            memcpy(mapped.pData, &viewProj, sizeof(XMMATRIX));
            m_context->Unmap(viewProjCB.Get(), 0);
        }

        m_context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

        D3D11_VIEWPORT viewport{};
        viewport.Width = (float)irradianceSize;
        viewport.Height = (float)irradianceSize;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);

        float clearColor[4] = { 0, 0, 0, 1 };
        m_context->ClearRenderTargetView(rtv.Get(), clearColor);

        // Настройка rasterizer state
        D3D11_RASTERIZER_DESC rsDesc = {};
        rsDesc.FillMode = D3D11_FILL_SOLID;
        rsDesc.CullMode = D3D11_CULL_NONE;
        rsDesc.FrontCounterClockwise = FALSE;
        rsDesc.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rsState;
        m_device->CreateRasterizerState(&rsDesc, &rsState);
        m_context->RSSetState(rsState.Get());

        m_context->IASetInputLayout(cubemapInputLayout.Get());
        UINT stride = sizeof(CubemapVertex);
        UINT offset = 0;
        m_context->IASetVertexBuffers(0, 1, quadVB.GetAddressOf(), &stride, &offset);
        m_context->IASetIndexBuffer(quadIB.Get(), DXGI_FORMAT_R16_UINT, 0);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        m_context->VSSetShader(cubemapVS.Get(), nullptr, 0);
        m_context->VSSetConstantBuffers(0, 1, viewProjCB.GetAddressOf());

        m_context->PSSetShader(m_irradiancePS.Get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, m_hdriCubemapSRV.GetAddressOf());
        m_context->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());

        m_context->DrawIndexed(6, 0, 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Восстановление rasterizer state
    m_context->RSSetState(nullptr);

    // Восстановление render targets
    m_context->OMSetRenderTargets(1, m_hdrRTV.GetAddressOf(), m_depthStencil.Get());

    // Восстановление viewport к размерам экрана
    RECT rect;
    GetClientRect(m_hwnd, &rect);
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)(rect.right - rect.left);
    vp.Height = (float)(rect.bottom - rect.top);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    return S_OK;
}

HRESULT Render::CreatePrefilteredMap()
{
    if (!m_hdriCubemapSRV)
    {
        OutputDebugString(L"CreatePrefilteredMap: HDRI cubemap not available\n");
        return E_FAIL;
    }

    // Создание текстуры cubemap с mip-уровнями
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_prefilteredSize;
    desc.Height = m_prefilteredSize;
    desc.MipLevels = m_prefilteredMipLevels;
    desc.ArraySize = 6; // 6 граней
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_prefilteredCubemap);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreatePrefilteredMap: failed to create texture\n");
        return hr;
    }

    // SRV для prefiltered cubemap
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = m_prefilteredMipLevels;
    srvDesc.TextureCube.MostDetailedMip = 0;
    hr = m_device->CreateShaderResourceView(m_prefilteredCubemap.Get(), &srvDesc, &m_prefilteredSRV);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreatePrefilteredMap: failed to create SRV\n");
        return hr;
    }

    // Загрузка предварительно скомпилированного шейдера Prefilter.cso
    ComPtr<ID3DBlob> psBlob;
    hr = D3DReadFileToBlob(L"Prefilter.cso", &psBlob);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreatePrefilteredMap: failed to load Prefilter.cso\n");
        return hr;
    }
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_prefilterPS);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreatePrefilteredMap: failed to create pixel shader\n");
        return hr;
    }

    // Загрузка предварительно скомпилированного шейдера CubemapFace.cso
    ComPtr<ID3DBlob> vsBlob;
    hr = D3DReadFileToBlob(L"CubemapFace.cso", &vsBlob);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreatePrefilteredMap: failed to load CubemapFace.cso\n");
        return hr;
    }
    ComPtr<ID3D11VertexShader> cubemapVS;
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &cubemapVS);
    if (FAILED(hr)) return hr;

    // Input layout для CubemapFace.vs
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    ComPtr<ID3D11InputLayout> cubemapLayout;
    hr = m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &cubemapLayout);
    if (FAILED(hr)) return hr;

    // Константный буфер для матрицы viewProj
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(XMMATRIX);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> viewProjCB;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &viewProjCB);
    if (FAILED(hr)) return hr;

    // Константный буфер для roughness
    struct PrefilterConstants {
        float roughness;
        float padding[3];
    };
    D3D11_BUFFER_DESC pcDesc = {};
    pcDesc.ByteWidth = sizeof(PrefilterConstants);
    pcDesc.Usage = D3D11_USAGE_DYNAMIC;
    pcDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    pcDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> prefilterCB;
    hr = m_device->CreateBuffer(&pcDesc, nullptr, &prefilterCB);
    if (FAILED(hr)) return hr;

    // Настройки камеры для каждой грани
    XMVECTOR eye = XMVectorSet(0, 0, 0, 1);
    XMVECTOR directions[6] = {
        XMVectorSet(1,  0,  0, 0), // +X
        XMVectorSet(-1,  0,  0, 0), // -X
        XMVectorSet(0,  1,  0, 0), // +Y
        XMVectorSet(0, -1,  0, 0), // -Y
        XMVectorSet(0,  0,  1, 0), // +Z
        XMVectorSet(0,  0, -1, 0)  // -Z
    };
    XMVECTOR ups[6] = {
        XMVectorSet(0, 1, 0, 0), // +X
        XMVectorSet(0, 1, 0, 0), // -X
        XMVectorSet(0, 0, -1, 0), // +Y
        XMVectorSet(0, 0, 1, 0),  // -Y
        XMVectorSet(0, 1, 0, 0), // +Z
        XMVectorSet(0, 1, 0, 0)  // -Z
    };

    const float nearp = 0.1f;
    const float farp = 10.0f;
    const float fov = XM_PIDIV2;
    const float width = nearp / tanf(fov / 2.0f);
    const float height = width;
    XMMATRIX proj = XMMatrixPerspectiveLH(2.0f * width, 2.0f * height, nearp, farp);

    // Квад-геометрия (для рендеринга граней)
    struct PrefilterVertex {
        float pos[3];
        float uv[2];
    };
    PrefilterVertex quadVerts[4] = {
        { {-0.5f, -0.5f, 0.5f}, {0, 1} },
        { {-0.5f,  0.5f, 0.5f}, {0, 0} },
        { { 0.5f,  0.5f, 0.5f}, {1, 0} },
        { { 0.5f, -0.5f, 0.5f}, {1, 1} }
    };
    WORD quadIndices[6] = { 0, 1, 2, 0, 2, 3 };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(quadVerts);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = { quadVerts };
    ComPtr<ID3D11Buffer> quadVB;
    hr = m_device->CreateBuffer(&vbDesc, &vbData, &quadVB);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = sizeof(quadIndices);
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = { quadIndices };
    ComPtr<ID3D11Buffer> quadIB;
    hr = m_device->CreateBuffer(&ibDesc, &ibData, &quadIB);
    if (FAILED(hr)) return hr;

    // Rasterizer state (отключаем culling)
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rsState;
    m_device->CreateRasterizerState(&rsDesc, &rsState);

    // Сохраняем старые состояния (чтобы восстановить после)
    ComPtr<ID3D11RenderTargetView> oldRTV;
    ComPtr<ID3D11DepthStencilView> oldDSV;
    m_context->OMGetRenderTargets(1, &oldRTV, &oldDSV);
    ComPtr<ID3D11RasterizerState> oldRS;
    m_context->RSGetState(&oldRS);

    // Основной цикл: для каждого mip-уровня (roughness)
    for (UINT mip = 0; mip < m_prefilteredMipLevels; ++mip)
    {
        float roughness = (float)mip / (float)(m_prefilteredMipLevels - 1);
        // Обновляем константный буфер roughness
        PrefilterConstants pc = { roughness, 0,0,0 };
        D3D11_MAPPED_SUBRESOURCE mappedPC;
        if (SUCCEEDED(m_context->Map(prefilterCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedPC)))
        {
            memcpy(mappedPC.pData, &pc, sizeof(pc));
            m_context->Unmap(prefilterCB.Get(), 0);
        }

        // Рендерим каждую грань
        for (UINT face = 0; face < 6; ++face)
        {
            // RTV для текущей грани и mip-уровня
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = desc.Format;
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.ArraySize = 1;
            rtvDesc.Texture2DArray.FirstArraySlice = face;
            rtvDesc.Texture2DArray.MipSlice = mip;
            ComPtr<ID3D11RenderTargetView> rtv;
            hr = m_device->CreateRenderTargetView(m_prefilteredCubemap.Get(), &rtvDesc, &rtv);
            if (FAILED(hr)) return hr;

            // Матрица view для грани
            XMMATRIX view = XMMatrixLookToLH(eye, directions[face], ups[face]);
            XMMATRIX viewProjMat = XMMatrixTranspose(view * proj);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(viewProjCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                memcpy(mapped.pData, &viewProjMat, sizeof(XMMATRIX));
                m_context->Unmap(viewProjCB.Get(), 0);
            }

            // Установка render target
            m_context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

            // Viewport (размер уменьшается с каждым mip-уровнем)
            UINT currentSize = m_prefilteredSize >> mip;
            if (currentSize < 1) currentSize = 1;
            D3D11_VIEWPORT vp = {};
            vp.Width = (float)currentSize;
            vp.Height = (float)currentSize;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &vp);

            // Очистка (необязательно)
            float clearColor[4] = { 0, 0, 0, 1 };
            m_context->ClearRenderTargetView(rtv.Get(), clearColor);

            // Установка состояния растеризации
            m_context->RSSetState(rsState.Get());

            // Установка входной сборки
            UINT stride = sizeof(PrefilterVertex);
            UINT offset = 0;
            m_context->IASetVertexBuffers(0, 1, quadVB.GetAddressOf(), &stride, &offset);
            m_context->IASetIndexBuffer(quadIB.Get(), DXGI_FORMAT_R16_UINT, 0);
            m_context->IASetInputLayout(cubemapLayout.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // Шейдеры и ресурсы
            m_context->VSSetShader(cubemapVS.Get(), nullptr, 0);
            m_context->VSSetConstantBuffers(0, 1, viewProjCB.GetAddressOf());

            m_context->PSSetShader(m_prefilterPS.Get(), nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, prefilterCB.GetAddressOf());
            m_context->PSSetShaderResources(0, 1, m_hdriCubemapSRV.GetAddressOf());
            m_context->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());

            // Рисуем quad
            m_context->DrawIndexed(6, 0, 0);

            // Отвязываем ресурсы
            ID3D11ShaderResourceView* nullSRV = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullSRV);
        }
    }

    // Восстановление старых состояний
    m_context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
    m_context->RSSetState(oldRS.Get());
    // Восстанавливаем viewport на весь экран (позже DrawScene установит свой)
    RECT rect;
    GetClientRect(m_hwnd, &rect);
    D3D11_VIEWPORT vpFull = { 0, 0, (float)(rect.right - rect.left), (float)(rect.bottom - rect.top), 0, 1 };
    m_context->RSSetViewports(1, &vpFull);

    OutputDebugString(L"CreatePrefilteredMap completed successfully\n");
    return S_OK;
}

HRESULT Render::CreateBRDFLUT()
{
    const UINT lutSize = 512; // размер LUT текстуры

    // Создание 2D текстуры формата R32G32_FLOAT
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = lutSize;
    desc.Height = lutSize;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_brdfLUT);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreateBRDFLUT: failed to create texture\n");
        return hr;
    }

    // SRV для BRDF LUT
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    hr = m_device->CreateShaderResourceView(m_brdfLUT.Get(), &srvDesc, &m_brdfSRV);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreateBRDFLUT: failed to create SRV\n");
        return hr;
    }

    // Загрузка пиксельного шейдера BRDF.cso
    ComPtr<ID3DBlob> psBlob;
    hr = D3DReadFileToBlob(L"BRDF.cso", &psBlob);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreateBRDFLUT: failed to load BRDF.cso\n");
        return hr;
    }
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_brdfPS);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreateBRDFLUT: failed to create pixel shader\n");
        return hr;
    }

    // Создание sampler с CLAMP адресацией
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_device->CreateSamplerState(&sampDesc, &m_brdfSampler);
    if (FAILED(hr))
    {
        OutputDebugString(L"CreateBRDFLUT: failed to create sampler\n");
        return hr;
    }

    // Сохраняем старые состояния
    ComPtr<ID3D11RenderTargetView> oldRTV;
    ComPtr<ID3D11DepthStencilView> oldDSV;
    m_context->OMGetRenderTargets(1, &oldRTV, &oldDSV);

    ComPtr<ID3D11VertexShader> oldVS;
    ComPtr<ID3D11PixelShader> oldPS;
    ComPtr<ID3D11InputLayout> oldLayout;
    m_context->VSGetShader(&oldVS, nullptr, nullptr);
    m_context->PSGetShader(&oldPS, nullptr, nullptr);
    m_context->IAGetInputLayout(&oldLayout);

    D3D11_VIEWPORT oldVP = {};
    UINT numVP = 1;
    m_context->RSGetViewports(&numVP, &oldVP);

    // Создаём RTV для BRDF текстуры
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = m_device->CreateRenderTargetView(m_brdfLUT.Get(), nullptr, &rtv);
    if (FAILED(hr)) return hr;

    // Устанавливаем RTV и viewport
    m_context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)lutSize, (float)lutSize, 0, 1 };
    m_context->RSSetViewports(1, &vp);

    // Очистка (необязательно)
    float clearColor[4] = { 0, 0, 0, 0 };
    m_context->ClearRenderTargetView(rtv.Get(), clearColor);

    // Настройка fullscreen quad
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_quadVertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetInputLayout(m_quadInputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->VSSetShader(m_quadVS.Get(), nullptr, 0);
    m_context->PSSetShader(m_brdfPS.Get(), nullptr, 0);

    // Рисуем quad
    m_context->Draw(4, 0);

    // Восстановление старых состояний
    m_context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
    m_context->VSSetShader(oldVS.Get(), nullptr, 0);
    m_context->PSSetShader(oldPS.Get(), nullptr, 0);
    m_context->IASetInputLayout(oldLayout.Get());
    m_context->RSSetViewports(1, &oldVP);

    OutputDebugString(L"CreateBRDFLUT completed successfully\n");
    return S_OK;
}

HRESULT Render::CreateSkyboxResources()
{
    // Создание геометрии сферы для skybox (используем ту же геометрию что и для PBR объекта)
    const int slices = 64;
    const int stacks = 32;
    const float radius = 1.0f;

    std::vector<Vertex> vertices;
    std::vector<WORD> indices;
    vertices.reserve((stacks + 1) * (slices + 1));
    indices.reserve(stacks * slices * 6);

    for (int stack = 0; stack <= stacks; ++stack)
    {
        float v = (float)stack / (float)stacks;
        float phi = v * XM_PI;
        float y = cosf(phi);
        float r = sinf(phi);

        for (int slice = 0; slice <= slices; ++slice)
        {
            float u = (float)slice / (float)slices;
            float theta = u * XM_2PI;

            float x = r * cosf(theta);
            float z = r * sinf(theta);

            Vertex vert{};
            vert.pos[0] = radius * x;
            vert.pos[1] = radius * y;
            vert.pos[2] = radius * z;

            vert.normal[0] = x;
            vert.normal[1] = y;
            vert.normal[2] = z;

            vert.color[0] = 1.0f;
            vert.color[1] = 1.0f;
            vert.color[2] = 1.0f;
            vert.color[3] = 1.0f;

            vertices.push_back(vert);
        }
    }

    auto idx = [slices](int stack, int slice) -> WORD {
        return (WORD)(stack * (slices + 1) + slice);
    };

    for (int stack = 0; stack < stacks; ++stack)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            WORD i0 = idx(stack, slice);
            WORD i1 = idx(stack + 1, slice);
            WORD i2 = idx(stack + 1, slice + 1);
            WORD i3 = idx(stack, slice + 1);

            indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i3);
        }
    }

    m_skyboxIndexCount = (UINT)indices.size();

    // Вершинный буфер
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = (UINT)(sizeof(Vertex) * vertices.size());
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertices.data();

    HRESULT hr = m_device->CreateBuffer(&vbDesc, &vbData, &m_skyboxVertexBuffer);
    if (FAILED(hr)) return hr;

    // Индексный буфер
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = (UINT)(sizeof(WORD) * indices.size());
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = indices.data();

    hr = m_device->CreateBuffer(&ibDesc, &ibData, &m_skyboxIndexBuffer);
    if (FAILED(hr)) return hr;

    // Компиляция шейдеров
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    #ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    #endif

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;

    // Вершинный шейдер
    hr = D3DCompileFromFile(L"Skybox.vs", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "vs_5_0", flags, 0, &vsBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        OutputDebugString(L"Ошибка компиляции Skybox.vs\n");
        return hr;
    }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_skyboxVS);
    if (FAILED(hr)) return hr;

    // Пиксельный шейдер
    hr = D3DCompileFromFile(L"Skybox.ps", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "ps_5_0", flags, 0, &psBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        OutputDebugString(L"Ошибка компиляции Skybox.ps\n");
        return hr;
    }

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_skyboxPS);
    if (FAILED(hr)) return hr;

    // Input layout (такой же как для основной геометрии)
    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = m_device->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_skyboxInputLayout);
    if (FAILED(hr)) return hr;

    return S_OK;
}

void Render::DrawSkybox()
{
    if (!m_skyboxVS || !m_skyboxPS) return;

    // Используем HDRI cubemap если доступен, иначе процедурный
    ID3D11ShaderResourceView* envToUse = m_hdriCubemapSRV ? m_hdriCubemapSRV.Get() : m_envSRV.Get();
    if (!envToUse) return;

    // Отключаем depth write для skybox (но оставляем depth test)
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // не пишем в depth
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dsDesc.StencilEnable = FALSE;

    ComPtr<ID3D11DepthStencilState> dsState;
    m_device->CreateDepthStencilState(&dsDesc, &dsState);
    m_context->OMSetDepthStencilState(dsState.Get(), 0);

    // Отключаем culling
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthClipEnable = TRUE;

    ComPtr<ID3D11RasterizerState> rsState;
    m_device->CreateRasterizerState(&rsDesc, &rsState);
    m_context->RSSetState(rsState.Get());

    // Настройка pipeline
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_skyboxVertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetIndexBuffer(m_skyboxIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(m_skyboxInputLayout.Get());

    m_context->VSSetShader(m_skyboxVS.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_worldBuffer.GetAddressOf());
    m_context->VSSetConstantBuffers(1, 1, m_viewProjBuffer.GetAddressOf());

    m_context->PSSetShader(m_skyboxPS.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &envToUse);
    m_context->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());

    m_context->DrawIndexed(m_skyboxIndexCount, 0, 0);

    // Очистка
    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);

    // Восстанавливаем нормальное depth state
    m_context->OMSetDepthStencilState(nullptr, 0);
    m_context->RSSetState(nullptr);
}


void Render::DrawEnvironmentToCurrentTarget()
{
    if (!m_environmentPS) return;

    // Используем HDRI cubemap если доступен, иначе процедурный
    ID3D11ShaderResourceView* envToUse = m_hdriCubemapSRV ? m_hdriCubemapSRV.Get() : m_envSRV.Get();
    if (!envToUse) return;

    // Draw fullscreen quad with cubemap sampled by view ray.
    m_context->VSSetShader(m_quadVS.Get(), nullptr, 0);
    m_context->PSSetShader(m_environmentPS.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &envToUse);
    m_context->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());
    m_context->PSSetConstantBuffers(1, 1, m_viewProjBuffer.GetAddressOf());

    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_quadVertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetIndexBuffer(m_quadIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    m_context->IASetInputLayout(m_quadInputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->DrawIndexed(6, 0, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);
}

