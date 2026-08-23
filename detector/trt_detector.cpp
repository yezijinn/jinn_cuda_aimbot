#ifdef USE_CUDA
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudawarping.hpp>
#include <opencv2/dnn.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <numeric>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <filesystem>
#include <sstream>

#include "trt_detector.h"
#include "nvinf.h"
#include "onnx_inspector.h"
#include "mybot.h"
#include "other_tools.h"
#include "postProcess.h"
#include "cuda_preprocess.h"
#include "capture.h"
#include "capture/circle_fov.h"
#include "mouse/AimbotTarget.h"
#include "scr/data_collector.h"
#include "config.h"

extern std::atomic<bool> detectionPaused;
int model_quant = 0;

extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> detection_resolution_changed;

static std::atomic<bool> error_logged{ false };

namespace {
bool tryGetDimInt(int64_t value, int* out)
{
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return false;
    *out = static_cast<int>(value);
    return true;
}

bool tryGetPositiveDimInt(int64_t value, int* out)
{
    if (value <= 0)
        return false;
    return tryGetDimInt(value, out);
}

std::string FormatTensorShape(const std::vector<int64_t>& shape)
{
    std::ostringstream stream;
    stream << "[";
    for (size_t index = 0; index < shape.size(); ++index)
    {
        if (index != 0)
            stream << ", ";
        stream << shape[index];
    }
    stream << "]";
    return stream.str();
}

const char* TensorDataTypeName(nvinfer1::DataType type)
{
    switch (type)
    {
    case nvinfer1::DataType::kFLOAT: return "FP32";
    case nvinfer1::DataType::kHALF: return "FP16";
    case nvinfer1::DataType::kINT8: return "INT8";
    case nvinfer1::DataType::kINT32: return "INT32";
    case nvinfer1::DataType::kBOOL: return "BOOL";
    case nvinfer1::DataType::kUINT8: return "UINT8";
    case nvinfer1::DataType::kFP8: return "FP8";
    case nvinfer1::DataType::kBF16: return "BF16";
    case nvinfer1::DataType::kINT64: return "INT64";
    case nvinfer1::DataType::kINT4: return "INT4";
    case nvinfer1::DataType::kFP4: return "FP4";
    }
    return "未知";
}

// 【性能】FP16 推理输出 → FP32 转换。
// 原实现为逐元素调用 __half2float 的标量循环：YOLO 输出典型形状为
// [1, 4+nc, 8400]，元素量 6.7e4 ~ 2e5；在 240 FPS 下每秒需完成
// 1600 万~4800 万次单元素转换，是推理线程 CPU 占用的主要来源之一。
// 改用 OpenCV 的 CV_16F → CV_32F 转换，其内部走 F16C / AVX2 硬件半精度指令，
// 单指令处理 8 个元素。
// 数值等价性：half → float 在 IEEE-754 下是**无损精确**的加宽转换
//（float 的指数与尾数范围完全覆盖 half，含非规格化数），
// 因此输出与标量循环逐位一致，检测结果零变化。
// 元素数超过 int 上限或未定义 CV_16F 时退回原标量路径。
void ConvertHalfToFloat(const void* src, float* dst, size_t numElements)
{
    if (!src || !dst || numElements == 0)
        return;

#ifdef CV_16F
    if (numElements <= static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        const cv::Mat halfMat(1, static_cast<int>(numElements), CV_16F, const_cast<void*>(src));
        cv::Mat floatMat(1, static_cast<int>(numElements), CV_32F, dst);
        // dst 尺寸/类型已匹配，Mat::create 为空操作，转换直接写入外部缓冲。
        halfMat.convertTo(floatMat, CV_32F);
        return;
    }
#endif

    const __half* halfPtr = reinterpret_cast<const __half*>(src);
    for (size_t i = 0; i < numElements; ++i)
        dst[i] = __half2float(halfPtr[i]);
}

void filterDetectionsByCircleFov(std::vector<Detection>& detections)
{
    if (detections.empty() || !config.circle_fov_enabled)
        return;

    const cv::Size detectionSize(config.detection_resolution, config.detection_resolution);
    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&detectionSize](const Detection& det)
            {
                const cv::Point2f center(
                    static_cast<float>(det.box.x) + static_cast<float>(det.box.width) * 0.5f,
                    static_cast<float>(det.box.y) + static_cast<float>(det.box.height) * 0.5f);
                return !pointInsideCircleFov(center, detectionSize, config.circle_fov_radius_percent);
            }),
        detections.end());
}

// 【修复 A·数据竞争 + 存盘竞争】引擎重新编译成功后，原实现在推理线程内直接执行
//     config.ai_model = ...;            // 无锁写 std::string
//     config.saveConfig("config.ini");  // 无锁全量读取所有配置字段并落盘
// 而 overlay 渲染线程在持有 configMutex 的前提下每帧读写同一批字段
//（draw_ai.cpp 每帧写 config.backend、模型下拉框读 config.ai_model）。
// 并发的 std::string 读/写在字符串超出 SSO 容量后会释放旧堆缓冲，读侧即 use-after-free；
// 两个线程还可能同时向 config.ini 写入，产生交错的半截配置文件。
// 修复：把「改字段 + 落盘」整体置于 configMutex 临界区内，与工程既有约定一致
//（Config::saveConfig 自身不加锁，由调用方保证持锁，见 overlay RenderOverlayFrame）。
// 该路径仅在引擎编译完成时执行一次（秒级操作），锁开销可忽略。
void PersistSelectedEngineModel(const std::string& engineFilePath)
{
    {
        std::lock_guard<std::mutex> lock(configMutex);
        config.ai_model = std::filesystem::path(engineFilePath).filename().string();
        config.saveConfig("config.ini");
    }

    std::cout << "[TensorRT] 编译完成，引擎已保存至:" << std::endl;
    // engineFilePath 为本地代码页(GBK)窄字符串，显示时转为 UTF-8 避免控制台乱码（文件读写仍用原窄字符串）。
    std::cout << std::filesystem::path(engineFilePath).u8string() << std::endl;
    std::cout << "[TensorRT] 后续运行将直接加载 .engine 文件，跳过编译步骤。" << std::endl;
}

// 解析 engine 尾部内嵌的 "key=value\n" 模型元数据，优先取 nc，
// 缺失时按 Ultralytics names 字典的 "索引:" 项数推断。
int InferClassCountFromEmbeddedMetadata(const std::string& metadata)
{
    if (metadata.empty())
        return -1;

    std::istringstream stream(metadata);
    std::string line;
    int inferred = -1;
    bool foundNc = false;

    while (std::getline(stream, line))
    {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;

        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "nc" && !foundNc)
        {
            try
            {
                inferred = std::stoi(value);
                foundNc = true;
            }
            catch (...)
            {
                // 非法 nc 值继续尝试 names，避免被篡改元数据阻断启动推断。
            }
        }
        else if (key == "names" && !foundNc)
        {
            int count = 0;
            for (std::size_t i = 0; i + 1 < value.size(); ++i)
            {
                if (value[i] >= '0' && value[i] <= '9' && value[i + 1] == ':')
                {
                    ++count;
                    while (i < value.size() && value[i] >= '0' && value[i] <= '9')
                        ++i;
                }
            }
            if (count > 0)
            {
                inferred = count;
                foundNc = true;
            }
        }
    }

    return inferred;
}
} // namespace

TrtDetector::TrtDetector()
    : frameReady(false),
    shouldExit(false),
    useCudaGraph(false),
    cudaGraphCaptured(false),
    cudaGraph(nullptr),
    cudaGraphExec(nullptr),
    stream(nullptr),
    img_scale(1.0f),
    numClasses(0),
    inputBufferDevice(nullptr),
    preprocessStartEvent(nullptr),
    inferenceStartEvent(nullptr),
    inferenceCompleteEvent(nullptr),
    copyCompleteEvent(nullptr),
    asyncInferenceInProgress(false)
{
}

TrtDetector::~TrtDetector()
{
    requestStop();
    destroyCudaGraph();
    freePinnedOutputs();

    for (auto& binding : inputBindings) if (binding.second) cudaFree(binding.second);
    for (auto& binding : outputBindings) if (binding.second) cudaFree(binding.second);
    if (inputBufferDevice) cudaFree(inputBufferDevice);
    if (preprocessStartEvent) cudaEventDestroy(preprocessStartEvent);
    if (inferenceStartEvent) cudaEventDestroy(inferenceStartEvent);
    if (inferenceCompleteEvent) cudaEventDestroy(inferenceCompleteEvent);
    if (copyCompleteEvent) cudaEventDestroy(copyCompleteEvent);
    if (stream) cudaStreamDestroy(stream);
}

void TrtDetector::requestStop()
{
    shouldExit = true;
    inferenceCV.notify_all();
}

bool TrtDetector::isInitialized() const
{
    return context != nullptr;
}

// 【稳定性·推理链路自恢复】
//
// 背景：前轮修复把 enqueueV3 / cudaMemcpyAsync / cudaEventSynchronize 的失败
// 从"静默产出幽灵检测框"改成了"跳过本帧"。这在瞬时故障下是正确的，但对**持久性
// 故障**留下了一个死角：显示驱动 TDR 恢复、独占全屏切换、显存被其它进程耗尽等
// 情况会让 CUDA 上下文/执行上下文永久失效，此后每一帧都失败、每一帧都 continue。
// 表现为程序还在跑、采集还在出帧、CPU/GPU 持续被占用，但检测结果永远为空，
// 控制台被同一条错误刷屏，且**永远不会自行恢复** —— 用户只能重启程序。
//
// 本函数在推理线程内维护一个连续失败计数。达到阈值后，以「冷却 + 尝试上限」的
// 方式触发一次重初始化。恢复动作刻意复用既有的 detector_model_changed 路径，
// 而不是新写一套拆除逻辑：那条路径（拆 CUDA Graph → 释放上下文/引擎/绑定 →
// initialize()）已在模型切换场景中长期验证，且天然运行在推理线程自身、
// 持有 inferenceMutex，重用它可把回归面压到最小。
//
// 三重限幅避免"恢复风暴"：
//   - kFailureThreshold：需连续失败足够多帧才认定为持久故障，滤掉瞬时抖动；
//   - kRecoveryCooldown：两次恢复之间至少间隔一段时间，避免重建引擎的高开销
//     动作被高频触发反而拖垮系统；
//   - kMaxRecoveryAttempts：预算耗尽后彻底停手（例如显卡已被拔出/驱动崩溃这类
//     不可恢复场景），转为静默跳帧，不再无谓地反复重建。
// 任意一帧推理成功都会把计数与预算一并清零，回到正常状态。
void TrtDetector::noteInferenceOutcome(bool ok)
{
    constexpr uint32_t kFailureThreshold = 30;      // 约 0.1~1 秒的连续失败
    constexpr uint32_t kMaxRecoveryAttempts = 5;
    constexpr auto kRecoveryCooldown = std::chrono::seconds(3);

    if (ok)
    {
        if (consecutiveInferenceFailures.load(std::memory_order_relaxed) != 0)
        {
            consecutiveInferenceFailures.store(0, std::memory_order_relaxed);
            inferenceRecoveryAttempts = 0;
            inferenceRecoveryExhausted = false;
        }
        return;
    }

    const uint32_t streak =
        consecutiveInferenceFailures.fetch_add(1, std::memory_order_relaxed) + 1;

    if (streak < kFailureThreshold || inferenceRecoveryExhausted)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (lastInferenceRecoveryTime.time_since_epoch().count() != 0 &&
        now - lastInferenceRecoveryTime < kRecoveryCooldown)
    {
        return;
    }

    if (inferenceRecoveryAttempts >= kMaxRecoveryAttempts)
    {
        inferenceRecoveryExhausted = true;
        std::cerr << "[Detector] 推理连续失败 " << streak
                  << " 帧，且已用尽 " << kMaxRecoveryAttempts
                  << " 次自动恢复机会；请检查显卡驱动/显存占用后重启程序。"
                  << std::endl;
        return;
    }

    ++inferenceRecoveryAttempts;
    lastInferenceRecoveryTime = now;
    consecutiveInferenceFailures.store(0, std::memory_order_relaxed);

    std::cerr << "[Detector] 推理连续失败 " << streak
              << " 帧，判定为持久性故障，触发第 " << inferenceRecoveryAttempts
              << "/" << kMaxRecoveryAttempts << " 次引擎重初始化。" << std::endl;

    // 复用已验证的重初始化路径：下一轮循环开头即完成拆除 + initialize()。
    detector_model_changed.store(true);
}

void TrtDetector::freePinnedOutputs()
{
    for (auto& kv : pinnedOutputBuffers)
    {
        if (kv.second)
            cudaFreeHost(kv.second);
    }
    pinnedOutputBuffers.clear();
}

void TrtDetector::allocatePinnedOutputs()
{
    freePinnedOutputs();

    for (const auto& name : outputNames)
    {
        const size_t bytes = outputSizes[name];
        if (bytes == 0) continue;

        void* hostPtr = nullptr;
        cudaError_t err = cudaHostAlloc(&hostPtr, bytes, cudaHostAllocDefault);
        if (err != cudaSuccess)
        {
            std::cerr << "[Detector] cudaHostAlloc failed for output " << name
                << " (" << bytes << " bytes): " << cudaGetErrorString(err) << std::endl;
            continue;
        }

        pinnedOutputBuffers[name] = hostPtr;

        if (config.verbose)
        {
            std::cout << "[Detector] Allocated pinned host buffer for output " << name
                << ": " << bytes << " bytes" << std::endl;
        }
    }
}

void TrtDetector::destroyCudaGraph()
{
    if (cudaGraphExec)
    {
        cudaGraphExecDestroy(cudaGraphExec);
        cudaGraphExec = nullptr;
    }
    if (cudaGraph)
    {
        cudaGraphDestroy(cudaGraph);
        cudaGraph = nullptr;
    }
    cudaGraphCaptured = false;
}

void TrtDetector::captureCudaGraph()
{
    if (!useCudaGraph || cudaGraphCaptured) return;

    destroyCudaGraph();

    cudaStreamSynchronize(stream);

    // 【修复 C·捕获模式冻结其他线程】cudaStreamCaptureModeGlobal 的语义是：
    // 在捕获窗口内，**进程中所有线程**发起的"潜在不安全" CUDA API
    //（cudaMalloc / cudaFree / 同步类调用等）一律失败返回
    // cudaErrorStreamCaptureUnsupported，并在该线程留下粘滞错误。
    // 本函数会在运行期被触发：用户在 UI 切换模型或切换"CUDA 图"开关时，
    // 推理线程重新捕获，而此时采集线程正在高频执行 GpuMat 分配与
    // cudaMemcpy2DFromArrayAsync —— 这些调用会被连带打回，
    // 表现为切模型瞬间大量掉帧，甚至 CUDA interop 被判定失败而退化到 CPU 采集路径。
    // 捕获本身只涉及本线程的 stream，正确的作用域是 ThreadLocal：
    // 仅限制本线程的不安全调用，其他线程不受影响。
    cudaError_t st = cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] BeginCapture failed: "
            << cudaGetErrorString(st) << std::endl;
        return;
    }

    if (!context->enqueueV3(stream))
    {
        std::cerr << "[Detector] CUDA graph enqueueV3 失败" << std::endl;
        const cudaError_t captureError = cudaGetLastError();
        if (captureError != cudaSuccess)
            std::cerr << "[Detector] CUDA错误: " << cudaGetErrorString(captureError) << std::endl;
        // 【修复·图对象泄漏】原实现直接把 cudaGraph 置空。若 EndCapture 成功返回了
        // 一个已构建的 graph 对象，该对象将永远无法回收（每次捕获失败泄漏一次）。
        cudaGraph_t partialGraph = nullptr;
        const cudaError_t endErr = cudaStreamEndCapture(stream, &partialGraph);
        if (endErr == cudaSuccess && partialGraph)
            cudaGraphDestroy(partialGraph);
        cudaGraph = nullptr;
        return;
    }
    cudaEventRecord(inferenceCompleteEvent, stream);

    // 【修复 D·operator[] 静默插入 nullptr】原实现用 outputBindings[name] /
    // outputSizes[name] 取值：键缺失时 unordered_map::operator[] 会**静默插入**
    // 一个 nullptr / 0 条目，既污染了 getBindings() 建立的绑定表，又会把
    // "从空设备指针拷贝" 这一非法节点录进 CUDA 图。改用 find() 显式判定，
    // 任一输出缺少有效缓冲即干净地中止本次捕获（回退到非图路径，功能不受影响）。
    bool captureCopiesOk = true;
    for (const auto& name : outputNames)
    {
        const auto itPinned = pinnedOutputBuffers.find(name);
        if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
            continue;  // 该输出未分配锁页缓冲：与原实现一致地跳过（后处理侧同样跳过）。

        const auto itDevice = outputBindings.find(name);
        const auto itSize = outputSizes.find(name);
        if (itDevice == outputBindings.end() || !itDevice->second
            || itSize == outputSizes.end() || itSize->second == 0)
        {
            std::cerr << "[Detector] CUDA 图捕获中止：输出 " << name
                << " 缺少有效的设备缓冲。" << std::endl;
            captureCopiesOk = false;
            break;
        }

        const cudaError_t copyErr = cudaMemcpyAsync(
            itPinned->second, itDevice->second, itSize->second,
            cudaMemcpyDeviceToHost, stream);
        if (copyErr != cudaSuccess)
        {
            std::cerr << "[Detector] CUDA 图捕获中止：录制输出拷贝失败: "
                << cudaGetErrorString(copyErr) << std::endl;
            captureCopiesOk = false;
            break;
        }
    }

    if (!captureCopiesOk)
    {
        cudaGraph_t partialGraph = nullptr;
        const cudaError_t endErr = cudaStreamEndCapture(stream, &partialGraph);
        if (endErr == cudaSuccess && partialGraph)
            cudaGraphDestroy(partialGraph);
        cudaGraph = nullptr;
        return;
    }

    st = cudaStreamEndCapture(stream, &cudaGraph);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] EndCapture failed: "
            << cudaGetErrorString(st) << std::endl;
        return;
    }

    st = cudaGraphInstantiate(&cudaGraphExec, cudaGraph, 0);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] GraphInstantiate failed: "
            << cudaGetErrorString(st) << std::endl;
        cudaGraphDestroy(cudaGraph);
        cudaGraph = nullptr;
        return;
    }

    cudaGraphCaptured = true;
}

inline bool TrtDetector::launchCudaGraph()
{
    const cudaError_t err = cudaGraphLaunch(cudaGraphExec, stream);
    if (err != cudaSuccess)
    {
        std::cerr << "[Detector] GraphLaunch failed: " << cudaGetErrorString(err) << std::endl;
        return false;
    }
    return true;
}

void TrtDetector::getInputNames()
{
    inputNames.clear();
    inputSizes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
        {
            inputNames.emplace_back(name);
            if (config.verbose)
            {
                std::cout << "[Detector] Detected input: " << name << std::endl;
            }
        }
    }
}

void TrtDetector::getOutputNames()
{
    outputNames.clear();
    outputSizes.clear();
    outputTypes.clear();
    outputShapes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kOUTPUT)
        {
            outputNames.emplace_back(name);
            outputTypes[name] = engine->getTensorDataType(name);

            if (config.verbose)
            {
                std::cout << "[Detector] Detected output: " << name << std::endl;
            }
        }
    }
}

bool TrtDetector::getBindings()
{
    for (auto& binding : inputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    inputBindings.clear();

    for (auto& binding : outputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    outputBindings.clear();

    for (const auto& name : inputNames)
    {
        size_t size = inputSizes[name];
        if (size > 0)
        {
            void* ptr = nullptr;

            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                inputBindings[name] = ptr;
                if (config.verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for input " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate input memory: " << cudaGetErrorString(err) << std::endl;
                for (auto& allocated : inputBindings)
                    if (allocated.second) cudaFree(allocated.second);
                inputBindings.clear();
                return false;
            }
        }
    }

    for (const auto& name : outputNames)
    {
        size_t size = outputSizes[name];
        if (size > 0) {
            void* ptr = nullptr;
            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                outputBindings[name] = ptr;
                if (config.verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for output " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate output memory: " << cudaGetErrorString(err) << std::endl;
                for (auto& allocated : outputBindings)
                    if (allocated.second) cudaFree(allocated.second);
                outputBindings.clear();
                for (auto& allocated : inputBindings)
                    if (allocated.second) cudaFree(allocated.second);
                inputBindings.clear();
                return false;
            }
        }
    }

    // 【修复·静默失效】显式校验每个 I/O 张量都拿到了非空设备缓冲。
    // 若某张量 size 为 0（getElementSize 对 BOOL/INT64/BF16 等返回 0），
    // 上面的分配循环会跳过它，导致后续 setTensorAddress 绑定空指针。
    for (const auto& name : inputNames)
    {
        const auto it = inputBindings.find(name);
        if (it == inputBindings.end() || !it->second)
        {
            std::cerr << "[Detector] 输入张量 " << name
                << " 未获得有效设备缓冲（不支持的数据类型或尺寸为 0）。" << std::endl;
            return false;
        }
    }
    for (const auto& name : outputNames)
    {
        const auto it = outputBindings.find(name);
        if (it == outputBindings.end() || !it->second)
        {
            std::cerr << "[Detector] 输出张量 " << name
                << " 未获得有效设备缓冲（不支持的数据类型或尺寸为 0）。" << std::endl;
            return false;
        }
    }
    return true;
}

bool TrtDetector::initialize(const std::string& modelFile)
{
    // 【修复·陈旧 CUDA Graph】initialize() 可被重入（模型/分辨率变更时推理线程重调）。
    // captureCudaGraph() 内部有 `if (cudaGraphCaptured) return;` 的短路，
    // 若此处不先销毁旧图，重入后 cudaGraphCaptured 仍为 true，
    // 新引擎将继续执行捕获了**旧 device binding 指针**的图 —— 那些指针已被
    // getBindings() 释放，属 use-after-free。当前调用方恰好在外部先调了
    // destroyCudaGraph()，此处补齐是为了让本函数自身具备重入安全性。
    destroyCudaGraph();

    // 【修复 B·半初始化状态】initialize() 有 12 条 `return false` 提前退出路径，其中
    // 位于 context.reset(engine->createExecutionContext()) 之后的若干条（张量维度非法、
    // getBindings 失败、setTensorAddress 失败、cudaEventCreate 失败）退出时，
    // context 仍为非空。而 isInitialized() 的判据正是 `context != nullptr`，
    // 推理线程的守卫 `if (!context) { sleep; continue; }` 因此不会拦截，
    // 于是继续沿正常路径执行：
    //   - cudaEventRecord(preprocessStartEvent=nullptr, ...) 返回错误但被丢弃；
    //   - cudaEventSynchronize(copyCompleteEvent=nullptr) 同样失败 ——
    //     整条推理链路自此**没有任何同步点**，postProcess 读取的是尚未完成
    //     D2H 拷贝的 pinned 缓冲，产出随机检测框（表现为"凭空出现的瞄准目标"）。
    // 修复：用局部 RAII 守卫保证任何失败退出都把对象复位到"未初始化"，
    // 使 isInitialized()/`!context` 守卫恢复有效。C++ 允许成员函数内的局部类
    // 访问所在类的私有成员，故此处可直接复位私有状态。
    bool initSucceeded = false;
    struct InitFailureGuard
    {
        TrtDetector* self;
        const bool* succeeded;
        ~InitFailureGuard()
        {
            if (*succeeded)
                return;
            self->destroyCudaGraph();
            self->context.reset();
            self->engine.reset();
        }
    } initFailureGuard{ this, &initSucceeded };

    if (!stream)
    {
        cudaError_t err = cudaStreamCreate(&stream);
        if (err != cudaSuccess || !stream)
        {
            std::cerr << "[TensorRT] 创建CUDA流失败: " << cudaGetErrorString(err) << std::endl;
            return false;
        }
    }

    runtime.reset(nvinfer1::createInferRuntime(gLogger));
    loadEngine(modelFile);
    if (!engine)
    {
        std::cerr << "[Detector] Engine loading failed" << std::endl;
        return false;
    }

    context.reset(engine->createExecutionContext());
    if (!context)
    {
        std::cerr << "[Detector] Context creation failed" << std::endl;
        return false;
    }

    getInputNames();
    getOutputNames();
    if (inputNames.empty() || outputNames.empty())
    {
        std::cerr << "[TensorRT] Engine must contain at least one input and one output tensor." << std::endl;
        return false;
    }
    inputName = inputNames[0];

    nvinfer1::Dims modelInputDims = engine->getTensorShape(inputName.c_str());
    if (modelInputDims.nbDims != 4)
    {
        std::cerr << "[TensorRT] Engine input must be a 4D NCHW tensor; got rank "
                  << modelInputDims.nbDims << "." << std::endl;
        return false;
    }
    bool isStatic = true;
    for (int i = 0; i < modelInputDims.nbDims; ++i)
        if (modelInputDims.d[i] <= 0) isStatic = false;

    // 【修复 G·无锁写共享配置】fixed_input_size 由 UI 线程（持 configMutex）读写，
    // 此处在推理线程无锁改写属数据竞争。改为持锁写；读取 detection_resolution
    // 一并在同一临界区取快照，避免同一函数内前后取到不一致的值。
    int targetWidth = 0;
    int targetHeight = 0;
    int staticWorkingWidth = 0;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        if (isStatic != config.fixed_input_size)
        {
            config.fixed_input_size = isStatic;
        }
        staticWorkingWidth = config.detection_resolution;
        bool forceValid = config.force_model_input_size
            && Config::normalizeModelInputSize(config.force_model_input_width,
                                               config.force_model_input_height);
        targetWidth = forceValid ? config.force_model_input_width : config.detection_resolution;
        targetHeight = forceValid ? config.force_model_input_height : config.detection_resolution;
    }
    nvinfer1::Dims inputDims = modelInputDims;
    if (!isStatic)
    {
        nvinfer1::Dims4 newShape{ 1, 3, targetHeight, targetWidth };
        context->setInputShape(inputName.c_str(), newShape);
        if (!context->allInputDimensionsSpecified())
        {
            std::cerr << "[Detector] Failed to set input dimensions" << std::endl;
            return false;
        }
        inputDims = context->getTensorShape(inputName.c_str());
    }
    else
    {
        inputDims = context->getTensorShape(inputName.c_str());
    }

    inputSizes.clear();
    outputSizes.clear();
    outputShapes.clear();
    outputTypes.clear();
    fp16OutputScratch.clear();

    for (const auto& inName : inputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(inName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(inName.c_str());
        inputSizes[inName] = getSizeByDim(d) * getElementSize(dt);
    }
    for (const auto& outName : outputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(outName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(outName.c_str());
        outputSizes[outName] = getSizeByDim(d) * getElementSize(dt);
        std::vector<int64_t> shape(d.nbDims);
        for (int j = 0; j < d.nbDims; ++j) shape[j] = d.d[j];
        outputShapes[outName] = std::move(shape);
        outputTypes[outName] = dt;
    }

    // 【修复·静默失效】绑定分配失败必须中止初始化，
    // 否则 setTensorAddress 会绑定空指针，enqueueV3 对空设备地址读写。
    if (!getBindings())
    {
        std::cerr << "[Detector] 设备绑定分配失败，初始化中止。" << std::endl;
        return false;
    }

    allocatePinnedOutputs();

    if (!outputNames.empty())
    {
        const std::string& mainOut = outputNames[0];
        const auto& shape = outputShapes[mainOut];
        numClasses = InferYoloClassCountFromShape(shape);

        // 端到端 NMS 引擎输出形如 [1,300,6]，6 列不含类别数；
        // 此时读取构建期内嵌的 ONNX 元数据（nc/names）作为类别数来源。
        if (numClasses <= 0)
        {
            std::filesystem::path metadataPath(modelFile);
            if (metadataPath.extension() == ".onnx")
                metadataPath.replace_extension(".engine");
            numClasses = InferClassCountFromEmbeddedMetadata(
                readEngineEmbeddedMetadata(metadataPath.string()));
        }

        if (numClasses <= 0 || numClasses > Config::MAX_MODEL_CLASSES)
        {
            std::cerr << "[Detector] 无法从模型输出张量或内嵌元数据推断有效类别数 (NC="
                      << numClasses << ")。输出形状: " << FormatTensorShape(shape) << std::endl;
            std::cerr << "[Detector] 类别数必须为 1.." << Config::MAX_MODEL_CLASSES
                      << "。请重新导出 ONNX 并删除旧 .engine 后重试。" << std::endl;
            return false;
        }

        if (numClasses > Config::FIXED_TARGET_CLASS_COUNT)
        {
            std::cout << "[Detector] 模型类别数 NC=" << numClasses
                      << " 大于当前类别开关上限 " << Config::FIXED_TARGET_CLASS_COUNT
                      << "，超出部分将被类别过滤丢弃。" << std::endl;
        }
    }

    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(inputDims.d[1], &c)
        || !tryGetPositiveDimInt(inputDims.d[2], &h)
        || !tryGetPositiveDimInt(inputDims.d[3], &w))
    {
        std::cerr << "[Detector] Invalid input dimensions" << std::endl;
        return false;
    }

    // 使用上文同一临界区取到的尺寸快照，保证与 setInputShape 所用值严格一致。
    // 静态引擎无法强制改 shape，因此 static 分支按模型真实输入尺寸保存。
    modelInputWidth = w;
    modelInputHeight = h;
    const int workingWidth = isStatic ? staticWorkingWidth : targetWidth;
    img_scale = static_cast<float>(workingWidth) / w;

    std::cout << "\n========== TensorRT 模型摘要 ==========" << std::endl;
    std::cout << "加载来源路径:" << std::endl;
    // modelFile 为本地代码页(GBK)窄字符串，显示时转为 UTF-8。
    std::cout << std::filesystem::path(modelFile).u8string() << std::endl;
    std::cout << "已验证的 TensorRT engine I/O:" << std::endl;
    std::cout << "输入张量:" << std::endl;
    for (const auto& inName : inputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(inName.c_str());
        std::vector<int64_t> shape(d.nbDims);
        for (int j = 0; j < d.nbDims; ++j) shape[j] = d.d[j];
        std::cout << "  " << inName << " " << FormatTensorShape(shape)
                  << " " << TensorDataTypeName(engine->getTensorDataType(inName.c_str())) << std::endl;
    }
    std::cout << "输出张量:" << std::endl;
    for (const auto& outputName : outputNames)
        std::cout << "  " << outputName << " " << FormatTensorShape(outputShapes[outputName])
                  << " " << TensorDataTypeName(outputTypes[outputName]) << std::endl;
    std::cout << "输入尺寸模式: " << (isStatic ? "固定尺寸" : "动态尺寸") << std::endl;

    cpuResizedBuffer.create(h, w, CV_8UC3);
    cpuFloatBuffer.create(h, w, CV_32FC3);
    inputHostBuffer.resize(static_cast<size_t>(c) * static_cast<size_t>(h) * static_cast<size_t>(w));

    // 【修复·未校验绑定】setTensorAddress 返回 false 表示地址未被 TensorRT 接受
    // （空指针、对齐不满足、张量名不存在）。原实现丢弃返回值，失败后 enqueueV3
    // 会使用未设置的张量地址。同时改用 find() 避免 operator[] 在键缺失时
    // 静默插入 nullptr。
    for (const auto& n : inputNames)
    {
        const auto it = inputBindings.find(n);
        if (it == inputBindings.end() || !context->setTensorAddress(n.c_str(), it->second))
        {
            std::cerr << "[Detector] 设置输入张量地址失败: " << n << std::endl;
            return false;
        }
    }
    for (const auto& n : outputNames)
    {
        const auto it = outputBindings.find(n);
        if (it == outputBindings.end() || !context->setTensorAddress(n.c_str(), it->second))
        {
            std::cerr << "[Detector] 设置输出张量地址失败: " << n << std::endl;
            return false;
        }
    }

    if (preprocessStartEvent) cudaEventDestroy(preprocessStartEvent);
    if (inferenceStartEvent) cudaEventDestroy(inferenceStartEvent);
    if (inferenceCompleteEvent) cudaEventDestroy(inferenceCompleteEvent);
    if (copyCompleteEvent) cudaEventDestroy(copyCompleteEvent);

    preprocessStartEvent = nullptr;
    inferenceStartEvent = nullptr;
    inferenceCompleteEvent = nullptr;
    copyCompleteEvent = nullptr;

    // 【修复·未检查 CUDA 错误】原实现丢弃 cudaEventCreate 返回值；
    // 事件创建失败时句柄保持为空，后续 cudaEventRecord/Synchronize 静默失败，
    // 推理线程将不再有任何同步点（读取到未完成的输出缓冲），
    // 且 cudaEventElapsedTime 输出未定义值污染性能面板。
    {
        // 【修复 E·推理线程 CPU 自旋】copyCompleteEvent 是整条链路上**唯一被
        // cudaEventSynchronize 等待**的事件。CUDA 默认的同步策略为
        // cudaDeviceScheduleAuto：当活动线程数不超过 CPU 逻辑核数时（本工程稳定为
        // 5 个线程），运行时选择**忙等自旋**。这意味着推理线程在每帧等待
        // GPU 完成推理+回传的整个窗口（典型 3~8 ms）内 100% 占满一个逻辑核，
        // 在 120~240 FPS 目标下即长期烧掉一整颗核心，与"低 CPU 占用"目标直接冲突，
        // 也挤占了鼠标控制线程与采集线程的调度配额。
        // 修复：仅对该事件使用 cudaEventBlockingSync 标志，使等待方改为在
        // 操作系统信号量上阻塞。这是**逐事件**的属性，不调用 cudaSetDeviceFlags，
        // 因此不影响其他线程（采集线程的 cudaStreamSynchronize 仍保持原策略）。
        // 代价是唤醒延迟约 10~50 µs，相对 3~8 ms 的推理耗时可忽略；
        // 其余三个事件仅用于 cudaEventElapsedTime 计时、从不被等待，保持默认创建。
        // 注意：cudaEventBlockingSync 不含 cudaEventDisableTiming，计时功能不受影响。
        const cudaError_t eventErr[4] = {
            cudaEventCreate(&preprocessStartEvent),
            cudaEventCreate(&inferenceStartEvent),
            cudaEventCreate(&inferenceCompleteEvent),
            cudaEventCreateWithFlags(&copyCompleteEvent, cudaEventBlockingSync)
        };
        for (const cudaError_t e : eventErr)
        {
            if (e != cudaSuccess)
            {
                std::cerr << "[Detector] 创建 CUDA 事件失败: " << cudaGetErrorString(e) << std::endl;
                return false;
            }
        }
    }

    useCudaGraph = config.use_cuda_graph;
    if (useCudaGraph)
    {
        captureCudaGraph();
    }

    if (config.verbose)
    {
        std::cout << "[Detector] Initialized. ModelStatic=" << std::boolalpha << isStatic
            << ", NetInput=" << h << "x" << w << " (scale=" << img_scale << ")" << std::endl;
    }

    initSucceeded = true;  // 【修复 B】仅此处置位，其余所有退出路径均由守卫复位对象状态。
    return true;
}

size_t TrtDetector::getSizeByDim(const nvinfer1::Dims& dims)
{
    // 【修复 G·整数回绕】原实现直接连乘且无溢出检查。畸形或被篡改的 engine 可给出
    // 极大维度（如 2^40 × 2^30），乘积在 size_t 上回绕成一个很小的值，
    // 后续 cudaMalloc 只申请到很小的缓冲，而 TensorRT 按真实张量尺寸读写
    // → 设备端堆越界。此处在每步乘法前做上溢判定，溢出即返回 0，
    // 由调用方（getBindings 的空缓冲校验）走既有的初始化失败路径。
    // 合法尺寸下的计算结果与原实现逐位一致。
    constexpr size_t kMaxSize = std::numeric_limits<size_t>::max();
    size_t size = 1;
    for (int i = 0; i < dims.nbDims; ++i)
    {
        if (dims.d[i] < 0) return 0;
        const size_t dim = static_cast<size_t>(dims.d[i]);
        if (dim != 0 && size > kMaxSize / dim)
            return 0;
        size *= dim;
    }
    return size;
}

size_t TrtDetector::getElementSize(nvinfer1::DataType dtype)
{
    switch (dtype)
    {
    case nvinfer1::DataType::kFLOAT: return 4;
    case nvinfer1::DataType::kHALF: return 2;
    case nvinfer1::DataType::kINT32: return 4;
    case nvinfer1::DataType::kINT8: return 1;
    default: return 0;
    }
}

void TrtDetector::loadEngine(const std::string& modelFile)
{
    std::string engineFilePath;
    std::filesystem::path modelPath(modelFile);
    std::string extension = modelPath.extension().string();

    if (extension == ".engine")
    {
        engineFilePath = modelFile;

        // Try loading the engine file
        std::cout << "\n========== TensorRT 引擎 ==========" << std::endl;
        std::cout << "正在加载引擎:" << std::endl;
        // engineFilePath 为本地代码页(GBK)窄字符串，显示时转为 UTF-8 避免控制台乱码（文件读写仍用原窄字符串）。
    std::cout << std::filesystem::path(engineFilePath).u8string() << std::endl;
        engine.reset(loadEngineFromFile(engineFilePath, runtime.get()));

        // 【功能·模型信息内嵌】引擎加载成功后检查是否携带内嵌模型信息；
        // 缺失且存在同目录 .onnx 时自动重建一次，把 ONNX 自定义元数据补进
        // engine 尾部。此后即使删除 .onnx，程序仍能推断模型信息
        //（类别数 / 分辨率 / names / 版本）。重建仅在缺失时触发一次，耗时较长。
        if (engine && readEngineEmbeddedMetadata(engineFilePath).empty())
        {
            std::filesystem::path onnxPath = modelPath;
            onnxPath.replace_extension(".onnx");
            if (fileExists(onnxPath.string()))
            {
                std::cout << "[TensorRT] 引擎缺少内嵌模型信息，将使用 ONNX 自动重建以嵌入（仅一次，耗时较长）..."
                    << std::endl;
                nvinfer1::ICudaEngine* rebuilt = buildEngineFromOnnx(onnxPath.string(), gLogger);
                if (rebuilt)
                {
                    delete rebuilt;
                    engine.reset(loadEngineFromFile(engineFilePath, runtime.get()));
                    if (engine)
                        std::cout << "[TensorRT] 引擎重建完成，已嵌入模型信息。" << std::endl;
                    else
                        std::cerr << "[TensorRT] 重建后重新加载失败，模型不可用。" << std::endl;
                }
                else
                {
                    std::cerr << "[TensorRT] 自动重建引擎失败，继续使用旧引擎。" << std::endl;
                }
            }
        }
        // 【功能·引擎绑定维度补充】自动重建之后若仍缺内嵌信息（无 .onnx 或重建失败），
        // 直接从已加载的 ICudaEngine 读取输入/输出张量形状，追加为精简嵌入块，
        // 使旧引擎在没有 .onnx 时至少能显示模型分辨率（类别数等仍依赖 onnx/嵌入块）。
        if (engine && readEngineEmbeddedMetadata(engineFilePath).empty())
        {
            const std::string bindingMeta = buildEngineBindingMetadata(engine.get());
            if (appendEmbeddedMetadataBlock(engineFilePath, bindingMeta))
                std::cout << "[TensorRT] 已从引擎本身补充输入/输出形状信息。" << std::endl;
        }

        // If engine loading failed (e.g. compute capability mismatch),
        // try to rebuild from the corresponding .onnx file if available
        if (!engine)
        {
            std::filesystem::path onnxPath = modelPath;
            onnxPath.replace_extension(".onnx");
            if (fileExists(onnxPath.string()))
            {
                std::cout << "[TensorRT] 引擎加载失败，检测到 ONNX 模型，正在重新编译..." << std::endl;
                std::cout << "[TensorRT] 原因：引擎文件为不同 GPU 编译，当前 GPU 需要重新编译。" << std::endl;

                // Delete the incompatible engine file
                try
                {
                    std::filesystem::remove(modelPath);
                    std::cout << "[TensorRT] 已删除不兼容的引擎文件。" << std::endl;
                }
                catch (...)
                {
                    std::cerr << "[TensorRT] 无法删除旧引擎文件，将覆盖写入。" << std::endl;
                }

                nvinfer1::ICudaEngine* builtEngine = buildEngineFromOnnx(onnxPath.string(), gLogger);
                if (builtEngine)
                {
                    // 【修复 F·重复序列化 + 重复落盘】buildEngineFromOnnx 内部已把
                    // buildSerializedNetwork 得到的 plan 字节写入同一个 <base>.engine 路径
                    //（nvinf.cpp: engineFile = onnxFile 去扩展名 + ".engine"，与此处的
                    // engineFilePath 逐字符相同），并且写失败时返回 nullptr。
                    // 原实现在此又对同一引擎调用 serialize() 并把结果覆盖写入同一文件：
                    //   1) 对 50~200MB 的引擎多做一次完整序列化，额外耗时 0.3~3 s；
                    //   2) 峰值内存多占一份完整引擎字节；
                    //   3) 同一文件在短时间内被写两次，中间存在可被其他进程读到的半截状态。
                    // 移除该冗余路径后，落盘内容仍是等价的合法引擎（plan 原始字节，更为规范），
                    // 后续 loadEngineFromFile 的加载结果不变。
                    delete builtEngine;

                    PersistSelectedEngineModel(engineFilePath);

                    // Reload the newly built engine
                    engine.reset(loadEngineFromFile(engineFilePath, runtime.get()));
                }
                else
                {
                    std::cerr << "[TensorRT] 从 ONNX 编译引擎失败。" << std::endl;
                }
            }
            else
            {
                std::cerr << "[TensorRT] 引擎加载失败，且未找到对应的 .onnx 文件用于重建。" << std::endl;
                std::cerr << "[TensorRT] 请确保 models/ 目录中有对应的 .onnx 模型文件。" << std::endl;
            }
        }
        return;
    }
    else if (extension == ".onnx")
    {
        engineFilePath = modelPath.replace_extension(".engine").string();

        if (!fileExists(engineFilePath))
        {
            std::cout << "\n========== TensorRT 引擎 ==========" << std::endl;
            std::cout << "正在从 ONNX 模型编译 TensorRT 引擎:" << std::endl;
            // modelFile 为本地代码页(GBK)窄字符串，显示时转为 UTF-8。
    std::cout << std::filesystem::path(modelFile).u8string() << std::endl;

            nvinfer1::ICudaEngine* builtEngine = buildEngineFromOnnx(modelFile, gLogger);
            if (builtEngine)
            {
                // 【修复 F·重复序列化 + 重复落盘】理由同 .engine 分支：
                // buildEngineFromOnnx 已将 plan 写入同一 engineFilePath 并在写失败时返回
                // nullptr，此处不再重复 serialize() 覆盖写入。
                delete builtEngine;

                PersistSelectedEngineModel(engineFilePath);
            }
        }
    }
    else
    {
        std::cerr << "[TensorRT] 不支持的模型格式: " << extension << std::endl;
        return;
    }

    std::cout << "正在加载引擎:" << std::endl;
    // engineFilePath 为本地代码页(GBK)窄字符串，显示时转为 UTF-8 避免控制台乱码（文件读写仍用原窄字符串）。
    std::cout << std::filesystem::path(engineFilePath).u8string() << std::endl;
    engine.reset(loadEngineFromFile(engineFilePath, runtime.get()));
}

void TrtDetector::processFrame(
    const cv::Mat& detection_frame,
    const cv::Mat& source_frame,
    std::chrono::steady_clock::time_point frameTimestamp)
{
    thread_local int firstCallCount = 0;
    if (config.verbose && ++firstCallCount <= 3)
        std::cout << "[Detector] processFrame 入口 #" << firstCallCount << " (detectionPaused=" << (detectionPaused ? "true" : "false") << ")" << std::endl;

    if (detectionPaused)
    {
        detectionBuffer.clear();
        return;
    }

    thread_local int processFrameCount = 0;
    if (config.verbose && ++processFrameCount % 60 == 1)
        std::cout << "[Detector] processFrame called (count=" << processFrameCount << ") frame=" << detection_frame.cols << "x" << detection_frame.rows << std::endl;

    std::unique_lock<std::mutex> lock(inferenceMutex);
    if (frameReady)
        overwrittenFrameCount.fetch_add(1, std::memory_order_relaxed);
    currentFrame = detection_frame;
    currentSourceFrame = source_frame.empty() ? detection_frame : source_frame;
    currentFrameGpu.release();
    currentFrameTimestamp = (frameTimestamp.time_since_epoch().count() != 0)
        ? frameTimestamp
        : std::chrono::steady_clock::now();
    pendingFrameType = PendingFrameType::Cpu;
    frameReady = true;
    submittedFrameSequence.fetch_add(1, std::memory_order_relaxed);
    inferenceCV.notify_one();
}

void TrtDetector::processFrameGpu(
    const cv::cuda::GpuMat& frame,
    std::chrono::steady_clock::time_point frameTimestamp)
{
    if (detectionPaused)
    {
        detectionBuffer.clear();
        return;
    }

    std::unique_lock<std::mutex> lock(inferenceMutex);
    if (frameReady)
        overwrittenFrameCount.fetch_add(1, std::memory_order_relaxed);
    currentFrame.release();
    currentSourceFrame.release();
    currentFrameGpu = frame;
    currentFrameTimestamp = (frameTimestamp.time_since_epoch().count() != 0)
        ? frameTimestamp
        : std::chrono::steady_clock::now();
    pendingFrameType = PendingFrameType::Gpu;
    frameReady = true;
    submittedFrameSequence.fetch_add(1, std::memory_order_relaxed);
    inferenceCV.notify_one();
}

std::vector<Detection> TrtDetector::detect(const cv::Mat& frame)
{
    std::vector<Detection> latestDetections;
    if (!context || frame.empty())
        return latestDetections;

    cudaEventRecord(preprocessStartEvent, stream);
    // 【修复·脏输入推理】预处理失败时不得继续 enqueueV3（否则用残留输入推理）。
    if (!preProcess(frame))
        return latestDetections;
    cudaEventRecord(inferenceStartEvent, stream);

    const bool usedGraph = useCudaGraph && cudaGraphCaptured;
    // 【修复 D】与 inferenceThread() 同构：原实现丢弃 enqueueV3 / cudaMemcpyAsync /
    // cudaEventSynchronize 的返回值，失败时会解码上一帧残留的 pinned 数据。
    bool downloadOk = true;
    if (usedGraph)
    {
        // 【修复·图启动失败被吞】与 inferenceThread() 同构：launchCudaGraph 失败时
        // 图内录制的 D2H 拷贝未执行，pinned 仍为上一帧数据，必须按失败处理并跳过
        // 后续事件同步，否则会解码陈旧结果产出幽灵检测框。
        if (!launchCudaGraph())
        {
            downloadOk = false;
        }
        else
        {
            cudaEventRecord(copyCompleteEvent, stream);
            if (cudaEventSynchronize(copyCompleteEvent) != cudaSuccess)
                downloadOk = false;
        }
    }
    else if (!context->enqueueV3(stream))
    {
        downloadOk = false;
    }
    else
    {
        cudaEventRecord(inferenceCompleteEvent, stream);

        for (const auto& name : outputNames)
        {
            const auto itPinned = pinnedOutputBuffers.find(name);
            if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
                continue;

            // find() 取代 operator[]，避免键缺失时静默插入 nullptr / 0。
            const auto itDevice = outputBindings.find(name);
            const auto itSize = outputSizes.find(name);
            if (itDevice == outputBindings.end() || !itDevice->second
                || itSize == outputSizes.end() || itSize->second == 0)
            {
                downloadOk = false;
                break;
            }

            if (cudaMemcpyAsync(
                    itPinned->second,
                    itDevice->second,
                    itSize->second,
                    cudaMemcpyDeviceToHost,
                    stream) != cudaSuccess)
            {
                downloadOk = false;
                break;
            }
        }

        cudaEventRecord(copyCompleteEvent, stream);
        if (cudaEventSynchronize(copyCompleteEvent) != cudaSuccess)
            downloadOk = false;
    }

    if (!downloadOk)
    {
        std::cerr << "[Detector] 推理或输出回传失败，本帧不产出检测结果。" << std::endl;
        cudaGetLastError();
        return latestDetections;
    }

    auto tPostStart = std::chrono::steady_clock::now();

    for (const auto& name : outputNames)
    {
        const auto itPinned = pinnedOutputBuffers.find(name);
        if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
            continue;

        nvinfer1::DataType dtype = outputTypes[name];
        if (dtype == nvinfer1::DataType::kHALF)
        {
            const size_t numElements = outputSizes[name] / sizeof(__half);

            auto& outputDataFloat = fp16OutputScratch[name];
            if (outputDataFloat.size() != numElements)
                outputDataFloat.resize(numElements);

            ConvertHalfToFloat(itPinned->second, outputDataFloat.data(), numElements);

            NmsTelemetry nmsTelemetry;
            latestDetections = postProcess(outputDataFloat.data(), name, &lastNmsTime, &nmsTelemetry);
            lastPreLimitCount.store(nmsTelemetry.preLimitCount, std::memory_order_relaxed);
            lastPreNmsCount.store(nmsTelemetry.preNmsCount, std::memory_order_relaxed);
            lastPostNmsCount.store(nmsTelemetry.postNmsCount, std::memory_order_relaxed);
        }
        else if (dtype == nvinfer1::DataType::kFLOAT)
        {
            const float* floatPtr = reinterpret_cast<const float*>(itPinned->second);
            NmsTelemetry nmsTelemetry;
            latestDetections = postProcess(floatPtr, name, &lastNmsTime, &nmsTelemetry);
            lastPreLimitCount.store(nmsTelemetry.preLimitCount, std::memory_order_relaxed);
            lastPreNmsCount.store(nmsTelemetry.preNmsCount, std::memory_order_relaxed);
            lastPostNmsCount.store(nmsTelemetry.postNmsCount, std::memory_order_relaxed);
        }
    }

    auto tPostEnd = std::chrono::steady_clock::now();

    float preprocessMs = 0.0f;
    float inferenceMs = 0.0f;
    float copyMs = 0.0f;

    cudaEventElapsedTime(&preprocessMs, preprocessStartEvent, inferenceStartEvent);
    cudaEventElapsedTime(&inferenceMs, inferenceStartEvent, inferenceCompleteEvent);
    cudaEventElapsedTime(&copyMs, inferenceCompleteEvent, copyCompleteEvent);

    lastPreprocessTime = std::chrono::duration<double, std::milli>(preprocessMs);
    lastInferenceTime = std::chrono::duration<double, std::milli>(inferenceMs);
    lastCopyTime = std::chrono::duration<double, std::milli>(copyMs);
    lastPostprocessTime = tPostEnd - tPostStart;
    lastPreprocessTimeMs.store(lastPreprocessTime.count(), std::memory_order_relaxed);
    lastInferenceTimeMs.store(lastInferenceTime.count(), std::memory_order_relaxed);
    lastCopyTimeMs.store(lastCopyTime.count(), std::memory_order_relaxed);
    lastPostprocessTimeMs.store(lastPostprocessTime.count(), std::memory_order_relaxed);
    lastNmsTimeMs.store(lastNmsTime.count(), std::memory_order_relaxed);

    return latestDetections;
}

void TrtDetector::inferenceThread()
{
    while (!shouldExit)
    {
        // 【修复·数据竞争 UB】原实现在此无锁读取 config.backend（std::string）。
        // overlay 渲染线程在 draw_ai() 中**每帧无条件执行** `config.backend = "TRT";`
        // （overlay/draw_ai.cpp:74），并发的读/写 std::string 是数据竞争：
        // 一旦字符串超出 SSO 容量转为堆存储，赋值会释放旧缓冲，读侧即 use-after-free。
        // config.ai_model 同理（下方模型路径解析处原亦为无锁读）。
        // 此处一次性取快照：循环体其余部分改用快照值，锁区间为常数级。
        std::string backendSnapshot;
        std::string aiModelSnapshotForInit;
        bool useCudaGraphSnapshot = false;
        {
            std::lock_guard<std::mutex> lock(configMutex);
            backendSnapshot = config.backend;
            aiModelSnapshotForInit = config.ai_model;
            useCudaGraphSnapshot = config.use_cuda_graph;
        }

        if (backendSnapshot != "TRT")
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (detector_model_changed.load())
        {
            std::cout << "[Detector] 模型变更触发推理线程重新初始化..." << std::endl;
            {
                std::unique_lock<std::mutex> lock(inferenceMutex);
                destroyCudaGraph();
                context.reset();
                engine.reset();

                freePinnedOutputs();

                for (auto& binding : inputBindings)
                    if (binding.second) cudaFree(binding.second);
                inputBindings.clear();
                for (auto& binding : outputBindings)
                    if (binding.second) cudaFree(binding.second);
                outputBindings.clear();

                currentFrame.release();
                currentSourceFrame.release();
                currentFrameGpu.release();
                frameReady = false;
                pendingFrameType = PendingFrameType::None;
            }
            std::error_code modelPathEc;
            const std::filesystem::path modelPath = std::filesystem::absolute(std::filesystem::path("models") / std::filesystem::path(aiModelSnapshotForInit).filename(), modelPathEc).lexically_normal();
            if (modelPathEc)
            {
                std::cerr << "[Detector] 模型路径解析失败: " << modelPathEc.message() << std::endl;
                detector_model_changed.store(false);
                continue;
            }
            // 【修复 B·配套】检查返回值。失败时 initialize() 内的守卫已把 context/engine
            // 复位为空，下方 `if (!context)` 分支会以 100ms 节流打印并等待用户
            // 重新选择模型，而不是带着空事件句柄继续跑"无同步点的推理"。
            if (!initialize(modelPath.string()))
            {
                std::cerr << "[Detector] 模型重新初始化失败，检测器保持未初始化状态："
                    << std::endl;
                std::cerr << modelPath.u8string() << std::endl;
            }
            else
            {
                std::cout << "[Detector] 推理线程重新初始化完成。" << std::endl;
            }
            // 【功能·模型信息刷新】模型切换后重新推断模型信息并发布，
            // 使 UI 的模型摘要/类别数即时更新（不再停留在启动时的一次性缓存）。
            // inspectLoadedEngineOnnx 在 .onnx 缺失时会回退到 .engine 内嵌元数据。
            {
                int refreshResolution = 640;
                {
                    std::lock_guard<std::mutex> lock(configMutex);
                    refreshResolution = config.detection_resolution;
                }
                const StartupOnnxReport refreshReport =
                    inspectLoadedEngineOnnx(modelPath.string(), refreshResolution);
                publishStartupOnnxReport(refreshReport);
                {
                    std::lock_guard<std::mutex> lock(configMutex);
                    if (refreshReport.width > 0)
                        config.model_input_width = refreshReport.width;
                    if (refreshReport.height > 0)
                        config.model_input_height = refreshReport.height;
                    const int actualW = (refreshReport.width > 0)
                        ? refreshReport.width
                        : modelInputWidth;
                    if (actualW > 0)
                        config.detection_resolution = actualW;
                }
            }
            detection_resolution_changed.store(true);
            detector_model_changed.store(false);
        }

        if (useCudaGraph != useCudaGraphSnapshot)
        {
            useCudaGraph = useCudaGraphSnapshot;
            if (!useCudaGraph)
            {
                destroyCudaGraph();
            }
            else if (context)
            {
                captureCudaGraph();
            }
        }

        cv::Mat frame;
        cv::Mat sourceFrame;
        cv::cuda::GpuMat frameGpu;
        std::chrono::steady_clock::time_point frameTimestamp{};
        PendingFrameType frameType = PendingFrameType::None;
        bool hasNewFrame = false;

        {
            std::unique_lock<std::mutex> lock(inferenceMutex);
            if (!frameReady && !shouldExit)
                inferenceCV.wait(lock, [this] { return frameReady || shouldExit; });

            if (shouldExit) break;

            if (frameReady)
            {
                frameType = pendingFrameType;
                frameTimestamp = currentFrameTimestamp;
                if (frameType == PendingFrameType::Gpu)
                {
                    frameGpu = currentFrameGpu;
                    currentFrameGpu.release();
                    currentFrame.release();
                    currentSourceFrame.release();
                }
                else
                {
                    frame = std::move(currentFrame);
                    sourceFrame = std::move(currentSourceFrame);
                    currentFrameGpu.release();
                }
                pendingFrameType = PendingFrameType::None;
                frameReady = false;
                hasNewFrame = true;
            }
        }

        if (!context)
        {
            bool expected = false;
            if (error_logged.compare_exchange_strong(expected, true, std::memory_order_relaxed))
            {
                std::cerr << "[Detector] Context not initialized" << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        else
        {
            error_logged.store(false, std::memory_order_relaxed);
        }

        if (hasNewFrame)
        {
            const bool hasCpuFrame = (frameType == PendingFrameType::Cpu && !frame.empty());
            const bool hasGpuFrame = (frameType == PendingFrameType::Gpu && !frameGpu.empty());
            if (!hasCpuFrame && !hasGpuFrame)
                continue;

            try
            {
                cudaEventRecord(preprocessStartEvent, stream);
                // 【修复·脏输入推理】CPU 分支原为 void 调用，预处理失败时仍继续
                // enqueueV3，使用设备端上一帧残留数据推理，产出错误检测框
                // （表现为"目标已消失但仍在瞄准旧位置"）。现两分支统一判定。
                const bool preprocessReady = hasGpuFrame ? preProcess(frameGpu) : preProcess(frame);
                if (!preprocessReady)
                {
                    // 预处理失败同样计入连续失败：H2D 拷贝/尺寸转换失败与推理失败
                    // 一样可能源自已永久失效的 CUDA 上下文，需要走同一套自恢复。
                    noteInferenceOutcome(false);
                    continue;
                }
                cudaEventRecord(inferenceStartEvent, stream);
                bool usedGraph = useCudaGraph && cudaGraphCaptured;
                // 【修复 D·静默失败 → 幽灵检测框】原实现丢弃了 enqueueV3、
                // cudaMemcpyAsync、cudaEventSynchronize 的全部返回值。
                // 一旦其中任何一步失败（显存紧张、TDR 后上下文失效、绑定表缺项），
                // pinned 缓冲仍保留**上一帧**的推理结果，而后处理照常执行，
                // 于是持续产出早已消失的目标框——用户侧表现为"目标不在了还在自瞄"。
                // 现改为逐步判定：任一步失败即跳过本帧后处理，
                // detectionBuffer 保持上一帧内容不被污染。
                bool downloadOk = true;
                if (usedGraph)
                {
                    // 【修复·图启动失败被吞 → 幽灵检测框】图内录制了输出 D2H 拷贝，
                    // launchCudaGraph 失败意味着这些拷贝未执行，pinned 缓冲仍是上一帧
                    // 数据。若此处忽略其返回值继续，随后对空流的事件同步很可能成功，
                    // downloadOk 保持真 → 解码陈旧结果、noteInferenceOutcome(true) 又
                    // 清零失败计数，使持久性故障下的自恢复永不触发。故启动失败即按
                    // 失败处理，且跳过后续事件同步（无新工作可等）。
                    if (!launchCudaGraph())
                    {
                        downloadOk = false;
                    }
                    else
                    {
                        cudaEventRecord(copyCompleteEvent, stream);
                        const cudaError_t syncErr = cudaEventSynchronize(copyCompleteEvent);
                        if (syncErr != cudaSuccess)
                        {
                            std::cerr << "[Detector] 等待推理完成失败(图模式): "
                                << cudaGetErrorString(syncErr) << std::endl;
                            downloadOk = false;
                        }
                    }
                }
                else
                {
                    if (!context->enqueueV3(stream))
                    {
                        std::cerr << "[Detector] enqueueV3 提交失败，跳过本帧。" << std::endl;
                        downloadOk = false;
                    }
                    else
                    {
                        cudaEventRecord(inferenceCompleteEvent, stream);

                        for (const auto& name : outputNames)
                        {
                            const auto itPinned = pinnedOutputBuffers.find(name);
                            if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
                                continue;

                            // 用 find() 取代 outputBindings[name] / outputSizes[name]：
                            // operator[] 在键缺失时会静默插入 nullptr / 0，
                            // 随后 cudaMemcpyAsync 从空设备指针拷贝并被忽略错误。
                            const auto itDevice = outputBindings.find(name);
                            const auto itSize = outputSizes.find(name);
                            if (itDevice == outputBindings.end() || !itDevice->second
                                || itSize == outputSizes.end() || itSize->second == 0)
                            {
                                std::cerr << "[Detector] 输出 " << name
                                    << " 缺少有效设备缓冲，跳过本帧。" << std::endl;
                                downloadOk = false;
                                break;
                            }

                            const cudaError_t copyErr = cudaMemcpyAsync(
                                itPinned->second,
                                itDevice->second,
                                itSize->second,
                                cudaMemcpyDeviceToHost,
                                stream
                            );
                            if (copyErr != cudaSuccess)
                            {
                                std::cerr << "[Detector] 输出回传失败: "
                                    << cudaGetErrorString(copyErr) << std::endl;
                                downloadOk = false;
                                break;
                            }
                        }

                        cudaEventRecord(copyCompleteEvent, stream);
                        const cudaError_t syncErr = cudaEventSynchronize(copyCompleteEvent);
                        if (syncErr != cudaSuccess)
                        {
                            std::cerr << "[Detector] 等待输出回传失败: "
                                << cudaGetErrorString(syncErr) << std::endl;
                            downloadOk = false;
                        }
                    }
                }

                if (!downloadOk)
                {
                    // 保守失败：不解码可能陈旧/未完成的 pinned 数据。
                    // 清空粘滞错误，避免污染后续 CUDA 调用的返回值判定。
                    cudaGetLastError();
                    // 记录一次失败。若故障是持久性的（TDR 后上下文失效、显存被
                    // 其它进程耗尽等），这里累积到阈值后会触发引擎重初始化，
                    // 而不是像原实现那样无限跳帧、永远停留在"跑着但不检测"的僵尸态。
                    noteInferenceOutcome(false);
                    continue;
                }

                // 本帧推理与回传均成功：清零连续失败计数与恢复预算。
                noteInferenceOutcome(true);

                // Post-processing (CPU)
                auto t_post_start = std::chrono::steady_clock::now();

                std::vector<Detection> latestDetections;
                for (const auto& name : outputNames)
                {
                    const auto itPinned = pinnedOutputBuffers.find(name);
                    if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
                        continue;

                    nvinfer1::DataType dtype = outputTypes[name];

                    if (dtype == nvinfer1::DataType::kHALF)
                    {
                        // Convert to float on CPU（SIMD 路径，见 ConvertHalfToFloat 注释）
                        const size_t numElements = outputSizes[name] / sizeof(__half);

                        auto& outputDataFloat = fp16OutputScratch[name];
                        if (outputDataFloat.size() != numElements)
                            outputDataFloat.resize(numElements);

                        ConvertHalfToFloat(itPinned->second, outputDataFloat.data(), numElements);

                        NmsTelemetry nmsTelemetry;
                        latestDetections = postProcess(outputDataFloat.data(), name, &lastNmsTime, &nmsTelemetry);
                        lastPreLimitCount.store(nmsTelemetry.preLimitCount, std::memory_order_relaxed);
                        lastPreNmsCount.store(nmsTelemetry.preNmsCount, std::memory_order_relaxed);
                        lastPostNmsCount.store(nmsTelemetry.postNmsCount, std::memory_order_relaxed);
                    }
                    else if (dtype == nvinfer1::DataType::kFLOAT)
                    {
                        const float* floatPtr = reinterpret_cast<const float*>(itPinned->second);
                        NmsTelemetry nmsTelemetry;
                        latestDetections = postProcess(floatPtr, name, &lastNmsTime, &nmsTelemetry);
                        lastPreLimitCount.store(nmsTelemetry.preLimitCount, std::memory_order_relaxed);
                        lastPreNmsCount.store(nmsTelemetry.preNmsCount, std::memory_order_relaxed);
                        lastPostNmsCount.store(nmsTelemetry.postNmsCount, std::memory_order_relaxed);
                    }
                }

                static int detectCount = 0;
                if (config.verbose && ++detectCount % 30 == 1)
                    std::cout << "[Detector] 推理 #" << detectCount << ": 检测到 " << latestDetections.size() << " 个目标" << std::endl;

                std::vector<cv::Rect> boxes;
                std::vector<int> classes;
                std::vector<float> confidences;
                boxes.reserve(latestDetections.size());
                classes.reserve(latestDetections.size());
                confidences.reserve(latestDetections.size());

                // 【线程安全 + 性能】类别放行表快照。
                // 原实现在本循环内无锁读取 config.class_enabled[] 与
                // config.mouse_hotkeys[slot].localConfig（std::unordered_map）。
                // overlay 线程在持有 configMutex 的情况下通过 setLocalBool 向该 map 插入键，
                // 可能触发 rehash；推理线程同时遍历桶链表即构成数据竞争（UB，可致崩溃）。
                // 同时原实现对每个检测框都执行 "class_enabled_" + to_string(id) 的字符串
                // 拼接与哈希查找，在高帧率下每秒产生上万次堆分配。
                // 现改为每帧一次性在锁内构建定长放行表：锁区间为常数级，热循环零分配。
                std::array<unsigned char, static_cast<std::size_t>(Config::FIXED_TARGET_CLASS_COUNT)> classAllowed{};
                std::string aiModelSnapshot;
                {
                    std::lock_guard<std::mutex> lock(configMutex);
                    const int activeClassSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
                    const Config::MouseHotkey* activeClassProfile =
                        (activeClassSlot >= 0 && activeClassSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS))
                            ? &config.mouse_hotkeys[static_cast<std::size_t>(activeClassSlot)]
                            : nullptr;
                    const TargetClassConfigKeys& classKeys = targetClassConfigKeys();
                    for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
                    {
                        bool allowed = config.isClassEnabled(cls);
                        if (allowed && activeClassProfile != nullptr)
                        {
                            allowed = activeClassProfile->localBool(classKeys.enabled[cls], false);
                        }
                        classAllowed[static_cast<std::size_t>(cls)] = allowed ? 1u : 0u;
                    }
                    // config.ai_model 同样由 overlay 线程改写，直接取 c_str() 存在悬垂风险。
                    aiModelSnapshot = config.ai_model;
                }

                for (const auto& det : latestDetections)
                {
                    // 越界 classId 与原 isClassEnabled() 的边界判定语义一致：直接丢弃。
                    if (det.classId < 0 || det.classId >= Config::FIXED_TARGET_CLASS_COUNT)
                        continue;
                    if (!classAllowed[static_cast<std::size_t>(det.classId)])
                        continue;
                    boxes.push_back(det.box);
                    classes.push_back(det.classId);
                    confidences.push_back(det.confidence);
                }

                detectionBuffer.set(boxes, classes, confidences, frameTimestamp);

                if (hasGpuFrame)
                {
                    cvm::MaybeCollectDataSample(
                        "",
                        aiModelSnapshot.c_str(),
                        frameGpu,
                        boxes,
                        classes,
                        confidences,
                        aiming.load(),
                        config);
                }
                else
                {
                    const cv::Mat& frameForCollection = sourceFrame.empty() ? frame : sourceFrame;
                    cvm::MaybeCollectDataSample(
                        "",
                        aiModelSnapshot.c_str(),
                        frameForCollection,
                        boxes,
                        classes,
                        confidences,
                        aiming.load(),
                        config);
                }

                auto t_post_end = std::chrono::steady_clock::now();

                float preprocessMs = 0.0f;
                float inferenceMs = 0.0f;
                float copyMs = 0.0f;

                cudaEventElapsedTime(&preprocessMs, preprocessStartEvent, inferenceStartEvent);
                cudaEventElapsedTime(&inferenceMs, inferenceStartEvent, inferenceCompleteEvent);
                cudaEventElapsedTime(&copyMs, inferenceCompleteEvent, copyCompleteEvent);

                lastPreprocessTime = std::chrono::duration<double, std::milli>(preprocessMs);
                lastInferenceTime = std::chrono::duration<double, std::milli>(inferenceMs);
                lastCopyTime = std::chrono::duration<double, std::milli>(copyMs);
                lastPostprocessTime = t_post_end - t_post_start;
                lastPreprocessTimeMs.store(lastPreprocessTime.count(), std::memory_order_relaxed);
                lastInferenceTimeMs.store(lastInferenceTime.count(), std::memory_order_relaxed);
                lastCopyTimeMs.store(lastCopyTime.count(), std::memory_order_relaxed);
                lastPostprocessTimeMs.store(lastPostprocessTime.count(), std::memory_order_relaxed);
                lastNmsTimeMs.store(lastNmsTime.count(), std::memory_order_relaxed);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Detector] Error during inference: " << e.what() << std::endl;
            }
        }
    }
}

bool TrtDetector::preProcess(const cv::Mat& frame)
{
    if (frame.empty())
        return false;

    // 用 find() 代替 operator[]：后者在键缺失时会向 inputBindings 静默插入
    // 一个 nullptr 条目，污染绑定表（getBindings 的校验逻辑将被绕过）。
    const auto inputIt = inputBindings.find(inputName);
    if (inputIt == inputBindings.end() || !inputIt->second)
        return false;
    void* inputBuffer = inputIt->second;

    nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(dims.d[1], &c)
        || !tryGetPositiveDimInt(dims.d[2], &h)
        || !tryGetPositiveDimInt(dims.d[3], &w))
    {
        return false;
    }

    if (c != 3)
        return false;

    cv::Mat bgrFrame;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, cpuBgrBuffer, cv::COLOR_BGRA2BGR);
        bgrFrame = cpuBgrBuffer;
        break;
    case 1:
        cv::cvtColor(frame, cpuBgrBuffer, cv::COLOR_GRAY2BGR);
        bgrFrame = cpuBgrBuffer;
        break;
    case 3:
        bgrFrame = frame;
        break;
    default:
        return false;
    }

    cv::Mat resizedBgr;
    if (bgrFrame.cols != w || bgrFrame.rows != h)
    {
        cv::resize(bgrFrame, cpuResizedBuffer, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
        resizedBgr = cpuResizedBuffer;
    }
    else
    {
        resizedBgr = bgrFrame;
    }
    resizedBgr.convertTo(cpuFloatBuffer, CV_32FC3, 1.0f / 255.0f);

    if (!copyCpuTensorToDevice(cpuFloatBuffer, w, h, inputBuffer))
        return false;
    return true;
}

bool TrtDetector::preProcess(const cv::cuda::GpuMat& frame)
{
    if (frame.empty())
        return false;

    if (frame.type() != CV_8UC4)
    {
        std::cerr << "[Detector] GPU preprocess requires CV_8UC4 BGRA input; got type "
            << frame.type() << std::endl;
        return false;
    }

    const auto inputIt = inputBindings.find(inputName);
    if (inputIt == inputBindings.end() || !inputIt->second)
    {
        std::cerr << "[Detector] GPU preprocess has no TensorRT input binding." << std::endl;
        return false;
    }

    const nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
    int channels = 0;
    int height = 0;
    int width = 0;
    if (!tryGetPositiveDimInt(dims.d[1], &channels) ||
        !tryGetPositiveDimInt(dims.d[2], &height) ||
        !tryGetPositiveDimInt(dims.d[3], &width) || channels != 3)
    {
        std::cerr << "[Detector] GPU preprocess requires a valid 3-channel NCHW input binding." << std::endl;
        return false;
    }

    if (!launch_bgra_resize_to_rgb_nchw(
            frame.ptr<unsigned char>(),
            frame.step,
            frame.cols,
            frame.rows,
            static_cast<float*>(inputIt->second),
            width,
            height,
            stream))
    {
        std::cerr << "[Detector] GPU preprocessing launch failed; frame was not submitted." << std::endl;
        return false;
    }
    return true;
}

bool TrtDetector::copyCpuTensorToDevice(const cv::Mat& bgrFloatFrame, int width, int height, void* inputBuffer)
{
    if (bgrFloatFrame.empty() || bgrFloatFrame.channels() != 3 || !inputBuffer)
        return false;

    const size_t channelSize = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t tensorSize = channelSize * 3;
    if (inputHostBuffer.size() != tensorSize)
        inputHostBuffer.resize(tensorSize);

    float* dst = inputHostBuffer.data();
    cv::Mat bgrToRgbPlanes[3] = {
        cv::Mat(height, width, CV_32F, dst + channelSize * 2),
        cv::Mat(height, width, CV_32F, dst + channelSize),
        cv::Mat(height, width, CV_32F, dst)
    };
    cv::split(bgrFloatFrame, bgrToRgbPlanes);

    cudaError_t err = cudaMemcpyAsync(
        inputBuffer,
        inputHostBuffer.data(),
        tensorSize * sizeof(float),
        cudaMemcpyHostToDevice,
        stream);
    if (err != cudaSuccess)
    {
        std::cerr << "[Detector] preprocess input copy failed: " << cudaGetErrorString(err) << std::endl;
        cudaGetLastError();
        return false;
    }
    return true;
}

std::vector<Detection> TrtDetector::postProcess(
    const float* output,
    const std::string& outputName,
    std::chrono::duration<double, std::milli>* nmsTime,
    NmsTelemetry* telemetry)
{
    if (!output)
        return {};

    const auto shapeIt = outputShapes.find(outputName);
    if (shapeIt == outputShapes.end())
        return {};

    std::vector<Detection> detections;

    // 推理线程读取 config 字段与 UI 线程（持 configMutex 写入）并发，
    // 必须加锁做快照，消除数据竞争（UB）。持锁区间仅覆盖 config 读取。
    float confThreshold = config.confidence_threshold;
    float nmsThreshold = config.nms_threshold;
    int maxDetections = std::max(1, config.max_detections);
    {
        std::lock_guard<std::mutex> lock(configMutex);
        confThreshold = config.confidence_threshold;
        nmsThreshold = config.nms_threshold;
        maxDetections = std::max(1, config.max_detections);
        const int activeSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
        if (activeSlot >= 0 && activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS))
        {
            const auto& profile = config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)];
            confThreshold = profile.localFloat("confidence_threshold", confThreshold);
            nmsThreshold = profile.localFloat("nms_threshold", nmsThreshold);
            maxDetections = std::max(1, profile.localInt("max_detections", maxDetections));
        }
    }

    detections = postProcessYolo(
        output,
        shapeIt->second,
        numClasses,
        confThreshold,
        nmsThreshold,
        maxDetections,
        nmsTime,
        telemetry
    );

    {
        // filterDetectionsByCircleFov 读取 config.circle_fov_* 与 detection_resolution，
        // 同样需要 configMutex 保护。
        std::lock_guard<std::mutex> lock(configMutex);
        filterDetectionsByCircleFov(detections);
    }
    return detections;
}
#endif
