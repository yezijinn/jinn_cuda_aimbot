#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <tchar.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <dcomp.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <chrono>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "overlay.h"
#include "overlay/ui_sections.h"
#include "overlay/draw_settings.h"
#include "overlay/config_dirty.h"
#include "overlay/preview_window.h"
#include "include/other_tools.h"
#include "config.h"
#include "keycodes.h"
#include "keyboard_listener.h"

#ifdef USE_CUDA
#include "trt_detector.h"
#endif

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3d11.lib")

ID3D11Device* g_pd3dDevice = NULL;
ID3D11DeviceContext* g_pd3dDeviceContext = NULL;
IDXGISwapChain1* g_pSwapChain = NULL;
IDCompositionDevice* g_dcompDevice = NULL;
IDCompositionTarget* g_dcompTarget = NULL;
IDCompositionVisual* g_dcompVisual = NULL;
ID3D11RenderTargetView* g_mainRenderTargetView = NULL;
HWND g_hwnd = NULL;
ImFont* g_debugBoldFont = nullptr;

extern Config config;
extern std::mutex configMutex;
extern std::atomic<bool> shouldExit;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
// 返回值改为 bool：GetBuffer/CreateRenderTargetView 均可能在设备丢失时失败，
// 调用方需要感知失败以触发设备重建，而不是继续使用空的 RTV。
bool CreateRenderTarget();
void CleanupRenderTarget();

ID3D11BlendState* g_pBlendState = nullptr;

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

const int BASE_OVERLAY_WIDTH = 760;
const int BASE_OVERLAY_HEIGHT = 480;
// 状态栏高度按当前字体行高动态计算，保证 "Jinn" / "TensorRT" 等文本完整显示、不被裁切
static float GetStatusBarHeight()
{
    return ImGui::GetTextLineHeightWithSpacing() + 4.0f;
}

int overlayWidth = 0;
int overlayHeight = 0;

static const int DRAG_BAR_HEIGHT_PX = 34;
static const int MIN_OVERLAY_W = 560;
static const int MIN_OVERLAY_H = 340;
static const int RESIZE_BORDER_PX = 8;
static const int WORKAREA_MARGIN_PX = 20;

static bool g_autoResizeEnabled = true;
static bool g_overlayVisible = false;
static bool g_renderingOverlayFrame = false;
// D3D11 设备丢失标记（TDR / 驱动更新 / 独显切换 / 远程桌面接管）。
// 仅由 overlay 线程读写，无需原子。置位后主循环会执行一次设备重建。
static bool g_deviceLost = false;

// RAII：保证 g_renderingOverlayFrame 在任何退出路径（含异常栈展开）都被复位。
// 原实现仅在函数末尾复位，一旦 UI 绘制抛异常，标志永久停在 true，
// RenderOverlayFrame 的重入守卫会让覆盖层永久黑屏。
struct OverlayFrameGuard
{
    OverlayFrameGuard() { g_renderingOverlayFrame = true; }
    ~OverlayFrameGuard() { g_renderingOverlayFrame = false; }
    OverlayFrameGuard(const OverlayFrameGuard&) = delete;
    OverlayFrameGuard& operator=(const OverlayFrameGuard&) = delete;
};

static bool g_manualWindowDragActive = false;
static WPARAM g_manualWindowDragHit = HTNOWHERE;
static POINT g_manualWindowDragStartPoint{};
static RECT g_manualWindowDragStartRect{};
static int g_activeOverlayTab = 0;
static bool g_pendingOverlayGeometryDirty = false;

std::vector<std::string> availableModels;
std::vector<std::string> key_names;
std::vector<const char*> key_names_cstrs;
std::vector<const char*> key_display_names_cstrs;

ID3D11ShaderResourceView* body_texture = nullptr;

static UINT GetDpiForWindowSafe(HWND hwnd);
static RECT GetOverlayWorkArea(HWND hwnd);
static void ClampOverlayToWorkArea(HWND hwnd, int& x, int& y, int& w, int& h);
static void EnsureOverlayInsideWorkArea(HWND hwnd, bool persistGeometry = false);
static bool StoreOverlayWindowGeometry(HWND hwnd, bool markDirty);
static bool ResizeOverlayBackBuffer(UINT width, UINT height);
static HRESULT RenderOverlayFrame(bool allowAutoResize, bool allowConfigSave);

std::vector<std::string> getAvailableModels();

static inline int ClampInt(int v, int lo, int hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

struct OverlayThreadConfigSnapshot
{
    std::vector<std::string> buttonOpenOverlay;
    bool excludeFromCapture = true;
};

static OverlayThreadConfigSnapshot SnapshotOverlayThreadConfig()
{
    std::lock_guard<std::mutex> lock(configMutex);
    OverlayThreadConfigSnapshot snapshot;
    snapshot.buttonOpenOverlay = config.button_open_overlay;
    snapshot.excludeFromCapture = config.overlay_exclude_from_capture;
    return snapshot;
}

static void TryAutoResizeOverlay(float extraContentWidth)
{
    IM_UNUSED(extraContentWidth);
    if (!g_hwnd || !g_autoResizeEnabled)
        return;

    // Keep the editor size stable. Long model names and combo popups should clip/scroll,
    // not grow the overlay frame across the screen.
}

static void Overlay_SetDisplayAffinity(HWND hwnd, bool excludeFromCapture)
{
    if (!hwnd)
        return;

    const DWORD wanted = excludeFromCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (SetWindowDisplayAffinity(hwnd, wanted))
        return;

    if (excludeFromCapture)
    {
        const DWORD err = GetLastError();
        std::cerr << "[覆盖层] SetWindowDisplayAffinity排除捕获失败, 错误=" << err
                  << "。尝试WDA_MONITOR回退方案。" << std::endl;
        if (!SetWindowDisplayAffinity(hwnd, WDA_MONITOR))
        {
            std::cerr << "[覆盖层] SetWindowDisplayAffinity显示器模式失败, 错误="
                      << GetLastError() << std::endl;
        }
    }
}

void Overlay_ApplyCaptureExclusion()
{
    Overlay_SetDisplayAffinity(g_hwnd, config.overlay_exclude_from_capture);
}

enum class SidebarIconKind
{
    Camera,
    Chip,
    Layers,
    Crosshair,
    Move,
    Curve,
    Spark,
    User,
    Mouse,
    Keyboard,
    Sliders,
    Monitor,
    Palette,
    Image,
    Bars,
    Debug
};

struct OverlayTabItem
{
    const char* label;
    const char* group;
    const char* description;
    void (*draw)();
    SidebarIconKind icon;
};

static void draw_hotkey_1()
{
    draw_hotkey_profile(0);
}

static void draw_hotkey_2()
{
    draw_hotkey_profile(1);
}

static void draw_hotkey_3()
{
    draw_hotkey_profile(2);
}

static void draw_program_config_page()
{
    draw_global_ai_settings();
    draw_ai();
    draw_buttons();
    draw_overlay();
}

static const OverlayTabItem kOverlayTabs[] = {
    { "画面", "采集与AI", "文件、文件夹、路径、模型名... 禁止使用中文。", draw_capture_and_model_settings, SidebarIconKind::Camera },
    { "键一", "热键配置", "第一个鼠标热键的局部参数。", draw_hotkey_1, SidebarIconKind::Mouse },
    { "键二", "热键配置", "第二个鼠标热键的局部参数。", draw_hotkey_2, SidebarIconKind::Mouse },
    { "键三", "热键配置", "第三个鼠标热键的局部参数。", draw_hotkey_3, SidebarIconKind::Mouse },
    { "全局", "", "全局参数，能影响其他菜单的功能", draw_program_config_page, SidebarIconKind::Sliders },
    { "鼠标", "", "鼠标输入后端、配置连接参数。", draw_mouse_input, SidebarIconKind::Mouse },
    { "调试", "监控工具", "截图、标注。", draw_debug, SidebarIconKind::Debug },
    { "性能", "监控工具", "捕获来源、性能统计、CUDA加速捕获的运行详情。", draw_performance_settings, SidebarIconKind::Bars },
};

static void DrawTitleBar(float width)
{
    ImGui::BeginChild("##overlay_title", ImVec2(width, 34.0f), ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const char* brandText = "软件名：咔蚯        版本号：1.0";
    const float textWidth = ImGui::CalcTextSize(brandText).x;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - textWidth) * 0.5f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(brandText);
    ImGui::EndChild();
}

static void DrawStatusBar(float width)
{
    const float h = GetStatusBarHeight();
    // 关闭 child 默认窗口内边距，改为按高度垂直居中，避免文本因 padding / 边框被裁切
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##overlay_status", ImVec2(width, h), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::SetCursorPosY((h - ImGui::GetTextLineHeight()) * 0.5f);
    if (ImGui::BeginTable("##overlay_status_table", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("status_left", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("status_right", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Jinn");
        ImGui::TableSetColumnIndex(1);
#ifdef USE_CUDA
        ImGui::TextDisabled("TensorRT");
#else
        ImGui::TextDisabled("CPU");
#endif
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

static UINT GetDpiForWindowSafe(HWND hwnd)
{
    UINT dpi = 96;
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto pGetDpiForWindow = (UINT(WINAPI*)(HWND))::GetProcAddress(user32, "GetDpiForWindow");
        if (pGetDpiForWindow)
            dpi = pGetDpiForWindow(hwnd);
    }
    return dpi;
}

static RECT GetOverlayWorkArea(HWND hwnd)
{
    RECT work{};
    HMONITOR monitor = nullptr;

    if (hwnd)
    {
        monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }
    else
    {
        POINT pt{};
        ::GetCursorPos(&pt);
        monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && ::GetMonitorInfo(monitor, &mi))
        return mi.rcWork;

    work.left = 0;
    work.top = 0;
    work.right = ::GetSystemMetrics(SM_CXSCREEN);
    work.bottom = ::GetSystemMetrics(SM_CYSCREEN);
    return work;
}

static RECT GetOverlayWorkAreaForRect(const RECT& rect)
{
    HMONITOR monitor = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && ::GetMonitorInfo(monitor, &mi))
        return mi.rcWork;

    return GetOverlayWorkArea(nullptr);
}

static void ClampOverlayToWorkArea(HWND hwnd, int& x, int& y, int& w, int& h)
{
    const RECT desiredRect = {
        x,
        y,
        x + OtherTools::MaxInt(1, w),
        y + OtherTools::MaxInt(1, h)
    };
    const RECT work = hwnd ? GetOverlayWorkArea(hwnd) : GetOverlayWorkAreaForRect(desiredRect);
    const UINT dpi = hwnd ? GetDpiForWindowSafe(hwnd) : 96;

    const int minW = ::MulDiv(MIN_OVERLAY_W, (int)dpi, 96);
    const int minH = ::MulDiv(MIN_OVERLAY_H, (int)dpi, 96);

    const int workW = OtherTools::MaxInt(1, static_cast<int>(work.right - work.left - WORKAREA_MARGIN_PX));
    const int workH = OtherTools::MaxInt(1, static_cast<int>(work.bottom - work.top - WORKAREA_MARGIN_PX));

    const int maxW = OtherTools::MaxInt(minW, workW);
    const int maxH = OtherTools::MaxInt(minH, workH);

    w = ClampInt(w, minW, maxW);
    h = ClampInt(h, minH, maxH);

    const int maxX = OtherTools::MaxInt(static_cast<int>(work.left), static_cast<int>(work.right - w));
    const int maxY = OtherTools::MaxInt(static_cast<int>(work.top), static_cast<int>(work.bottom - h));
    x = ClampInt(x, static_cast<int>(work.left), maxX);
    y = ClampInt(y, static_cast<int>(work.top), maxY);
}

static void MarkOverlayGeometryDirty()
{
    if (ImGui::GetCurrentContext())
    {
        OverlayConfig_MarkDirty();
    }
    else
    {
        g_pendingOverlayGeometryDirty = true;
    }
}

static bool StoreOverlayWindowGeometry(HWND hwnd, bool markDirty)
{
    if (!hwnd)
        return false;

    RECT wndRect{};
    if (!::GetWindowRect(hwnd, &wndRect))
        return false;

    const int x = wndRect.left;
    const int y = wndRect.top;
    const int w = wndRect.right - wndRect.left;
    const int h = wndRect.bottom - wndRect.top;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        changed = config.overlay_x != x ||
                  config.overlay_y != y ||
                  config.overlay_width != w ||
                  config.overlay_height != h;

        if (changed)
        {
            config.overlay_x = x;
            config.overlay_y = y;
            config.overlay_width = w;
            config.overlay_height = h;
        }
    }

    if (changed && markDirty)
        MarkOverlayGeometryDirty();

    return changed;
}

static void EnsureOverlayInsideWorkArea(HWND hwnd, bool persistGeometry)
{
    if (!hwnd)
        return;

    RECT wndRect{};
    ::GetWindowRect(hwnd, &wndRect);

    const int oldW = overlayWidth;
    const int oldH = overlayHeight;

    int x = wndRect.left;
    int y = wndRect.top;
    int w = overlayWidth;
    int h = overlayHeight;
    ClampOverlayToWorkArea(hwnd, x, y, w, h);

    overlayWidth = w;
    overlayHeight = h;

    if (x != wndRect.left || y != wndRect.top || w != oldW || h != oldH)
        ::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER);

    if (persistGeometry)
        StoreOverlayWindowGeometry(hwnd, true);
}

bool InitializeBlendState()
{
    D3D11_BLEND_DESC blendDesc;
    ZeroMemory(&blendDesc, sizeof(blendDesc));

    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr = g_pd3dDevice->CreateBlendState(&blendDesc, &g_pBlendState);
    if (FAILED(hr))
        return false;

    float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
    g_pd3dDeviceContext->OMSetBlendState(g_pBlendState, blendFactor, 0xffffffff);
    return true;
}

bool CreateDeviceD3D(HWND hWnd)
{
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        ARRAYSIZE(featureLevelArray),
        D3D11_SDK_VERSION,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext);

    if (FAILED(hr))
        return false;

    IDXGIDevice* dxgiDev = nullptr;
    hr = g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (FAILED(hr) || !dxgiDev)
        return false;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDev->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter)
    {
        dxgiDev->Release();
        return false;
    }

    IDXGIFactory2* factory2 = nullptr;
    {
        IDXGIFactory* baseFactory = nullptr;
        hr = adapter->GetParent(IID_PPV_ARGS(&baseFactory));
        if (FAILED(hr) || !baseFactory)
        {
            adapter->Release();
            dxgiDev->Release();
            return false;
        }
        hr = baseFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        baseFactory->Release();
    }

    if (FAILED(hr) || !factory2)
    {
        adapter->Release();
        dxgiDev->Release();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = overlayWidth;
    scd.Height = overlayHeight;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Scaling = DXGI_SCALING_STRETCH;

    hr = factory2->CreateSwapChainForComposition(
        g_pd3dDevice,
        &scd,
        nullptr,
        &g_pSwapChain);

    factory2->Release();
    adapter->Release();

    if (FAILED(hr) || !g_pSwapChain)
    {
        dxgiDev->Release();
        return false;
    }

    hr = DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&g_dcompDevice));
    dxgiDev->Release();
    if (FAILED(hr) || !g_dcompDevice)
        return false;

    hr = g_dcompDevice->CreateTargetForHwnd(hWnd, TRUE, &g_dcompTarget);
    if (FAILED(hr) || !g_dcompTarget)
        return false;

    hr = g_dcompDevice->CreateVisual(&g_dcompVisual);
    if (FAILED(hr) || !g_dcompVisual)
        return false;

    hr = g_dcompVisual->SetContent(g_pSwapChain);
    if (FAILED(hr))
        return false;

    hr = g_dcompTarget->SetRoot(g_dcompVisual);
    if (FAILED(hr))
        return false;

    g_dcompDevice->Commit();

    if (!InitializeBlendState())
        return false;

    if (!CreateRenderTarget())
        return false;
    return true;
}

bool CreateRenderTarget()
{
    if (!g_pSwapChain || !g_pd3dDevice)
        return false;

    ID3D11Texture2D* pBackBuffer = NULL;
    // 修复：原实现丢弃 GetBuffer 的 HRESULT，设备丢失/显存不足时 pBackBuffer 保持 NULL，
    // 随后的 pBackBuffer->Release() 是确定性的空指针解引用（进程崩溃）。
    HRESULT hr = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(hr) || pBackBuffer == NULL)
    {
        std::cerr << "[覆盖层] IDXGISwapChain::GetBuffer 失败, hr=0x"
                  << std::hex << static_cast<unsigned long>(hr) << std::dec << std::endl;
        return false;
    }

    hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr))
    {
        std::cerr << "[覆盖层] CreateRenderTargetView 失败, hr=0x"
                  << std::hex << static_cast<unsigned long>(hr) << std::dec << std::endl;
        g_mainRenderTargetView = NULL;
        return false;
    }
    return true;
}

void CleanupRenderTarget()
{
    if (g_pd3dDeviceContext)
    {
        ID3D11RenderTargetView* nullRenderTarget = nullptr;
        g_pd3dDeviceContext->OMSetRenderTargets(1, &nullRenderTarget, NULL);
    }
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

static bool ResizeOverlayBackBuffer(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    overlayWidth = static_cast<int>(width);
    overlayHeight = static_cast<int>(height);

    if (!g_pd3dDevice || !g_pSwapChain)
        return true;

    CleanupRenderTarget();
    const HRESULT hr = g_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
    {
        // 设备丢失时 ResizeBuffers 恒失败，标记待重建避免后续每帧空转。
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            g_deviceLost = true;
        return false;
    }

    if (!CreateRenderTarget())
    {
        g_deviceLost = true;
        return false;
    }
    if (g_dcompDevice)
        g_dcompDevice->Commit();

    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();

    if (g_dcompVisual) { g_dcompVisual->Release(); g_dcompVisual = NULL; }
    if (g_dcompTarget) { g_dcompTarget->Release(); g_dcompTarget = NULL; }
    if (g_dcompDevice) { g_dcompDevice->Release(); g_dcompDevice = NULL; }

    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
    if (g_pBlendState) { g_pBlendState->Release(); g_pBlendState = nullptr; }
}

// 设备丢失后的重建：释放全部僵尸 COM 对象并重新创建 D3D11 设备 + 交换链 + DComp，
// 同时重建 ImGui DX11 后端（ImGui 上下文与字体图集保留，无需重新加载字体）。
// 带重试上限，避免在硬件永久性故障时形成高频空转。
static bool TryRecoverDeviceD3D()
{
    static int recoverAttempts = 0;
    constexpr int kMaxRecoverAttempts = 10;

    if (recoverAttempts >= kMaxRecoverAttempts)
        return false;

    ++recoverAttempts;

    ImGui_ImplDX11_Shutdown();
    CleanupDeviceD3D();

    if (!g_hwnd || !CreateDeviceD3D(g_hwnd))
    {
        CleanupDeviceD3D();
        std::cerr << "[覆盖层] D3D11 设备重建失败 (" << recoverAttempts << "/"
                  << kMaxRecoverAttempts << ")" << std::endl;
        return false;
    }

    if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext))
    {
        std::cerr << "[覆盖层] 设备重建后 ImGui_ImplDX11_Init 失败" << std::endl;
        return false;
    }

    Overlay_ApplyCaptureExclusion();
    g_deviceLost = false;
    recoverAttempts = 0;
    std::cout << "[覆盖层] D3D11 设备已重建。" << std::endl;
    return true;
}

static bool IsOverlayResizeHit(WPARAM hit)
{
    return hit == HTLEFT || hit == HTRIGHT || hit == HTTOP || hit == HTBOTTOM ||
           hit == HTTOPLEFT || hit == HTTOPRIGHT || hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT;
}

static bool IsOverlayMoveOrResizeHit(WPARAM hit)
{
    return hit == HTCAPTION || IsOverlayResizeHit(hit);
}

static void BeginManualOverlayWindowDrag(HWND hwnd, WPARAM hit)
{
    if (!hwnd || !IsOverlayMoveOrResizeHit(hit))
        return;

    g_manualWindowDragActive = true;
    g_manualWindowDragHit = hit;
    ::GetCursorPos(&g_manualWindowDragStartPoint);
    ::GetWindowRect(hwnd, &g_manualWindowDragStartRect);
    ::SetCapture(hwnd);

    if (IsOverlayResizeHit(hit))
        g_autoResizeEnabled = false;
}

static void EndManualOverlayWindowDrag(HWND hwnd)
{
    if (!g_manualWindowDragActive)
        return;

    g_manualWindowDragActive = false;
    g_manualWindowDragHit = HTNOWHERE;

    if (::GetCapture() == hwnd)
        ::ReleaseCapture();

    EnsureOverlayInsideWorkArea(hwnd, true);
}

static void UpdateManualOverlayWindowDrag(HWND hwnd)
{
    if (!hwnd || !g_manualWindowDragActive)
        return;

    POINT pt{};
    ::GetCursorPos(&pt);

    const int dx = pt.x - g_manualWindowDragStartPoint.x;
    const int dy = pt.y - g_manualWindowDragStartPoint.y;
    const int startW = g_manualWindowDragStartRect.right - g_manualWindowDragStartRect.left;
    const int startH = g_manualWindowDragStartRect.bottom - g_manualWindowDragStartRect.top;

    int x = g_manualWindowDragStartRect.left;
    int y = g_manualWindowDragStartRect.top;
    int w = startW;
    int h = startH;

    if (g_manualWindowDragHit == HTCAPTION)
    {
        x += dx;
        y += dy;
        ClampOverlayToWorkArea(hwnd, x, y, w, h);
    }
    else
    {
        const UINT dpi = GetDpiForWindowSafe(hwnd);
        const int minW = ::MulDiv(MIN_OVERLAY_W, (int)dpi, 96);
        const int minH = ::MulDiv(MIN_OVERLAY_H, (int)dpi, 96);
        const RECT work = GetOverlayWorkArea(hwnd);
        const int maxW = OtherTools::MaxInt(minW, static_cast<int>((work.right - work.left) - WORKAREA_MARGIN_PX));
        const int maxH = OtherTools::MaxInt(minH, static_cast<int>((work.bottom - work.top) - WORKAREA_MARGIN_PX));

        const bool left = g_manualWindowDragHit == HTLEFT ||
                          g_manualWindowDragHit == HTTOPLEFT ||
                          g_manualWindowDragHit == HTBOTTOMLEFT;
        const bool right = g_manualWindowDragHit == HTRIGHT ||
                           g_manualWindowDragHit == HTTOPRIGHT ||
                           g_manualWindowDragHit == HTBOTTOMRIGHT;
        const bool top = g_manualWindowDragHit == HTTOP ||
                         g_manualWindowDragHit == HTTOPLEFT ||
                         g_manualWindowDragHit == HTTOPRIGHT;
        const bool bottom = g_manualWindowDragHit == HTBOTTOM ||
                            g_manualWindowDragHit == HTBOTTOMLEFT ||
                            g_manualWindowDragHit == HTBOTTOMRIGHT;

        if (left)
        {
            w = ClampInt(startW - dx, minW, maxW);
            x = g_manualWindowDragStartRect.right - w;
        }
        else if (right)
        {
            w = ClampInt(startW + dx, minW, maxW);
        }

        if (top)
        {
            h = ClampInt(startH - dy, minH, maxH);
            y = g_manualWindowDragStartRect.bottom - h;
        }
        else if (bottom)
        {
            h = ClampInt(startH + dy, minH, maxH);
        }

        ClampOverlayToWorkArea(hwnd, x, y, w, h);
    }

    ::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_NCHITTEST:
        {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            ::ScreenToClient(hWnd, &pt);

            RECT rc;
            ::GetClientRect(hWnd, &rc);

            const UINT dpi = GetDpiForWindowSafe(hWnd);
            const int border = ::MulDiv(RESIZE_BORDER_PX, (int)dpi, 96);
            const bool left = pt.x < rc.left + border;
            const bool right = pt.x >= rc.right - border;
            const bool top = pt.y < rc.top + border;
            const bool bottom = pt.y >= rc.bottom - border;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;

            if (pt.y >= rc.top && pt.y < rc.top + DRAG_BAR_HEIGHT_PX)
                return HTCAPTION;

            return HTCLIENT;
        }
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindowSafe(hWnd);
            const int minW = ::MulDiv(MIN_OVERLAY_W, (int)dpi, 96);
            const int minH = ::MulDiv(MIN_OVERLAY_H, (int)dpi, 96);
            const RECT work = GetOverlayWorkArea(hWnd);
            const int maxW = OtherTools::MaxInt(minW, static_cast<int>((work.right - work.left) - WORKAREA_MARGIN_PX));
            const int maxH = OtherTools::MaxInt(minH, static_cast<int>((work.bottom - work.top) - WORKAREA_MARGIN_PX));
            mmi->ptMinTrackSize.x = minW;
            mmi->ptMinTrackSize.y = minH;
            if (maxW > 0) mmi->ptMaxTrackSize.x = maxW;
            if (maxH > 0) mmi->ptMaxTrackSize.y = maxH;
            return 0;
        }
        case WM_NCLBUTTONDOWN:
            if (IsOverlayMoveOrResizeHit(wParam))
            {
                BeginManualOverlayWindowDrag(hWnd, wParam);
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (g_manualWindowDragActive)
            {
                UpdateManualOverlayWindowDrag(hWnd);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
        case WM_NCLBUTTONUP:
            if (g_manualWindowDragActive)
            {
                EndManualOverlayWindowDrag(hWnd);
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            if (g_manualWindowDragActive)
            {
                g_manualWindowDragActive = false;
                g_manualWindowDragHit = HTNOWHERE;
                EnsureOverlayInsideWorkArea(hWnd, true);
                return 0;
            }
            break;
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_EXITSIZEMOVE:
        g_autoResizeEnabled = false;
        EnsureOverlayInsideWorkArea(hWnd, true);
        return 0;

    case WM_DISPLAYCHANGE:
        EnsureOverlayInsideWorkArea(hWnd, true);
        return 0;

    case WM_DPICHANGED:
        EnsureOverlayInsideWorkArea(hWnd, true);
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            const UINT width = (UINT)LOWORD(lParam);
            const UINT height = (UINT)HIWORD(lParam);

            if (ResizeOverlayBackBuffer(width, height) && g_overlayVisible)
                RenderOverlayFrame(false, false);
        }
        return 0;

    case WM_DESTROY:
        shouldExit = true;
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

void SetupImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImFontConfig fontConfig{};
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 2;
    fontConfig.PixelSnapH = true;

    static const ImWchar chineseRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x2000, 0x206F, // General Punctuation
        0x3000, 0x30FF, // CJK Symbols and Punctuations, Hiragana, Katakana
        0x31F0, 0x31FF, // Katakana Phonetic Extensions
        0xFF00, 0xFFEF, // Half-width characters
        0x4E00, 0x9FAF, // CJK Ideograms
        0,
    };

    bool fontLoaded = false;
    if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 16.5f, &fontConfig, chineseRanges))
        fontLoaded = true;
    else if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyhbd.ttc", 16.5f, &fontConfig, chineseRanges))
        fontLoaded = true;
    else if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", 16.5f, &fontConfig, chineseRanges))
        fontLoaded = true;
    else if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simsun.ttc", 16.5f, &fontConfig, chineseRanges))
        fontLoaded = true;

    if (!fontLoaded)
    {
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 16.5f, &fontConfig) &&
            !io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\SegUIVar.ttf", 16.5f, &fontConfig) &&
            !io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.5f, &fontConfig))
        {
            io.Fonts->AddFontDefault();
        }
    }

    ImFontConfig boldFontConfig = fontConfig;
    g_debugBoldFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\msyhbd.ttc", 16.5f, &boldFontConfig, chineseRanges);

    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // 修复：原实现丢弃两个后端 Init 的返回值，失败时后续 NewFrame 会触发断言/UB。
    if (!ImGui_ImplWin32_Init(g_hwnd))
        std::cerr << "[覆盖层] ImGui_ImplWin32_Init 失败" << std::endl;
    if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext))
        std::cerr << "[覆盖层] ImGui_ImplDX11_Init 失败" << std::endl;

    if (g_pendingOverlayGeometryDirty)
    {
        g_pendingOverlayGeometryDirty = false;
        OverlayConfig_MarkDirty();
    }
}

bool CreateOverlayWindow()
{
    // 修复：原实现无锁读取 config 的 4 个几何字段，与同文件 StoreOverlayWindowGeometry()
    // 的加锁写入策略自相矛盾。此处不持有任何锁，加短锁快照是安全的。
    int overlayX = 0;
    int overlayY = 0;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        overlayX = config.overlay_x;
        overlayY = config.overlay_y;
        overlayWidth = config.overlay_width > 0 ? config.overlay_width : BASE_OVERLAY_WIDTH;
        overlayHeight = config.overlay_height > 0 ? config.overlay_height : BASE_OVERLAY_HEIGHT;
    }

    {
        int x = overlayX;
        int y = overlayY;
        int w = overlayWidth;
        int h = overlayHeight;
        ClampOverlayToWorkArea(nullptr, x, y, w, h);
        overlayX = x;
        overlayY = y;
        overlayWidth = w;
        overlayHeight = h;
    }

    WNDCLASSEX wc = {
        sizeof(WNDCLASSEX),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        GetModuleHandle(NULL),
        NULL,
        NULL,
        NULL,
        NULL,
        _T("Chrome"),
        NULL
    };
    if (::RegisterClassEx(&wc) == 0)
    {
        const DWORD err = ::GetLastError();
        // ERROR_CLASS_ALREADY_EXISTS 属可恢复情形（重建窗口时类仍在注册表中）。
        if (err != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::cerr << "[覆盖层] RegisterClassEx 失败, 错误=" << err << std::endl;
            return false;
        }
    }

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    const DWORD style = WS_POPUP;

    RECT wr = { overlayX, overlayY, overlayX + overlayWidth, overlayY + overlayHeight };
    ::AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    const int wndW = wr.right - wr.left;
    const int wndH = wr.bottom - wr.top;

    g_hwnd = ::CreateWindowEx(
        exStyle,
        wc.lpszClassName, _T("Chrome"),
        style,
        wr.left, wr.top, wndW, wndH,
        NULL, NULL, wc.hInstance, NULL);

    if (g_hwnd == NULL)
    {
        // 修复：原实现直接 return，已注册的窗口类未注销（与下方 D3D 失败分支不一致）。
        std::cerr << "[覆盖层] CreateWindowEx 失败, 错误=" << ::GetLastError() << std::endl;
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        return false;
    }

    EnsureOverlayInsideWorkArea(g_hwnd, true);

    BOOL dwm = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&dwm)) && dwm)
    {
        MARGINS m = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
    }

    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);

    if (!CreateDeviceD3D(g_hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        return false;
    }

    Overlay_ApplyCaptureExclusion();

    return true;
}

static HRESULT RenderOverlayFrame(bool allowAutoResize, bool allowConfigSave)
{
    if (!g_overlayVisible || !g_pSwapChain || !g_pd3dDeviceContext || !g_mainRenderTargetView ||
        !ImGui::GetCurrentContext() || g_renderingOverlayFrame)
    {
        return S_FALSE;
    }

    const float w = static_cast<float>(overlayWidth);
    const float h = static_cast<float>(overlayHeight);
    if (w <= 0.0f || h <= 0.0f)
        return S_FALSE;

    OverlayFrameGuard frameGuard;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::Begin("##editor_root", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);
    {
        std::lock_guard<std::mutex> lock(configMutex);

        const int tabCount = static_cast<int>(sizeof(kOverlayTabs) / sizeof(kOverlayTabs[0]));
        if (g_activeOverlayTab < 0 || g_activeOverlayTab >= tabCount)
            g_activeOverlayTab = 0;

        // 顶部标题栏
        const float contentW = ImGui::GetContentRegionAvail().x;
        DrawTitleBar(contentW);
        ImGui::Separator();
        if (ImGui::BeginTabBar("##overlay_tabs", ImGuiTabBarFlags_FittingPolicyScroll))
        {
            for (int i = 0; i < tabCount; ++i)
            {
                if (ImGui::BeginTabItem(kOverlayTabs[i].label))
                {
                    g_activeOverlayTab = i;
                    ImGui::EndTabItem();
                }
                if (ImGui::IsItemClicked())
                {
                    g_activeOverlayTab = i;
                }
            }
            ImGui::EndTabBar();
        }
        float contentExtraW = 0.0f;

        const float contentHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - GetStatusBarHeight() - ImGui::GetStyle().ItemSpacing.y);
        ImGui::BeginChild("##options_content", ImVec2(contentW, contentHeight),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);

        ImGui::TextUnformatted(kOverlayTabs[g_activeOverlayTab].label);
        ImGui::TextDisabled("%s", kOverlayTabs[g_activeOverlayTab].description);
        ImGui::Separator();

        // 渲染页面内容
        kOverlayTabs[g_activeOverlayTab].draw();

        contentExtraW = ImGui::GetScrollMaxX();
        ImGui::EndChild();

        DrawStatusBar(contentW);

        if (allowAutoResize)
            TryAutoResizeOverlay(contentExtraW);

        if (allowConfigSave)
            OverlayConfig_TrySave();
    }

    ImGui::End();
    ImGui::Render();

    const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    const HRESULT result = g_pSwapChain->Present(0, 0);
    // frameGuard 析构负责复位 g_renderingOverlayFrame
    return result;
}

void OverlayThread()
{
    if (!CreateOverlayWindow())
    {
        std::cout << "[覆盖层] 无法创建覆盖层窗口！" << std::endl;
        // 修复：原实现直接 return 而不置 shouldExit，导致程序进入
        // 「采集/推理/鼠标全开、但用户完全看不到也操作不了面板」的失控状态，
        // 热键无响应，只能强杀进程。此处主动请求整体退出。
        shouldExit = true;
        return;
    }

    SetupImGui();

    bool show_overlay = false;

    for (const auto& pair : KeyCodes::key_code_map)
        key_names.push_back(pair.first);

    std::sort(key_names.begin(), key_names.end());
    key_names_cstrs.reserve(key_names.size());
    key_display_names_cstrs.reserve(key_names.size());
    for (const auto& name : key_names)
    {
        key_names_cstrs.push_back(name.c_str());
        auto it = KeyCodes::key_display_names.find(name);
        if (it != KeyCodes::key_display_names.end())
            key_display_names_cstrs.push_back(it->second.c_str());
        else
            key_display_names_cstrs.push_back(name.c_str());
    }

    // 移除：全局 availableModels 写入后全工程无任何读取点（死变量），
    // 这次调用只带来一次无锁读 config.backend（std::string 数据竞争 UB）
    // 与两次 models/ 目录枚举的启动开销。模型列表由 UI 侧按需获取。

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    OverlayThreadConfigSnapshot overlayCfg = SnapshotOverlayThreadConfig();
    bool lastExcludeFromCapture = overlayCfg.excludeFromCapture;
    bool overlayHotkeyWasDown = false;
    Overlay_SetDisplayAffinity(g_hwnd, lastExcludeFromCapture);

    while (!shouldExit)
    {
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
            {
                shouldExit = true;
                break;
            }
        }
        if (shouldExit) break;

        overlayCfg = SnapshotOverlayThreadConfig();

        if (lastExcludeFromCapture != overlayCfg.excludeFromCapture)
        {
            lastExcludeFromCapture = overlayCfg.excludeFromCapture;
            Overlay_SetDisplayAffinity(g_hwnd, lastExcludeFromCapture);
        }

        const bool overlayHotkeyDown = isAnyKeyPressedWin32Only(overlayCfg.buttonOpenOverlay);
        if (overlayHotkeyDown && !overlayHotkeyWasDown)
        {
            show_overlay = !show_overlay;
            g_overlayVisible = show_overlay;

            if (show_overlay)
            {
                g_autoResizeEnabled = true;
                EnsureOverlayInsideWorkArea(g_hwnd, true);
                ShowWindow(g_hwnd, SW_SHOW);
                SetForegroundWindow(g_hwnd);
            }
            else
            {
                StoreOverlayWindowGeometry(g_hwnd, true);
                {
                    std::lock_guard<std::mutex> lock(configMutex);
                    OverlayConfig_SaveNow();
                }
                ShowWindow(g_hwnd, SW_HIDE);
            }
        }
        overlayHotkeyWasDown = overlayHotkeyDown;

        bool previewEnabled = false;
        {
            std::lock_guard<std::mutex> lock(configMutex);
            previewEnabled = config.show_window;
        }
        PreviewWindow::UpdateAndRender(previewEnabled, g_pd3dDevice, g_pd3dDeviceContext);

        if (!show_overlay)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 设备丢失恢复：原实现只处理 OCCLUDED / ACCESS_LOST 两个最轻状态，
        // DEVICE_REMOVED / DEVICE_RESET（TDR、驱动更新、独显切换、远程桌面接管）
        // 完全没有分支 —— 设备一旦丢失，全局 COM 指针变成僵尸对象，
        // Present 恒失败、GetBuffer 恒失败，覆盖层永久黑屏且无任何日志。
        if (g_deviceLost)
        {
            if (!TryRecoverDeviceD3D())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }

        HRESULT result = RenderOverlayFrame(true, true);
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
        {
            const HRESULT reason = g_pd3dDevice ? g_pd3dDevice->GetDeviceRemovedReason() : E_FAIL;
            std::cerr << "[覆盖层] D3D11 设备丢失, hr=0x" << std::hex
                      << static_cast<unsigned long>(result) << ", reason=0x"
                      << static_cast<unsigned long>(reason) << std::dec
                      << "，将尝试重建。" << std::endl;
            g_deviceLost = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (result == DXGI_STATUS_OCCLUDED || result == DXGI_ERROR_ACCESS_LOST)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    {
        StoreOverlayWindowGeometry(g_hwnd, true);
        std::lock_guard<std::mutex> lock(configMutex);
        OverlayConfig_SaveNow();
    }

    PreviewWindow::Shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClass(_T("Chrome"), GetModuleHandle(NULL));
}

int APIENTRY _tWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPTSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    std::thread overlay(OverlayThread);
    overlay.join();
    return 0;
}
