#ifndef DUPLICATION_API_CAPTURE_H
#define DUPLICATION_API_CAPTURE_H

#include <d3d11.h>
#include <dxgi1_2.h>
#include <opencv2/opencv.hpp>
#include <chrono>
#include <memory>
#include <cstdint>
#ifdef USE_CUDA
#include <cuda_runtime_api.h>
#endif

#include "capture.h"

class DDAManager;

#ifdef USE_CUDA
enum class GpuCaptureStatus
{
    Captured,
    NotReady,
    Timeout,
    DeviceLost,
    AcquireFailed,
    MissingTexture,
    CudaMapFailed,
    CudaArrayFailed,
    CudaCopyFailed,
    NoPresent
};

struct DdaCaptureFrameInfo
{
    bool hasLastPresentTime = false;
    bool hasLastMouseUpdateTime = false;
    bool rectsCoalesced = false;
};
#endif

class DuplicationAPIScreenCapture : public IScreenCapture
{
public:
    DuplicationAPIScreenCapture(int desiredWidth, int desiredHeight, int monitorIndex);
    ~DuplicationAPIScreenCapture();

    cv::Mat GetNextFrameCpu() override;
#ifdef USE_CUDA
    bool GetNextFrameGpu(
        cv::cuda::GpuMat& gpuFrameBgra,
        GpuCaptureStatus* status,
        uint32_t* accumulatedFrames,
        DdaCaptureFrameInfo* frameInfo);
#endif
    bool isInitialized() const { return initialized_; }

private:
    std::unique_ptr<DDAManager> m_ddaManager;

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    IDXGIOutputDuplication* deskDupl = nullptr;
    IDXGIOutput1* output1 = nullptr;

    ID3D11Texture2D* stagingTextureCPU = nullptr;
#ifdef USE_CUDA
    ID3D11Texture2D* interopTextureGPU = nullptr;
    cudaGraphicsResource_t cudaInteropResource = nullptr;
    // 采集专用非阻塞流。DDA 的 interop 映射与设备到设备拷贝若下发到 legacy 默认流(0)，
    // 会与 TensorRT 推理所用的 blocking 流产生隐式串行化，使采集线程每帧阻塞等待整轮推理。
    cudaStream_t cudaCaptureStream = nullptr;
    bool cudaInteropReady = false;
    bool cudaInteropPermanentlyDisabled = false;
    // 运行期瞬时 CUDA 故障后的受限自恢复状态（冷却时间 + 次数上限）。
    int cudaInteropRecoveryAttempts = 0;
    std::chrono::steady_clock::time_point cudaInteropLastRecovery{};
#endif

    int screenWidth = 0;
    int screenHeight = 0;
    int regionWidth = 0;
    int regionHeight = 0;
    bool initialized_ = false;

    bool createStagingTextureCPU();
#ifdef USE_CUDA
    bool createInteropTextureGPU();
    bool initializeCudaInterop();
    void releaseCudaInterop();
    // 运行期 interop 失效后的受限重建；成功返回 true。
    bool tryRecoverCudaInterop();
#endif
};

#endif // DUPLICATION_API_CAPTURE_H
