#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "winrt_capture.h"
#include "mybot.h"
#include "other_tools.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace
{
uint64_t ElapsedMicros(std::chrono::steady_clock::time_point start,
                       std::chrono::steady_clock::time_point end)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}
}

winrt::com_ptr<IGraphicsCaptureItemInterop> GetInteropFactory()
{
    static winrt::com_ptr<IGraphicsCaptureItemInterop> s_factory = [] {
        auto factory = winrt::get_activation_factory<
            GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        return factory.as<IGraphicsCaptureItemInterop>();
    }();
    return s_factory;
}

HWND WinRTScreenCapture::FindWindowByTitleSubstring(const std::string& title_substr)
{
    return FindCaptureWindowByTitle(title_substr);
}

WinRTScreenCapture::WinRTScreenCapture(int desiredWidth, int desiredHeight, const Options& options)
    : desiredRegionWidth(desiredWidth)
    , desiredRegionHeight(desiredHeight)
    , regionWidth(desiredWidth)
    , regionHeight(desiredHeight)
{
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    // Step 1: Create D3D11 device
    try
    {
        winrt::check_hresult(
            D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                0,
                createDeviceFlags,
                featureLevels,
                ARRAYSIZE(featureLevels),
                D3D11_SDK_VERSION,
                d3dDevice.put(),
                nullptr,
                d3dContext.put()
            )
        );
    }
    catch (const winrt::hresult_error& e)
    {
        throw std::runtime_error(
            std::string("[WinRTCapture] D3D11CreateDevice failed: ") + winrt::to_string(e.message()) + " (0x" +
            std::to_string(static_cast<unsigned>(e.code())) + ")");
    }

    // Step 2: Get DXGI device and create WinRT Direct3D device
    try
    {
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));
        winrt::com_ptr<IDXGIDevice1> dxgiDevice1;
        if (SUCCEEDED(dxgiDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice1.put()))))
        {
            dxgiDevice1->SetMaximumFrameLatency(1);
        }

        device = CreateDirect3DDevice(dxgiDevice.get());
        if (!device)
        {
            throw std::runtime_error("[WinRTCapture] Failed to create IDirect3DDevice.");
        }
    }
    catch (const winrt::hresult_error& e)
    {
        throw std::runtime_error(
            std::string("[WinRTCapture] CreateDirect3DDevice failed: ") + winrt::to_string(e.message()) + " (0x" +
            std::to_string(static_cast<unsigned>(e.code())) + ")");
    }

    // Step 3: Get capture item (monitor or window)
    try
    {
        if (options.target == "window")
        {
            if (options.windowTitle.empty())
            {
                throw std::runtime_error("[WinRTCapture] capture_target=window but capture_window_title is empty.");
            }
            HWND hwnd = FindWindowByTitleSubstring(options.windowTitle);
            if (!hwnd)
            {
                throw std::runtime_error("[WinRTCapture] Target window not found by title substring: " + options.windowTitle);
            }
            captureItem = CreateCaptureItemForWindow(hwnd);
        }
        else
        {
            HMONITOR hMonitor = GetMonitorHandleByIndex(options.monitorIndex);
            if (!hMonitor)
            {
                throw std::runtime_error("[WinRTCapture] Invalid monitor index in config.");
            }
            captureItem = CreateCaptureItemForMonitor(hMonitor);
        }
        if (!captureItem)
        {
            throw std::runtime_error("[WinRTCapture] CreateCaptureItem returned null.");
        }
    }
    catch (const winrt::hresult_error& e)
    {
        throw std::runtime_error(
            std::string("[WinRTCapture] CreateCaptureItem failed: ") + winrt::to_string(e.message()) + " (0x" +
            std::to_string(static_cast<unsigned>(e.code())) + ")");
    }

    // Step 4: Query capture item size and set up regions
    try
    {
        screenWidth = captureItem.Size().Width;
        screenHeight = captureItem.Size().Height;
    }
    catch (const winrt::hresult_error& e)
    {
        throw std::runtime_error(
            std::string("[WinRTCapture] CaptureItem.Size() failed: ") + winrt::to_string(e.message()) + " (0x" +
            std::to_string(static_cast<unsigned>(e.code())) + ")");
    }

    SetSourceDimensions(screenWidth, screenHeight);
    desiredRegionWidth = std::max(1, desiredRegionWidth);
    desiredRegionHeight = std::max(1, desiredRegionHeight);
    regionWidth = std::clamp(desiredRegionWidth, 1, std::max(1, screenWidth));
    regionHeight = std::clamp(desiredRegionHeight, 1, std::max(1, screenHeight));

    regionX = (screenWidth - regionWidth) / 2;
    regionY = (screenHeight - regionHeight) / 2;

    // Step 5: Create frame pool
    try
    {
        framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            3,
            captureItem.Size()
        );
    }
    catch (const winrt::hresult_error& e)
    {
        throw std::runtime_error(
            std::string("[WinRTCapture] CreateFramePool failed: ") + winrt::to_string(e.message()) + " (0x" +
            std::to_string(static_cast<unsigned>(e.code())) + ")");
    }

    // Step 6: Create capture session and configure
    try
    {
        session = framePool.CreateCaptureSession(captureItem);
    }
    catch (const winrt::hresult_error& e)
    {
        throw std::runtime_error(
            std::string("[WinRTCapture] CreateCaptureSession failed: ") + winrt::to_string(e.message()) + " (0x" +
            std::to_string(static_cast<unsigned>(e.code())) + ")");
    }

    try
    {
        if (auto session5 = session.try_as<IGraphicsCaptureSession5>())
        {
            session5.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ 0 });
        }

        if (!options.captureBorders)
        {
            try { session.IsBorderRequired(false); }
            catch (const winrt::hresult_error&) { /* 接口不在此 Windows 版本上可用，跳过 */ }
        }

        if (!options.captureCursor)
        {
            try { session.IsCursorCaptureEnabled(false); }
            catch (const winrt::hresult_error&) { /* 接口不在此 Windows 版本上可用，跳过 */ }
        }
    }
    catch (const winrt::hresult_error& e)
    {
        throw std::runtime_error(
            std::string("[WinRTCapture] Session config failed: ") + winrt::to_string(e.message()) + " (0x" +
            std::to_string(static_cast<unsigned>(e.code())) + ")");
    }

    // Step 7: Create staging texture
    if (!createStagingTextureCPU())
    {
        throw std::runtime_error("[WinRTCapture] createStagingTextureCPU() failed.");
    }

    // Step 8: Start capture
    try
    {
        session.StartCapture();
    }
    catch (const winrt::hresult_error& e)
    {
        throw std::runtime_error(
            std::string("[WinRTCapture] StartCapture failed: ") + winrt::to_string(e.message()) + " (0x" +
            std::to_string(static_cast<unsigned>(e.code())) + ")");
    }
}

WinRTScreenCapture::~WinRTScreenCapture()
{
    if (session)
        session.Close();
    if (framePool)
        framePool.Close();

    stagingTextureCPU = nullptr;
    sharedTexture = nullptr;
    d3dContext = nullptr;
    d3dDevice = nullptr;
}

bool WinRTScreenCapture::createStagingTextureCPU()
{
    stagingTextureCPU = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = regionWidth;
    desc.Height = regionHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, stagingTextureCPU.put());
    if (FAILED(hr))
    {
        std::cerr << "[WinRTCapture] CreateTexture2D(staging) failed hr=" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}

cv::Mat WinRTScreenCapture::GetNextFrameCpu()
{
    if (!framePool || !stagingTextureCPU)
        return cv::Mat();

    captureWinrtPollAttemptsTotal.fetch_add(1, std::memory_order_relaxed);

    // 排空帧池，只保留最新一帧。
    // WGC 的 Direct3D11CaptureFramePool 是 FIFO：TryGetNextFrame 返回的是最旧的未取帧。
    // 若每轮只取一帧，而采集循环因推理耗时而慢于合成器出帧速率，帧池（3 缓冲）会持续积压，
    // 使送入检测器的画面最多滞后 2 帧（60Hz 下约 33ms），直接抬高端到端瞄准延迟。
    // 中间的过期帧必须显式 Close()，以立即把表面缓冲归还给帧池，避免缓冲耗尽后取不到新帧。
    Direct3D11CaptureFrame lastFrame = framePool.TryGetNextFrame();
    if (!lastFrame)
    {
        captureWinrtEmptyPollsTotal.fetch_add(1, std::memory_order_relaxed);
        return cv::Mat();
    }
    captureWinrtFramesDrainedTotal.fetch_add(1, std::memory_order_relaxed);

    // RAII 守卫：绑定到 lastFrame 变量本身（而非某一帧的值），
    // 确保排空循环及后续任何返回/异常路径都会把当前持有帧的表面缓冲归还帧池。
    struct FrameCloser
    {
        Direct3D11CaptureFrame& frame;
        ~FrameCloser() { if (frame) frame.Close(); }
    } frameCloser{ lastFrame };

    while (auto newerFrame = framePool.TryGetNextFrame())
    {
        lastFrame.Close();
        lastFrame = newerFrame;
        captureWinrtFramesDrainedTotal.fetch_add(1, std::memory_order_relaxed);
    }

    const auto contentSize = lastFrame.ContentSize();
    if (contentSize.Width > 0 && contentSize.Height > 0 &&
        (contentSize.Width != screenWidth || contentSize.Height != screenHeight))
    {
        screenWidth = contentSize.Width;
        screenHeight = contentSize.Height;
        SetSourceDimensions(screenWidth, screenHeight);

        regionWidth = std::clamp(desiredRegionWidth, 1, std::max(1, screenWidth));
        regionHeight = std::clamp(desiredRegionHeight, 1, std::max(1, screenHeight));
        regionX = (screenWidth - regionWidth) / 2;
        regionY = (screenHeight - regionHeight) / 2;

        // Recreate() 会释放帧池原有的全部表面缓冲，当前 lastFrame 持有的表面随之失效。
        // 必须在重建前归还并置空该帧，且本轮不得再访问其 Surface()，否则属于访问已释放资源。
        lastFrame.Close();
        lastFrame = nullptr;

        framePool.Recreate(
            device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            3,
            contentSize
        );

        // 重建暂存纹理；失败时内部已输出错误，函数入口的空指针检查会让后续轮次安全返回空帧。
        (void)createStagingTextureCPU();

        // 本轮放弃出帧，下一轮从重建后的帧池取新尺寸的帧。
        return cv::Mat();
    }

    auto frameSurface = lastFrame.Surface();
    auto frameTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frameSurface);
    if (!frameTexture)
        return cv::Mat();

    D3D11_BOX sourceRegion;
    sourceRegion.left = regionX;
    sourceRegion.top = regionY;
    sourceRegion.front = 0;
    sourceRegion.right = regionX + regionWidth;
    sourceRegion.bottom = regionY + regionHeight;
    sourceRegion.back = 1;

    const auto readbackStart = std::chrono::steady_clock::now();
    d3dContext->CopySubresourceRegion(
        stagingTextureCPU.get(),
        0,
        0, 0, 0,
        frameTexture.get(),
        0,
        &sourceRegion
    );

    D3D11_MAPPED_SUBRESOURCE mapped;
    const auto mapStart = std::chrono::steady_clock::now();
    HRESULT hrMap = d3dContext->Map(stagingTextureCPU.get(), 0, D3D11_MAP_READ, 0, &mapped);
    const auto mapEnd = std::chrono::steady_clock::now();
    captureWinrtMapMicrosTotal.fetch_add(ElapsedMicros(mapStart, mapEnd), std::memory_order_relaxed);
    if (FAILED(hrMap))
    {
        std::cerr << "[WinRTCapture] Map stagingTextureCPU failed hr=" << std::hex << hrMap << std::endl;
        if (hrMap == DXGI_ERROR_DEVICE_REMOVED || hrMap == DXGI_ERROR_DEVICE_RESET)
            capture_method_changed.store(true);
        return cv::Mat();
    }

    // Map 成功后必须 Unmap。cv::Mat 构造或后续拷贝若抛异常，直接返回会让
    // staging texture 永久处于 mapped 状态，后续采集全部失败。
    struct MappedResourceUnmapGuard
    {
        ID3D11DeviceContext* context;
        ID3D11Texture2D* texture;

        ~MappedResourceUnmapGuard()
        {
            if (context && texture)
                context->Unmap(texture, 0);
        }
    } unmapGuard{ d3dContext.get(), stagingTextureCPU.get() };

    cv::Mat cpuFrame(regionHeight, regionWidth, CV_8UC4);
    const auto pixelCopyStart = std::chrono::steady_clock::now();
    for (int y = 0; y < regionHeight; y++)
    {
        unsigned char* dstRow = cpuFrame.ptr<unsigned char>(y);
        unsigned char* srcRow = (unsigned char*)mapped.pData + y * mapped.RowPitch;
        memcpy(dstRow, srcRow, regionWidth * 4);
    }
    const auto pixelCopyEnd = std::chrono::steady_clock::now();
    d3dContext->Unmap(stagingTextureCPU.get(), 0);
    unmapGuard.texture = nullptr;
    const auto readbackEnd = std::chrono::steady_clock::now();

    captureWinrtPixelCopyMicrosTotal.fetch_add(
        ElapsedMicros(pixelCopyStart, pixelCopyEnd),
        std::memory_order_relaxed);
    captureWinrtReadbackMicrosTotal.fetch_add(
        ElapsedMicros(readbackStart, readbackEnd),
        std::memory_order_relaxed);
    captureWinrtFramesReturnedTotal.fetch_add(1, std::memory_order_relaxed);

    return cpuFrame;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem
WinRTScreenCapture::CreateCaptureItemForMonitor(HMONITOR hMonitor)
{
    auto interopFactory = GetInteropFactory();
    GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interopFactory->CreateForMonitor(
        hMonitor,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );
    if (FAILED(hr))
    {
        throw std::runtime_error("[WinRTCapture] CreateForMonitor failed, HR=" + std::to_string(hr));
    }
    return item;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem
WinRTScreenCapture::CreateCaptureItemForWindow(HWND hWnd)
{
    auto interopFactory = GetInteropFactory();
    GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interopFactory->CreateForWindow(
        hWnd,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );
    if (FAILED(hr))
    {
        throw std::runtime_error("[WinRTCapture] CreateForWindow failed, HR=" + std::to_string(hr));
    }
    return item;
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
WinRTScreenCapture::CreateDirect3DDevice(IDXGIDevice* dxgiDevice)
{
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(
        CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put())
    );
    return inspectable.as<IDirect3DDevice>();
}

template<typename T>
winrt::com_ptr<T> WinRTScreenCapture::GetDXGIInterfaceFromObject(
    winrt::Windows::Foundation::IInspectable const& object)
{
    auto dxgiInterfaceAccess = object.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<T> result = nullptr;
    winrt::check_hresult(
        dxgiInterfaceAccess->GetInterface(winrt::guid_of<T>(), result.put_void())
    );
    return result;
}
