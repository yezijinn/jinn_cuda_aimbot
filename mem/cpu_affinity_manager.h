#pragma once
#include <windows.h>

#include <cstddef>

// CPU 亲和性 / 系统内存预留管理器。
// 生命周期：RAII。析构自动归还预留的系统内存，保证任何返回路径都不泄漏。
class CPUAffinityManager {
public:
    CPUAffinityManager() = default;
    ~CPUAffinityManager();

    // 持有裸内存指针，禁止拷贝/移动，避免双重 free。
    CPUAffinityManager(const CPUAffinityManager&) = delete;
    CPUAffinityManager& operator=(const CPUAffinityManager&) = delete;
    CPUAffinityManager(CPUAffinityManager&&) = delete;
    CPUAffinityManager& operator=(CPUAffinityManager&&) = delete;

    bool reserveCPUCores(int numCores);
    bool reserveSystemMemory(std::size_t reservedMemoryMB);

    // 归还预留的系统内存。幂等：可重复调用，未持有时为空操作。
    void releaseSystemMemory();

    std::size_t reservedMemoryBytes() const { return reservedMemorySize; }

private:
    // 必须显式初始化：原实现未初始化，未调用 reserveCPUCores 时读取即为未定义行为。
    DWORD_PTR originalMask = 0;
    void* reservedMemory = nullptr;
    std::size_t reservedMemorySize = 0;
};
