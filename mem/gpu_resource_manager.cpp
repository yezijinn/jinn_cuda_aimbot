#include "gpu_resource_manager.h"

#ifdef USE_CUDA

#include <iostream>
#include <limits>

GPUResourceManager::~GPUResourceManager()
{
    // RAII 兜底：无论 main 从哪条路径返回（FatalExit / 异常 / 正常结束），
    // 预留显存都会被归还，不再泄漏到进程退出。
    releaseReservation();
}

void GPUResourceManager::releaseReservation()
{
    if (reservedBuffer == nullptr)
    {
        reservedSize = 0;
        return;
    }

    const cudaError_t err = cudaFree(reservedBuffer);
    if (err != cudaSuccess)
    {
        std::cerr << "[GPU] cudaFree 归还预留显存失败: " << cudaGetErrorString(err) << std::endl;
    }

    // 无论 cudaFree 成功与否都清空句柄，避免后续重复释放同一指针。
    reservedBuffer = nullptr;
    reservedSize = 0;
}

bool GPUResourceManager::reserveGPUMemory(std::size_t reservedMemoryMB)
{
    // 重复调用先归还旧块，否则旧指针会被覆盖并永久泄漏。
    releaseReservation();

    if (reservedMemoryMB == 0)
        return false;

    // MB -> Byte 的乘法溢出保护（配置项来自 ini，可能是任意大的数值）。
    constexpr std::size_t kBytesPerMB = 1024ULL * 1024ULL;
    if (reservedMemoryMB > (std::numeric_limits<std::size_t>::max)() / kBytesPerMB)
    {
        std::cerr << "[GPU] 预留显存请求过大，已忽略: " << reservedMemoryMB << " MB" << std::endl;
        return false;
    }

    std::size_t totalMemory = 0;
    std::size_t freeMemory = 0;
    cudaError_t err = cudaMemGetInfo(&freeMemory, &totalMemory);
    if (err != cudaSuccess)
    {
        std::cerr << "[GPU] cudaMemGetInfo failed: " << cudaGetErrorString(err) << std::endl;
        return false;
    }

    const std::size_t requestSize = reservedMemoryMB * kBytesPerMB;

    if (freeMemory < requestSize)
    {
        std::cerr << "[GPU] Not enough free memory. Requested " << reservedMemoryMB
                  << " MB, free " << (freeMemory / kBytesPerMB) << " MB." << std::endl;
        return false;
    }

    void* buffer = nullptr;
    err = cudaMalloc(&buffer, requestSize);
    if (err != cudaSuccess)
    {
        std::cerr << "[GPU] cudaMalloc failed: " << cudaGetErrorString(err) << std::endl;
        return false;
    }

    err = cudaMemset(buffer, 0, requestSize);
    if (err != cudaSuccess)
    {
        std::cerr << "[GPU] cudaMemset failed: " << cudaGetErrorString(err) << std::endl;
        cudaFree(buffer);
        return false;
    }

    // 仅在完全成功后才登记成员，失败路径不会留下半初始化状态。
    reservedBuffer = buffer;
    reservedSize = requestSize;
    return true;
}

bool GPUResourceManager::setGPUExclusiveMode()
{
    cudaError_t err = cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);
    if (err != cudaSuccess)
    {
        std::cerr << "[GPU] cudaDeviceSetCacheConfig failed: " << cudaGetErrorString(err) << std::endl;
        return false;
    }
    return true;
}

#endif // USE_CUDA
