#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>

#ifdef USE_CUDA
#include <cuda_d3d11_interop.h>
#include <cuda_runtime_api.h>
#endif

#include "duplication_api_capture.h"
#include "mybot.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

template <typename T>
inline void SafeRelease(T** ppInterface)
{
    if (*ppInterface)
    {
        (*ppInterface)->Release();
        *ppInterface = nullptr;
    }
}

struct FrameContext
{
    ID3D11Texture2D* texture = nullptr;
    bool hasAcquiredFrame = false;
    uint32_t accumulatedFrames = 0;
    bool hasLastPresentTime = false;
    bool hasLastMouseUpdateTime = false;
    bool pointerVisible = false;
    bool rectsCoalesced = false;
    uint32_t totalMetadataBufferSize = 0;
    uint32_t pointerShapeBufferSize = 0;
};

#ifdef USE_CUDA
// 运行期 CUDA interop 自恢复策略：最多重试 5 次，每次间隔至少 1 秒。
// 目的是让单次瞬时故障（驱动 TDR 恢复、短暂显存压力等）不会使 GPU 零拷贝路径
// 在整个 capturer 生命周期内永久退化为 CPU 路径。
constexpr int kMaxCudaInteropRecoveryAttempts = 5;
constexpr std::chrono::seconds kCudaInteropRecoveryCooldown{ 1 };

void SetGpuCaptureStatus(GpuCaptureStatus* status, GpuCaptureStatus value)
{
    if (status)
        *status = value;
}

void ResetDdaCaptureFrameInfo(DdaCaptureFrameInfo* info)
{
    if (info)
        *info = DdaCaptureFrameInfo{};
}

void SetDdaCaptureFrameInfo(DdaCaptureFrameInfo* info, const FrameContext& frameCtx)
{
    if (!info)
        return;

    info->hasLastPresentTime = frameCtx.hasLastPresentTime;
    info->hasLastMouseUpdateTime = frameCtx.hasLastMouseUpdateTime;
    info->rectsCoalesced = frameCtx.rectsCoalesced;
}

bool IsCudaDeviceOnCaptureAdapter(const LUID& adapterLuid, int cudaDevice)
{
#if defined(CUDART_VERSION) && CUDART_VERSION >= 10000
    cudaDeviceProp deviceProp{};
    const cudaError_t propertyError = cudaGetDeviceProperties(&deviceProp, cudaDevice);
    if (propertyError != cudaSuccess)
    {
        std::cerr << "[DDA] Unable to inspect CUDA device " << cudaDevice << ": "
            << cudaGetErrorString(propertyError) << std::endl;
        return false;
    }

    if (deviceProp.tccDriver)
    {
        std::cerr << "[DDA] CUDA device " << cudaDevice
            << " uses TCC; its DXGI LUID is undefined. GPU interop disabled." << std::endl;
        return false;
    }

    return std::memcmp(deviceProp.luid, &adapterLuid, sizeof(deviceProp.luid)) == 0;
#else
    (void)adapterLuid;
    (void)cudaDevice;
    std::cerr << "[DDA] CUDA SDK has no comparable DXGI LUID API. GPU interop disabled." << std::endl;
    return false;
#endif
}
#endif

class DDAManager
{
public:
    DDAManager()
        : m_device(nullptr)
        , m_context(nullptr)
        , m_duplication(nullptr)
        , m_output1(nullptr)
        , m_frameAcquired(false)
    {
        ZeroMemory(&m_duplDesc, sizeof(m_duplDesc));
    }

    ~DDAManager()
    {
        Release();
    }

    HRESULT Initialize(
        int monitorIndex,
        int /*captureWidth*/,
        int /*captureHeight*/,
        int& outScreenWidth,
        int& outScreenHeight,
        ID3D11Device** outDevice,
        ID3D11DeviceContext** outContext)
    {
        HRESULT hr = S_OK;

        IDXGIFactory1* factory = nullptr;
        hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
        if (FAILED(hr))
        {
            std::cerr << "[DDA] CreateDXGIFactory1 failed hr=" << std::hex << hr << std::endl;
            return hr;
        }

        IDXGIAdapter1* adapter = nullptr;
        IDXGIOutput* output = nullptr;
        const int targetMonitorIndex = std::max(0, monitorIndex);

        int currentMonitorIndex = 0;
        bool foundOutput = false;
        for (UINT adapterIdx = 0; ; ++adapterIdx)
        {
            IDXGIAdapter1* candidateAdapter = nullptr;
            hr = factory->EnumAdapters1(adapterIdx, &candidateAdapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr))
            {
                std::cerr << "[DDA] EnumAdapters1 failed hr=" << std::hex << hr << std::endl;
                SafeRelease(&factory);
                return hr;
            }

            for (UINT outputIdx = 0; ; ++outputIdx)
            {
                IDXGIOutput* candidateOutput = nullptr;
                hr = candidateAdapter->EnumOutputs(outputIdx, &candidateOutput);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(hr))
                {
                    std::cerr << "[DDA] EnumOutputs failed hr=" << std::hex << hr << std::endl;
                    SafeRelease(&candidateAdapter);
                    SafeRelease(&factory);
                    return hr;
                }

                if (currentMonitorIndex == targetMonitorIndex)
                {
                    adapter = candidateAdapter;
                    output = candidateOutput;
                    foundOutput = true;
                    break;
                }

                ++currentMonitorIndex;
                candidateOutput->Release();
            }

            if (foundOutput)
                break;

            candidateAdapter->Release();
        }

        if (!foundOutput || !adapter || !output)
        {
            std::cerr << "[DDA] No monitor with index " << targetMonitorIndex << std::endl;
            SafeRelease(&adapter);
            SafeRelease(&output);
            SafeRelease(&factory);
            return DXGI_ERROR_NOT_FOUND;
        }

        {
            D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
            UINT createDeviceFlags = 0;

            hr = D3D11CreateDevice(
                adapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                createDeviceFlags,
                featureLevels,
                1,
                D3D11_SDK_VERSION,
                &m_device,
                nullptr,
                &m_context
            );
            if (FAILED(hr))
            {
                std::cerr << "[DDA] D3D11CreateDevice failed hr=" << std::hex << hr << std::endl;
                SafeRelease(&output);
                SafeRelease(&adapter);
                SafeRelease(&factory);
                return hr;
            }

            IDXGIDevice1* dxgiDevice = nullptr;
            if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice)))
            {
                dxgiDevice->SetMaximumFrameLatency(1);
                dxgiDevice->Release();
            }
        }

        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&m_output1);
        if (FAILED(hr))
        {
            std::cerr << "[DDA] QueryInterface(IDXGIOutput1) failed hr=" << std::hex << hr << std::endl;
            SafeRelease(&m_context);
            SafeRelease(&m_device);
            SafeRelease(&output);
            SafeRelease(&adapter);
            SafeRelease(&factory);
            return hr;
        }

        hr = m_output1->DuplicateOutput(m_device, &m_duplication);
        if (FAILED(hr))
        {
            std::cerr << "[DDA] DuplicateOutput failed hr=" << std::hex << hr << std::endl;
            SafeRelease(&m_output1);
            SafeRelease(&m_context);
            SafeRelease(&m_device);
            SafeRelease(&output);
            SafeRelease(&adapter);
            SafeRelease(&factory);
            return hr;
        }

        m_duplication->GetDesc(&m_duplDesc);

        DXGI_OUTPUT_DESC oDesc{};
        output->GetDesc(&oDesc);
        outScreenWidth = oDesc.DesktopCoordinates.right - oDesc.DesktopCoordinates.left;
        outScreenHeight = oDesc.DesktopCoordinates.bottom - oDesc.DesktopCoordinates.top;

        SafeRelease(&output);
        if (adapter)
        {
            DXGI_ADAPTER_DESC1 adapterDesc{};
            hr = adapter->GetDesc1(&adapterDesc);
            if (FAILED(hr))
            {
                std::cerr << "[DDA] GetDesc1 failed hr=" << std::hex << hr << std::endl;
                SafeRelease(&adapter);
                SafeRelease(&factory);
                return hr;
            }
            m_adapterLuid = adapterDesc.AdapterLuid;
        }
        SafeRelease(&adapter);
        SafeRelease(&factory);

        if (outDevice)  *outDevice = m_device;
        if (outContext) *outContext = m_context;

        return hr;
    }

    HRESULT AcquireFrame(FrameContext& frameCtx, UINT timeout = 100)
    {
        frameCtx.texture = nullptr;
        frameCtx.hasAcquiredFrame = false;
        if (!m_duplication) return E_FAIL;

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        IDXGIResource* resource = nullptr;

        HRESULT hr = m_duplication->AcquireNextFrame(timeout, &frameInfo, &resource);
        if (FAILED(hr)) return hr;

        frameCtx.hasAcquiredFrame = true;
        frameCtx.accumulatedFrames = frameInfo.AccumulatedFrames;
        frameCtx.hasLastPresentTime = frameInfo.LastPresentTime.QuadPart != 0;
        frameCtx.hasLastMouseUpdateTime = frameInfo.LastMouseUpdateTime.QuadPart != 0;
        frameCtx.pointerVisible = frameInfo.PointerPosition.Visible != FALSE;
        frameCtx.rectsCoalesced = frameInfo.RectsCoalesced != FALSE;
        frameCtx.totalMetadataBufferSize = frameInfo.TotalMetadataBufferSize;
        frameCtx.pointerShapeBufferSize = frameInfo.PointerShapeBufferSize;
        m_frameAcquired = true;

        if (resource)
        {
            hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frameCtx.texture);
            resource->Release();
        }
        return hr;
    }

    void ReleaseFrame()
    {
        if (!m_duplication || !m_frameAcquired)
            return;

        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }

    void Release()
    {
        if (m_duplication)
        {
            ReleaseFrame();
            m_duplication->Release();
            m_duplication = nullptr;
        }
        SafeRelease(&m_output1);
        SafeRelease(&m_context);
        SafeRelease(&m_device);
    }

public:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;
    IDXGIOutputDuplication* m_duplication;
    IDXGIOutput1* m_output1;
    DXGI_OUTDUPL_DESC m_duplDesc;
    bool m_frameAcquired;
    LUID m_adapterLuid{};
};

DuplicationAPIScreenCapture::DuplicationAPIScreenCapture(int desiredWidth, int desiredHeight, int monitorIndex)
    : d3dDevice(nullptr)
    , d3dContext(nullptr)
    , deskDupl(nullptr)
    , output1(nullptr)
    , stagingTextureCPU(nullptr)
    , screenWidth(0)
    , screenHeight(0)
    , regionWidth(desiredWidth)
    , regionHeight(desiredHeight)
{
    m_ddaManager = std::make_unique<DDAManager>();

    HRESULT hr = m_ddaManager->Initialize(
        monitorIndex,
        regionWidth,
        regionHeight,
        screenWidth,
        screenHeight,
        &d3dDevice,
        &d3dContext
    );
    if (FAILED(hr))
    {
        std::cerr << "[DDA] DDAManager Initialize failed hr=0x" << std::hex << hr << std::endl;
        return;
    }

    regionWidth = std::clamp(regionWidth, 1, std::max(1, screenWidth));
    regionHeight = std::clamp(regionHeight, 1, std::max(1, screenHeight));
    SetSourceDimensions(screenWidth, screenHeight);

    initialized_ = createStagingTextureCPU();
    if (!initialized_)
        return;

#ifdef USE_CUDA
    initializeCudaInterop();
#endif
}

DuplicationAPIScreenCapture::~DuplicationAPIScreenCapture()
{
 #ifdef USE_CUDA
    releaseCudaInterop();
 #endif
    if (m_ddaManager)
    {
        m_ddaManager->Release();
        m_ddaManager.reset();
    }
    SafeRelease(&stagingTextureCPU);

    d3dDevice = nullptr;
    d3dContext = nullptr;
    deskDupl = nullptr;
    output1 = nullptr;
}

cv::Mat DuplicationAPIScreenCapture::GetNextFrameCpu()
{
    if (!m_ddaManager || !m_ddaManager->m_duplication || !stagingTextureCPU)
        return cv::Mat();

    FrameContext frameCtx;
    HRESULT hr = m_ddaManager->AcquireFrame(frameCtx, 0);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        return cv::Mat();
    }
    else if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_INVALID_CALL)
    {
        capture_method_changed.store(true);
        return cv::Mat();
    }
    else if (FAILED(hr))
    {
        std::cerr << "[DuplicationAPIScreenCapture] AcquireNextFrame (CPU) failed hr=0x"
            << std::hex << hr << std::endl;
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        return cv::Mat();
    }

    if (!frameCtx.texture)
    {
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        return cv::Mat();
    }

    const int copyWidth = std::min(regionWidth, std::max(1, screenWidth));
    const int copyHeight = std::min(regionHeight, std::max(1, screenHeight));
    const int left = std::max(0, (screenWidth - copyWidth) / 2);
    const int top = std::max(0, (screenHeight - copyHeight) / 2);

    D3D11_BOX sourceRegion;
    sourceRegion.left = left;
    sourceRegion.top = top;
    sourceRegion.front = 0;
    sourceRegion.right = sourceRegion.left + copyWidth;
    sourceRegion.bottom = sourceRegion.top + copyHeight;
    sourceRegion.back = 1;

    d3dContext->CopySubresourceRegion(
        stagingTextureCPU,
        0,
        0, 0, 0,
        frameCtx.texture,
        0,
        &sourceRegion
    );

    m_ddaManager->ReleaseFrame();
    frameCtx.texture->Release();

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hrMap = d3dContext->Map(stagingTextureCPU, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hrMap))
    {
        std::cerr << "[DDA] Map stagingTextureCPU failed hr=" << std::hex << hrMap << std::endl;
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
    } unmapGuard{ d3dContext, stagingTextureCPU };

    cv::Mat cpuFrame(regionHeight, regionWidth, CV_8UC4);
    const size_t rowBytes = static_cast<size_t>(regionWidth) * 4;
    const unsigned char* srcBase = static_cast<const unsigned char*>(mapped.pData);
    if (static_cast<size_t>(mapped.RowPitch) == rowBytes && cpuFrame.step == rowBytes)
    {
        // 行距连续时合并为单次拷贝，减少 regionHeight 次调用与循环开销。
        std::memcpy(cpuFrame.data, srcBase, rowBytes * static_cast<size_t>(regionHeight));
    }
    else
    {
        for (int y = 0; y < regionHeight; y++)
        {
            unsigned char* dstRow = cpuFrame.ptr<unsigned char>(y);
            const unsigned char* srcRow = srcBase + static_cast<size_t>(y) * mapped.RowPitch;
            std::memcpy(dstRow, srcRow, rowBytes);
        }
    }

    d3dContext->Unmap(stagingTextureCPU, 0);
    unmapGuard.texture = nullptr;
    return cpuFrame;
}

bool DuplicationAPIScreenCapture::GetNextFrameGpu(
    cv::cuda::GpuMat& gpuFrameBgra,
    GpuCaptureStatus* status,
    uint32_t* accumulatedFrames,
    DdaCaptureFrameInfo* frameInfo)
{
    // interop 在运行期被瞬时故障置为不可用时，按冷却时间与次数上限尝试重建，
    // 避免一次性错误导致本 capturer 生命周期内永久退化到 CPU 回退路径。
    if (!cudaInteropReady && !cudaInteropPermanentlyDisabled)
        tryRecoverCudaInterop();

    if (!m_ddaManager || !m_ddaManager->m_duplication || !interopTextureGPU || !cudaInteropResource || !cudaInteropReady)
    {
        SetGpuCaptureStatus(status, GpuCaptureStatus::NotReady);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }

    FrameContext frameCtx;
    HRESULT hr = m_ddaManager->AcquireFrame(frameCtx, 0);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        SetGpuCaptureStatus(status, GpuCaptureStatus::Timeout);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }
    else if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_INVALID_CALL)
    {
        capture_method_changed.store(true);
        SetGpuCaptureStatus(status, GpuCaptureStatus::DeviceLost);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }
    else if (FAILED(hr))
    {
        std::cerr << "[DuplicationAPIScreenCapture] AcquireNextFrame (GPU) failed hr=0x"
            << std::hex << hr << std::endl;
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        SetGpuCaptureStatus(status, GpuCaptureStatus::AcquireFailed);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }

    if (!frameCtx.texture)
    {
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        SetGpuCaptureStatus(status, GpuCaptureStatus::MissingTexture);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    // 无新画面事件：桌面复制 API 在仅指针移动或仅元数据更新时同样返回成功，
    // 此时桌面图像与上一帧逐像素相同。继续执行 CopySubresourceRegion + CUDA 拷贝
    // + TensorRT 推理提交属于纯粹浪费（GPU 占用、功耗与延迟预算）。
    // LastPresentTime==0 与 AccumulatedFrames==0 同时成立才判定为无新画面，取保守判据。
    // 上层 captureThread 已实现该状态的完整处理（保持采集可用、保留既有检测结果、
    // 不清空预览帧），此处补齐生产端。
    if (!frameCtx.hasLastPresentTime && frameCtx.accumulatedFrames == 0)
    {
        m_ddaManager->ReleaseFrame();
        frameCtx.texture->Release();
        frameCtx.texture = nullptr;
        SetGpuCaptureStatus(status, GpuCaptureStatus::NoPresent);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    const int copyWidth = std::min(regionWidth, std::max(1, screenWidth));
    const int copyHeight = std::min(regionHeight, std::max(1, screenHeight));
    const int left = std::max(0, (screenWidth - copyWidth) / 2);
    const int top = std::max(0, (screenHeight - copyHeight) / 2);

    D3D11_BOX sourceRegion;
    sourceRegion.left = left;
    sourceRegion.top = top;
    sourceRegion.front = 0;
    sourceRegion.right = sourceRegion.left + copyWidth;
    sourceRegion.bottom = sourceRegion.top + copyHeight;
    sourceRegion.back = 1;

    d3dContext->CopySubresourceRegion(
        interopTextureGPU,
        0,
        0, 0, 0,
        frameCtx.texture,
        0,
        &sourceRegion
    );

    m_ddaManager->ReleaseFrame();
    frameCtx.texture->Release();

    // 全部 interop 操作下发到采集专用非阻塞流。若流创建失败则退化为默认流（原行为）。
    cudaStream_t copyStream = cudaCaptureStream;

    cudaError_t cuErr = cudaGraphicsMapResources(1, &cudaInteropResource, copyStream);
    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsMapResources failed: " << cudaGetErrorString(cuErr) << std::endl;
        cudaInteropReady = false;
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaMapFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    cudaArray_t mappedArray = nullptr;
    cuErr = cudaGraphicsSubResourceGetMappedArray(&mappedArray, cudaInteropResource, 0, 0);
    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsSubResourceGetMappedArray failed: " << cudaGetErrorString(cuErr) << std::endl;
        const cudaError_t unmapError = cudaGraphicsUnmapResources(1, &cudaInteropResource, copyStream);
        if (unmapError != cudaSuccess)
            std::cerr << "[DDA] cudaGraphicsUnmapResources failed: " << cudaGetErrorString(unmapError) << std::endl;
        cudaInteropReady = false;
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaArrayFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    try
    {
        gpuFrameBgra.create(regionHeight, regionWidth, CV_8UC4);
    }
    catch (const cv::Exception& error)
    {
        std::cerr << "[DDA] Unable to allocate GPU frame: " << error.what() << std::endl;
        const cudaError_t unmapError = cudaGraphicsUnmapResources(1, &cudaInteropResource, copyStream);
        if (unmapError != cudaSuccess)
        {
            std::cerr << "[DDA] cudaGraphicsUnmapResources failed: "
                << cudaGetErrorString(unmapError) << std::endl;
            cudaInteropReady = false;
        }
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaCopyFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    cuErr = cudaMemcpy2DFromArrayAsync(
        gpuFrameBgra.ptr<unsigned char>(),
        gpuFrameBgra.step,
        mappedArray,
        0, 0,
        static_cast<size_t>(regionWidth) * 4,
        static_cast<size_t>(regionHeight),
        cudaMemcpyDeviceToDevice,
        copyStream
    );

    // unmap 是流序操作，排在拷贝之后即可保证资源在拷贝完成前保持映射。
    cudaError_t unmapErr = cudaGraphicsUnmapResources(1, &cudaInteropResource, copyStream);
    if (unmapErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsUnmapResources failed: " << cudaGetErrorString(unmapErr) << std::endl;
        cudaInteropReady = false;
    }

    // 仅等待本流：帧数据在返回给调用方之前必须就绪，但不再牵连推理流的在途工作。
    const cudaError_t syncErr = cudaStreamSynchronize(copyStream);
    if (syncErr != cudaSuccess)
        std::cerr << "[DDA] cudaStreamSynchronize failed: " << cudaGetErrorString(syncErr) << std::endl;

    if (cuErr != cudaSuccess || unmapErr != cudaSuccess || syncErr != cudaSuccess)
    {
        if (cuErr != cudaSuccess)
            std::cerr << "[DDA] cudaMemcpy2DFromArrayAsync failed: " << cudaGetErrorString(cuErr) << std::endl;
        cudaInteropReady = false;
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaCopyFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    SetGpuCaptureStatus(status, GpuCaptureStatus::Captured);
    if (accumulatedFrames)
        *accumulatedFrames = frameCtx.accumulatedFrames;
    SetDdaCaptureFrameInfo(frameInfo, frameCtx);
    return true;
}


bool DuplicationAPIScreenCapture::createStagingTextureCPU()
{
    if (!d3dDevice) return false;

    SafeRelease(&stagingTextureCPU);

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
    desc.MiscFlags = 0;

    HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTextureCPU);
    if (FAILED(hr))
    {
        std::cerr << "[DDA] CreateTexture2D(staging) failed hr=" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}

#ifdef USE_CUDA
bool DuplicationAPIScreenCapture::createInteropTextureGPU()
{
    if (!d3dDevice)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = regionWidth;
    desc.Height = regionHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    const HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &interopTextureGPU);
    if (FAILED(hr))
    {
        std::cerr << "[DDA] CreateTexture2D(interop) failed hr=" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}

bool DuplicationAPIScreenCapture::initializeCudaInterop()
{
    if (!m_ddaManager || cudaInteropPermanentlyDisabled)
        return false;

    // config 由 UI/键盘线程并发写入，必须在 configMutex 下取快照后再使用。
    int cudaDevice = 0;
    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        cudaDevice = config.cuda_device_index;
    }
    int deviceCount = 0;
    const cudaError_t countError = cudaGetDeviceCount(&deviceCount);
    if (countError != cudaSuccess || cudaDevice < 0 || cudaDevice >= deviceCount)
    {
        std::cerr << "[DDA] Selected CUDA device is unavailable; GPU interop disabled."
            << std::endl;
        cudaInteropPermanentlyDisabled = true;
        return false;
    }

    if (!IsCudaDeviceOnCaptureAdapter(m_ddaManager->m_adapterLuid, cudaDevice))
    {
        std::cerr << "[DDA] Capture adapter LUID does not match selected CUDA device "
            << cudaDevice << "; GPU interop disabled." << std::endl;
        cudaInteropPermanentlyDisabled = true;
        return false;
    }

    const cudaError_t setDeviceError = cudaSetDevice(cudaDevice);
    if (setDeviceError != cudaSuccess)
    {
        std::cerr << "[DDA] cudaSetDevice(" << cudaDevice << ") failed: "
            << cudaGetErrorString(setDeviceError) << std::endl;
        cudaInteropPermanentlyDisabled = true;
        return false;
    }

    if (!createInteropTextureGPU())
    {
        cudaInteropPermanentlyDisabled = true;
        return false;
    }

    const cudaError_t registerError = cudaGraphicsD3D11RegisterResource(
        &cudaInteropResource, interopTextureGPU, cudaGraphicsRegisterFlagsNone);
    if (registerError != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsD3D11RegisterResource failed: "
            << cudaGetErrorString(registerError) << std::endl;
        SafeRelease(&interopTextureGPU);
        cudaInteropPermanentlyDisabled = true;
        return false;
    }

    // 采集专用非阻塞流：避免 interop 映射与 D2D 拷贝经由 legacy 默认流(0)
    // 与 TensorRT 推理所用的 blocking 流隐式串行化。创建失败时退回默认流，
    // 仅损失并发度，不影响功能正确性。
    if (!cudaCaptureStream)
    {
        const cudaError_t streamError =
            cudaStreamCreateWithFlags(&cudaCaptureStream, cudaStreamNonBlocking);
        if (streamError != cudaSuccess)
        {
            std::cerr << "[DDA] cudaStreamCreateWithFlags failed: "
                << cudaGetErrorString(streamError) << "; falling back to the default stream."
                << std::endl;
            cudaCaptureStream = nullptr;
        }
    }

    cudaInteropReady = true;
    return true;
}

void DuplicationAPIScreenCapture::releaseCudaInterop()
{
    if (cudaCaptureStream)
    {
        // 先排空在途拷贝，再销毁流，避免销毁仍有工作在途的流。
        const cudaError_t syncError = cudaStreamSynchronize(cudaCaptureStream);
        if (syncError != cudaSuccess)
        {
            std::cerr << "[DDA] cudaStreamSynchronize (teardown) failed: "
                << cudaGetErrorString(syncError) << std::endl;
        }
        const cudaError_t destroyError = cudaStreamDestroy(cudaCaptureStream);
        if (destroyError != cudaSuccess)
        {
            std::cerr << "[DDA] cudaStreamDestroy failed: "
                << cudaGetErrorString(destroyError) << std::endl;
        }
        cudaCaptureStream = nullptr;
    }

    if (cudaInteropResource)
    {
        const cudaError_t unregisterError = cudaGraphicsUnregisterResource(cudaInteropResource);
        if (unregisterError != cudaSuccess)
        {
            std::cerr << "[DDA] cudaGraphicsUnregisterResource failed: "
                << cudaGetErrorString(unregisterError) << std::endl;
        }
        cudaInteropResource = nullptr;
    }
    cudaInteropReady = false;
    SafeRelease(&interopTextureGPU);
}

bool DuplicationAPIScreenCapture::tryRecoverCudaInterop()
{
    if (cudaInteropPermanentlyDisabled)
        return false;

    if (cudaInteropRecoveryAttempts >= kMaxCudaInteropRecoveryAttempts)
    {
        std::cerr << "[DDA] CUDA interop recovery attempts exhausted ("
            << kMaxCudaInteropRecoveryAttempts << "); GPU path disabled for this capturer."
            << std::endl;
        cudaInteropPermanentlyDisabled = true;
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (cudaInteropLastRecovery.time_since_epoch().count() != 0 &&
        now - cudaInteropLastRecovery < kCudaInteropRecoveryCooldown)
    {
        return false;
    }

    cudaInteropLastRecovery = now;
    ++cudaInteropRecoveryAttempts;

    releaseCudaInterop();
    // 清除可能残留的粘滞错误状态，避免把上一次的错误误判为本次重建失败。
    cudaGetLastError();

    if (!initializeCudaInterop())
    {
        std::cerr << "[DDA] CUDA interop recovery attempt "
            << cudaInteropRecoveryAttempts << " failed." << std::endl;
        // initializeCudaInterop 的所有失败分支都会置永久禁用位。
        // 只要重试预算未耗尽，就撤销该置位，让后续冷却窗口仍能再次尝试；
        // 预算耗尽后保持禁用，确保永久性故障不会无限重试。
        if (cudaInteropRecoveryAttempts < kMaxCudaInteropRecoveryAttempts)
            cudaInteropPermanentlyDisabled = false;
        return false;
    }

    std::cout << "[DDA] CUDA interop recovered on attempt "
        << cudaInteropRecoveryAttempts << "." << std::endl;
    // 恢复成功后重置预算，使长时间运行中零星出现的瞬时故障不会累计耗尽重试机会。
    cudaInteropRecoveryAttempts = 0;
    return true;
}
#endif
