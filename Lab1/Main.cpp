#include <windowsx.h>
#include <DirectXMath.h>

#include "AppFramework.h"
#include "Render.h"
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ImGui Headers - добавляем файлы из папки "imgui"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
// Явное объявление для ImGui функции
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

constexpr int MAX_STRING_LENGTH = 100;

// Глобальные данные приложения
WCHAR g_AppTitle[MAX_STRING_LENGTH] = L"Lab1 DirectX11";
WCHAR g_WindowClassName[MAX_STRING_LENGTH] = L"Lab1WindowClass";

Render* g_pGraphics = nullptr;

static bool   g_mouseRightPressed = false;
static int    g_prevMouseX = 0;
static int    g_prevMouseY = 0;
static UINT   g_windowWidth = 800;   // начальные размеры (должны совпадать с CreateWindow)
static UINT   g_windowHeight = 600;
static const float g_cameraRotSpeed = DirectX::XM_2PI; // полный оборот за ширину экрана

// Объявления функций
ATOM RegisterAppWindow(HINSTANCE hInst);
BOOL CreateAppWindow(HINSTANCE hInst, int showCmd);
LRESULT CALLBACK WndProcedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void OnWindowSizeChanged(HWND hwnd);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Регистрация окна
    if (!RegisterAppWindow(hInstance))
    {
        OutputDebugString(L"Ошибка регистрации класса окна\n");
        return FALSE;
    }

    // Создание окна
    if (!CreateAppWindow(hInstance, nCmdShow))
    {
        OutputDebugString(L"Ошибка создания окна\n");
        return FALSE;
    }

    // Главный цикл обработки сообщений
    MSG message = {};
    while (message.message != WM_QUIT)
    {
        if (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        else
        {
            // Рендеринг сцены
            if (g_pGraphics)
            {
                g_pGraphics->DrawScene();
            }
        }
    }

    // Освобождение ресурсов
    if (g_pGraphics)
    {
        g_pGraphics->Shutdown();
        delete g_pGraphics;
        g_pGraphics = nullptr;
    }

    return static_cast<int>(message.wParam);
}

ATOM RegisterAppWindow(HINSTANCE hInst)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcedure;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_LAB1));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = g_WindowClassName;
    wc.hIconSm = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wc);
}

BOOL CreateAppWindow(HINSTANCE hInst, int showCmd)
{
    HWND hwnd = CreateWindowW(g_WindowClassName, g_AppTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 800, 600,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd)
    {
        OutputDebugString(L"Не удалось создать окно\n");
        return FALSE;
    }

    // Инициализация графического движка
    g_pGraphics = new Render();
    if (FAILED(g_pGraphics->Initialize(hwnd)))
    {
        OutputDebugString(L"Ошибка инициализации графики\n");
        delete g_pGraphics;
        g_pGraphics = nullptr;
        return FALSE;
    }

    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);

    return TRUE;
}

LRESULT CALLBACK WndProcedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        g_windowWidth = LOWORD(lp);
        g_windowHeight = HIWORD(lp);
        OnWindowSizeChanged(hwnd);
        break;

    case WM_RBUTTONDOWN:
        g_mouseRightPressed = true;
        g_prevMouseX = GET_X_LPARAM(lp);
        g_prevMouseY = GET_Y_LPARAM(lp);
        SetCapture(hwnd);               // захватываем мышь, чтобы получать сообщения даже вне окна
        break;

    case WM_RBUTTONUP:
        g_mouseRightPressed = false;
        ReleaseCapture();                // отпускаем мышь
        break;

    case WM_MOUSEMOVE:
        if (g_mouseRightPressed && g_pGraphics)
        {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);

            // Вычисляем относительное перемещение, нормируя на размер окна
            float dx = static_cast<float>(x - g_prevMouseX) / (g_windowWidth > 0 ? static_cast<float>(g_windowWidth) : 1.0f) * g_cameraRotSpeed;
            float dy = static_cast<float>(y - g_prevMouseY) / (g_windowHeight > 0 ? static_cast<float>(g_windowHeight) : 1.0f) * g_cameraRotSpeed;

            // Передаём приращения углов в камеру
            g_pGraphics->RotateAroundTarget(dx, dy);

            g_prevMouseX = x;
            g_prevMouseY = y;
        }
        break;

    case WM_MOUSEWHEEL:
        if (g_pGraphics)
        {
            short delta = GET_WHEEL_DELTA_WPARAM(wp);          
            float distance = delta / 120.0f * 0.5f;            // множитель для чувствительности
            g_pGraphics->Zoom(distance);
        }
        break;

    case WM_KEYDOWN:
        if (g_pGraphics)
        {
            switch (wp)
            {
            case 'W': // Поворот вверх
                g_pGraphics->RotateView(0.0f, 0.02f);
                break;
            case 'S': // Поворот вниз
                g_pGraphics->RotateView(0.0f, -0.02f);
                break;
            case 'A': // Поворот влево
                g_pGraphics->RotateView(-0.02f, 0.0f);
                break;
            case 'D': // Поворот вправо
                g_pGraphics->RotateView(0.02f, 0.0f);
                break;
            case VK_UP: // Движение вверх
                g_pGraphics->MoveView(0.0f, 0.1f, 0.0f);
                break;
            case VK_DOWN: // Движение вниз
                g_pGraphics->MoveView(0.0f, -0.1f, 0.0f);
                break;
            case VK_LEFT: // Движение влево
                g_pGraphics->MoveView(-0.1f, 0.0f, 0.0f);
                break;
            case VK_RIGHT: // Движение вправо
                g_pGraphics->MoveView(0.1f, 0.0f, 0.0f);
                break;

            case VK_ADD: // Приближение
            case 0xBB:
                g_pGraphics->MoveView(0.0f, 0.0f, 0.1f);
                break;
            case VK_SUBTRACT: // Отдаление
            case 0xBD:
                g_pGraphics->MoveView(0.0f, 0.0f, -0.1f);
                break;
            case VK_SPACE: // Вращение кубика
                g_pGraphics->ToggleAutoRotate();
                break;
            }
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

void OnWindowSizeChanged(HWND hwnd)
{
    if (g_pGraphics)
    {
        g_pGraphics->HandleResize(hwnd);
    }
}
