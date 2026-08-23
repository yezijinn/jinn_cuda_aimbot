#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <mutex>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "overlay.h"
#include "overlay/preview_window.h"
#include "overlay/draw_settings.h"
#include "overlay/config_dirty.h"
#include "mybot.h"

void draw_debug_frame();

namespace
{
constexpr int kPreviewDefaultWidth = 480;
constexpr int kPreviewDefaultHeight = 400;
const wchar_t kPreviewClassName[] = L"JinnCapturePreviewWindow";

HWND g_hPreviewWnd = nullptr;
IDXGISwapChain1* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

ImGuiContext* g_ctx = nullptr;
bool g_imguiReady = false;
bool g_resizePending = false;
bool g_closeRequested = false;

RECT g_lastWindowRect{};
bool g_hasLastWindowRect = false;

void SavePreviewRect(HWND hwnd)
{
    if (!hwnd || !::IsWindow(hwnd))
        return;

    RECT rc{};
    if (::GetWindowRect(hwnd, &rc))
    {
        g_lastWindowRect = rc;
        g_hasLastWindowRect = true;
    }
}

bool CreatePreviewRenderTarget(ID3D11Device* device)
{
    if (!device || !g_swapChain)
        return false;

    if (g_rtv)
    {
        g_rtv->Release();
        g_rtv = nullptr;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer)
        return false;

    hr = device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    backBuffer->Release();
    return SUCCEEDED(hr) && g_rtv != nullptr;
}

void CleanupPreviewResources()
{
    if (g_pd3dDeviceContext)
    {
        ID3D11RenderTargetView* nullRtv = nullptr;
        g_pd3dDeviceContext->OMSetRenderTargets(1, &nullRtv, nullptr);
    }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    g_resizePending = false;
}

void ResizePreviewBackBuffer(int width, int height)
{
    if (width <= 0 || height <= 0 || !g_swapChain)
        return;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreatePreviewRenderTarget(g_pd3dDevice);
    g_resizePending = false;
}

LRESULT CALLBACK PreviewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_ctx)
    {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_ctx);
        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        ImGui::SetCurrentContext(previous);
        if (handled)
            return handled;
    }

    if (msg == WM_SIZE)
    {
        g_resizePending = true;
        return 0;
    }
    if (msg == WM_CLOSE)
    {
        g_closeRequested = true;
        ::ShowWindow(hWnd, SW_HIDE);
        return 0;
    }
    return ::DefWindowProc(hWnd, msg, wParam, lParam);
}

bool RegisterPreviewClass()
{
    static bool registered = false;
    if (registered)
        return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kPreviewClassName;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    registered = ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

bool CreatePreviewSwapChain(ID3D11Device* device, int width, int height)
{
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    HRESULT hr = device ? device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) : E_POINTER;
    if (SUCCEEDED(hr))
        hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr))
    {
        IDXGIFactory* baseFactory = nullptr;
        hr = adapter->GetParent(IID_PPV_ARGS(&baseFactory));
        if (SUCCEEDED(hr))
        {
            hr = baseFactory->QueryInterface(IID_PPV_ARGS(&factory));
            baseFactory->Release();
        }
    }
    if (SUCCEEDED(hr))
    {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc.Scaling = DXGI_SCALING_STRETCH;
        hr = factory->CreateSwapChainForHwnd(device, g_hPreviewWnd, &desc, nullptr, nullptr, &g_swapChain);
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    return SUCCEEDED(hr) && g_swapChain != nullptr;
}

bool SetupPreviewImGui(ID3D11Device* device, ID3D11DeviceContext* deviceContext)
{
    if (g_imguiReady)
        return true;
    if (!device || !deviceContext || !g_hPreviewWnd)
        return false;

    ImGuiContext* previous = ImGui::GetCurrentContext();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    IMGUI_CHECKVERSION();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    static const ImWchar chineseRanges[] = {
        0x0020, 0x00FF, 0x2000, 0x206F, 0x3000, 0x30FF,
        0x31F0, 0x31FF, 0xFF00, 0xFFEF, 0x4E00, 0x9FAF, 0,
    };
    ImFontConfig fontConfig{};
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 1;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 16.5f, &fontConfig, chineseRanges))
        io.Fonts->AddFontDefault();

    const bool winOk = ImGui_ImplWin32_Init(g_hPreviewWnd);
    const bool d3dOk = ImGui_ImplDX11_Init(device, deviceContext);
    if (!winOk || !d3dOk)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_ctx);
        g_ctx = nullptr;
        ImGui::SetCurrentContext(previous);
        return false;
    }

    g_imguiReady = true;
    ImGui::SetCurrentContext(previous);
    return true;
}

bool CreatePreviewResources(ID3D11Device* device, ID3D11DeviceContext* deviceContext)
{
    if (!device || !deviceContext)
        return false;
    if (!RegisterPreviewClass())
        return false;

    if (!g_hPreviewWnd)
    {
        RECT rc{0, 0, kPreviewDefaultWidth, kPreviewDefaultHeight};
        ::AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (g_hasLastWindowRect && g_lastWindowRect.right > g_lastWindowRect.left)
        {
            x = g_lastWindowRect.left;
            y = g_lastWindowRect.top;
            w = g_lastWindowRect.right - g_lastWindowRect.left;
            h = g_lastWindowRect.bottom - g_lastWindowRect.top;
        }

        g_hPreviewWnd = ::CreateWindowExW(
            WS_EX_TOPMOST,
            kPreviewClassName,
            L"捕获预览",
            WS_OVERLAPPEDWINDOW,
            x,
            y,
            w,
            h,
            nullptr,
            nullptr,
            ::GetModuleHandleW(nullptr),
            nullptr);
        if (!g_hPreviewWnd)
            return false;
    }
    ::SetWindowPos(g_hPreviewWnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    RECT client{};
    ::GetClientRect(g_hPreviewWnd, &client);
    const int clientW = std::max(1, static_cast<int>(client.right - client.left));
    const int clientH = std::max(1, static_cast<int>(client.bottom - client.top));

    if (!g_swapChain && !CreatePreviewSwapChain(device, clientW, clientH))
        return false;
    if (!g_rtv && !CreatePreviewRenderTarget(device))
        return false;
    if (!g_imguiReady && !SetupPreviewImGui(device, deviceContext))
        return false;

    if (!::IsWindowVisible(g_hPreviewWnd))
        ::ShowWindow(g_hPreviewWnd, SW_SHOWNORMAL);
    return true;
}

void RenderPreviewFrame(ID3D11DeviceContext* deviceContext)
{
    if (!deviceContext || !g_swapChain || !g_rtv || !g_ctx)
        return;

    RECT client{};
    ::GetClientRect(g_hPreviewWnd, &client);
    const int clientW = std::max(1, static_cast<int>(client.right - client.left));
    const int clientH = std::max(1, static_cast<int>(client.bottom - client.top));
    if (g_resizePending)
        ResizePreviewBackBuffer(clientW, clientH);
    if (!g_rtv)
        return;

    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(clientW), static_cast<float>(clientH)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    if (ImGui::Begin("##preview_fill", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        draw_debug_frame();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::Render();

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<FLOAT>(clientW);
    vp.Height = static_cast<FLOAT>(clientH);
    vp.MaxDepth = 1.0f;
    deviceContext->RSSetViewports(1, &vp);
    deviceContext->OMSetRenderTargets(1, &g_rtv, nullptr);
    const float clearColor[4] = {0.04f, 0.04f, 0.05f, 1.0f};
    deviceContext->ClearRenderTargetView(g_rtv, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swapChain->Present(1, 0);
}

} // namespace

namespace PreviewWindow
{
void UpdateAndRender(bool enabled, ID3D11Device* device, ID3D11DeviceContext* deviceContext)
{
    if (g_closeRequested)
    {
        g_closeRequested = false;
        if (g_hPreviewWnd && ::IsWindow(g_hPreviewWnd))
            ::ShowWindow(g_hPreviewWnd, SW_HIDE);
        {
            std::lock_guard<std::mutex> lock(configMutex);
            if (config.show_window)
            {
                config.show_window = false;
                OverlayConfig_MarkDirty();
            }
        }
    }

    if (!enabled)
    {
        if (g_hPreviewWnd && ::IsWindow(g_hPreviewWnd) && ::IsWindowVisible(g_hPreviewWnd))
            ::ShowWindow(g_hPreviewWnd, SW_HIDE);
        return;
    }

    if (!CreatePreviewResources(device, deviceContext))
        return;

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);
    RenderPreviewFrame(deviceContext);
    ImGui::SetCurrentContext(previous);
}

void Shutdown()
{
    if (g_ctx)
    {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_ctx);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_ctx);
        g_ctx = nullptr;
        g_imguiReady = false;
        ImGui::SetCurrentContext(previous);
    }

    CleanupPreviewResources();
    if (g_hPreviewWnd && ::IsWindow(g_hPreviewWnd))
    {
        SavePreviewRect(g_hPreviewWnd);
        ::DestroyWindow(g_hPreviewWnd);
    }
    g_hPreviewWnd = nullptr;
    ::UnregisterClassW(kPreviewClassName, ::GetModuleHandleW(nullptr));
}
} // namespace PreviewWindow
