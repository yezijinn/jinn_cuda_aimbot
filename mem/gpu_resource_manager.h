#pragma once

#include <cstddef>

#ifdef USE_CUDA
#include <cuda_runtime.h>

// GPU 显存预留管理器。
// 语义：启动阶段先向驱动申请一块显存，避免初始化窗口期被其它进程抢占；
//       随后必须在本进程真正需要显存（TensorRT 引擎反序列化/构建）之前归还，
//       否则这块显存对本进程完全不可用，等价于永久浪费。
// 生命周期：RAII。析构自动归还，保证任何返回路径都不泄漏显存。
class GPUResourceManager {
public:
    GPUResourceManager() = default;
    ~GPUResourceManager();

    // 持有裸显存指针，禁止拷贝/移动，避免双重 cudaFree。
    GPUResourceManager(const GPUResourceManager&) = delete;
    GPUResourceManager& operator=(const GPUResourceManager&) = delete;
    GPUResourceManager(GPUResourceManager&&) = delete;
    GPUResourceManager& operator=(GPUResourceManager&&) = delete;

    bool reserveGPUMemory(std::size_t reservedMemoryMB);
    bool setGPUExclusiveMode();

    // 归还预留显存。幂等：可重复调用，未持有时为空操作。
    void releaseReservation();

    bool hasReservation() const { return reservedBuffer != nullptr; }
    std::size_t reservedBytes() const { return reservedSize; }

private:
    void* reservedBuffer = nullptr;
    std::size_t reservedSize = 0;
};
#else
class GPUResourceManager {
public:
    bool reserveGPUMemory(std::size_t) { return false; }
    bool setGPUExclusiveMode() { return false; }
    void releaseReservation() {}
    bool hasReservation() const { return false; }
    std::size_t reservedBytes() const { return 0; }
};
#endif
