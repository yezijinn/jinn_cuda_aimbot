#include "cpu_affinity_manager.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace
{
// DWORD_PTR 的位宽即单个处理器组内可寻址的逻辑核上限（x64 = 64）。
constexpr int kAffinityBitCount = static_cast<int>(sizeof(DWORD_PTR) * 8);
} // namespace

CPUAffinityManager::~CPUAffinityManager()
{
    // RAII 兜底：无论 main 从哪条路径返回，预留的系统内存都会被归还。
    releaseSystemMemory();
}

void CPUAffinityManager::releaseSystemMemory()
{
    if (reservedMemory != nullptr)
    {
        std::free(reservedMemory);
        reservedMemory = nullptr;
    }
    reservedMemorySize = 0;
}

bool CPUAffinityManager::reserveCPUCores(int numCores)
{
    if (numCores <= 0)
    {
        std::cerr << "[CPU] 预留核心数必须为正数，当前值: " << numCores << std::endl;
        return false;
    }

    // 原实现用 (1ULL << i) 逐位置位，i >= 64 时属于未定义行为；
    // 且不校验核心是否真实存在，掩码包含不存在的核会导致 SetThreadAffinityMask 直接失败。
    // 这里改为：从进程实际可用的亲和性掩码中，按低位到高位取前 numCores 个真实存在的核。
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    DWORD_PTR mask = 0;

    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask != 0)
    {
        int picked = 0;
        for (int bit = 0; bit < kAffinityBitCount && picked < numCores; ++bit)
        {
            const DWORD_PTR probe = static_cast<DWORD_PTR>(1) << bit;
            if ((processMask & probe) != 0)
            {
                mask |= probe;
                ++picked;
            }
        }

        if (picked == 0)
        {
            std::cerr << "[CPU] 进程亲和性掩码中没有可用核心。" << std::endl;
            return false;
        }

        if (picked < numCores)
        {
            std::cerr << "[CPU] 请求预留 " << numCores << " 个核心，实际可用 " << picked
                      << " 个，已按可用数量绑定。" << std::endl;
        }
    }
    else
    {
        // 回退路径：无法查询进程亲和性时，仍按请求构造掩码，但把位移限制在合法范围内。
        const int safeCores = (numCores > kAffinityBitCount) ? kAffinityBitCount : numCores;
        for (int i = 0; i < safeCores; ++i)
            mask |= (static_cast<DWORD_PTR>(1) << i);
    }

    originalMask = SetThreadAffinityMask(GetCurrentThread(), mask);
    if (originalMask == 0)
    {
        std::cerr << "[CPU] Failed to set thread affinity mask. GetLastError="
                  << GetLastError() << std::endl;
        return false;
    }

    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
    {
        std::cerr << "[CPU] Failed to set process priority. GetLastError="
                  << GetLastError() << std::endl;
    }

    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST))
    {
        std::cerr << "[CPU] Failed to set thread priority. GetLastError="
                  << GetLastError() << std::endl;
    }

    return true;
}

bool CPUAffinityManager::reserveSystemMemory(std::size_t reservedMemoryMB)
{
    // 重复调用先归还旧块，否则旧指针会被覆盖并永久泄漏。
    releaseSystemMemory();

    if (reservedMemoryMB == 0)
        return false;

    // MB -> Byte 的乘法溢出保护（配置项来自 ini，可能是任意大的数值）。
    constexpr std::size_t kBytesPerMB = 1024ULL * 1024ULL;
    if (reservedMemoryMB > (std::numeric_limits<std::size_t>::max)() / kBytesPerMB)
    {
        std::cerr << "[CPU] 预留系统内存请求过大，已忽略: " << reservedMemoryMB << " MB" << std::endl;
        return false;
    }

    const std::size_t requestSize = reservedMemoryMB * kBytesPerMB;

    void* buffer = std::malloc(requestSize);
    if (buffer == nullptr)
    {
        std::cerr << "[CPU] Failed to reserve system memory: " << reservedMemoryMB << " MB." << std::endl;
        return false;
    }

    // 预触摸全部页面，使其真正提交为物理页（与原实现一致）。
    std::memset(buffer, 0, requestSize);

    reservedMemory = buffer;
    reservedMemorySize = requestSize;
    return true;
}
