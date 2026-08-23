#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <optional>
#include <deque>
#include <random>
#include <array>
#include <cwchar>
#include <memory>

#include <opencv2/core/utils/logger.hpp>

#include "capture.h"
#include "mouse.h"
#include "mybot.h"
#include "keyboard_listener.h"
#include "overlay.h"
#include "overlay/onnx_inspector.h"

#include "other_tools.h"
#include "virtual_camera.h"
#include "mem/cpu_affinity_manager.h"
#include "runtime/thread_loops.h"
#include "benchmarks/provider_benchmark.h"

#include "mem/gpu_resource_manager.h"
#include "tensorrt/nvinf.h"

std::condition_variable frameCV;
std::atomic<bool> shouldExit(false);
std::atomic<bool> aiming(false);
std::atomic<bool> detectionPaused(false);
std::mutex configMutex;
std::mutex inputDevicesMutex;

TrtDetector* trt_detector = nullptr;

MouseThread* globalMouseThread = nullptr;
Config config;


KmboxNetConnection* kmboxNetSerial = nullptr;
KmboxAConnection* kmboxASerial = nullptr;
MakcuConnection* makcuSerial = nullptr;
std::unique_ptr<IMouseInput> activeMouseInputOwner;

std::atomic<bool> detection_resolution_changed(false);
std::atomic<bool> capture_method_changed(false);
std::atomic<bool> capture_cursor_changed(false);
std::atomic<bool> capture_borders_changed(false);
std::atomic<bool> capture_fps_changed(false);
std::atomic<bool> capture_window_changed(false);
std::atomic<bool> virtual_camera_apply_requested(false);
std::atomic<bool> detector_model_changed(false);
std::atomic<bool> show_window_changed(false);
std::atomic<bool> input_method_changed(false);

std::atomic<bool> zooming(false);
std::atomic<bool> shooting(false);
std::atomic<int> active_mouse_hotkey_slot(-1);




static int FatalExit(const std::string& message)
{
    std::cerr << message << std::endl;
    std::cout << "按回车键退出...";
    std::cin.get();
    return -1;
}

static void SetWorkingDirectoryToExecutable()
{
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
    {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        std::error_code ec;
        std::filesystem::current_path(exeDir, ec);
        if (ec && config.verbose)
        {
            std::cout << "[配置] 设置工作目录失败: " << exeDir.u8string()
                      << " (" << ec.message() << ")" << std::endl;
        }
    }
}

static std::filesystem::path ModelsDirectory()
{
    std::error_code ec;
    const auto dir = std::filesystem::absolute(std::filesystem::path("models"), ec).lexically_normal();
    return ec ? std::filesystem::path("models") : dir;
}

static std::filesystem::path ModelPath(const std::string& modelName)
{
    return (ModelsDirectory() / std::filesystem::path(modelName).filename()).lexically_normal();
}

static void ApplyModelInputResolution(int modelWidth, int modelHeight)
{
    if (modelWidth <= 0 || modelHeight <= 0)
        return;

    {
        std::lock_guard<std::mutex> lock(configMutex);
        config.model_input_width = modelWidth;
        config.model_input_height = modelHeight;

        int forceWidth = config.force_model_input_width;
        int forceHeight = config.force_model_input_height;
        const bool forceValid = config.force_model_input_size
            && Config::normalizeModelInputSize(forceWidth, forceHeight);
        config.detection_resolution = forceValid ? forceWidth : modelWidth;
    }

    detection_resolution_changed.store(true);
}

static bool SelectCompatibleAiModel()
{
    std::vector<std::string> availableModels = getAvailableModels();
    if (!config.ai_model.empty())
    {
        const std::filesystem::path modelPath = ModelPath(config.ai_model);
        if (!std::filesystem::exists(modelPath))
        {
            std::cerr << "[模型] 指定的模型不存在:" << std::endl;
            std::cerr << modelPath.u8string() << std::endl;
        }
#ifdef USE_CUDA
        else if (modelPath.extension() == ".engine" || modelPath.extension() == ".onnx")
        {
            if (modelPath.extension() == ".onnx")
            {
                const std::filesystem::path enginePath = modelPath.parent_path() /
                    (modelPath.stem().string() + ".engine");
                if (!std::filesystem::exists(enginePath))
                {
                    std::cout << "\n========== 模型 ==========" << std::endl;
                    std::cout << "检测到 ONNX 模型:" << std::endl;
                    std::cout << modelPath.u8string() << std::endl;
                    std::cout << "将自动编译为 TensorRT 引擎:" << std::endl;
                    std::cout << enginePath.u8string() << std::endl;
                    std::cout << "[模型] 后续运行将直接加载 .engine 文件" << std::endl;
                }
            }
            return true;
        }
#endif
        else if (std::find(availableModels.begin(), availableModels.end(), config.ai_model) != availableModels.end())
        {
            return true;
        }
        else
        {
            std::cerr << "[主程序] 指定模型与此后端不兼容 "
                      << config.backend << ": " << config.ai_model << std::endl;
        }
    }

    if (availableModels.empty())
    {
        std::cerr << "[主程序] 在 'models' 目录中未找到兼容后端 "
                  << config.backend << "." << std::endl;
        return false;
    }

    config.ai_model = availableModels[0];
    config.saveConfig("config.ini");
    std::cout << "\n========== 模型 ==========" << std::endl;
    std::cout << "已选择首个兼容 " << config.backend << " 模型:" << std::endl;
    std::cout << ModelPath(config.ai_model).u8string() << std::endl;
    return true;
}

static void HandleThreadCrash(const char* name, const std::exception* ex)
{
    std::cerr << "[线程] " << name << " 崩溃: "
              << (ex ? ex->what() : "未知异常") << std::endl;
    shouldExit = true;
#ifdef USE_CUDA
    if (trt_detector)
        trt_detector->requestStop();
#endif
    frameCV.notify_all();
    detectionBuffer.cv.notify_all();
}

template <typename Func>
static std::thread StartThreadGuarded(const char* name, Func func)
{
    return std::thread([name, func]() mutable {
        try
        {
            func();
        }
        catch (const std::exception& e)
        {
            HandleThreadCrash(name, &e);
        }
        catch (...)
        {
            HandleThreadCrash(name, nullptr);
        }
        });
}

// 【退出安全兜底】std::thread 在仍处于 joinable 状态时被析构，标准要求直接调用
// std::terminate()。main 中 5 个后台线程全部创建于同一个 try 块内：从第一个线程
// 创建成功、到最后一个 join() 返回之间，任何一处抛出异常都会在栈展开时析构掉仍
// joinable 的线程对象，使进程当场 terminate —— 没有任何清理：硬件盒子的鼠标按键
// 可能卡在按下状态、串口/UDP 套接字不关闭、CUDA 上下文不释放。
// 可能的抛出点包括：std::thread 构造在系统资源不足时抛 std::system_error、
// welcome_message()、以及 join() 自身抛 std::system_error。
// 本 RAII 守卫在异常路径上先置 shouldExit 并唤醒全部条件变量，再按与正常路径
// 完全相同的顺序 join，把"立即 terminate"降级为"有序退出"。
// 正常路径下所有线程都已被显式 join，此处 joinable() 均为 false，开销为零。
struct ShutdownJoinGuard
{
    std::array<std::thread*, 5> threads{};

    ~ShutdownJoinGuard()
    {
        bool anyPending = false;
        for (std::thread* t : threads)
        {
            if (t && t->joinable())
            {
                anyPending = true;
                break;
            }
        }
        if (!anyPending)
            return;

        std::cerr << "[主程序] 检测到异常退出路径，正在有序停止后台线程..." << std::endl;
        shouldExit = true;
#ifdef USE_CUDA
        if (trt_detector)
            trt_detector->requestStop();
#endif
        frameCV.notify_all();
        detectionBuffer.cv.notify_all();

        for (std::thread* t : threads)
        {
            if (!t || !t->joinable())
                continue;
            try
            {
                t->join();
            }
            catch (const std::exception& e)
            {
                // join 失败时绝不能让 joinable 的 thread 对象继续析构（那必然
                // terminate）。detach 并非理想收场，但相比进程立即崩溃仍是更优解。
                std::cerr << "[主程序] 线程 join 失败，改为 detach: " << e.what() << std::endl;
                t->detach();
            }
        }
    }
};

void createInputDevices()
{
    if (globalMouseThread)
    {
        std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
        globalMouseThread->setMouseInput(nullptr);
    }

    std::unique_ptr<IMouseInput> oldMouseInputOwner;
    {
        std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
        oldMouseInputOwner = std::move(activeMouseInputOwner);
        kmboxNetSerial = nullptr;
        kmboxASerial = nullptr;
        makcuSerial = nullptr;
    }
    oldMouseInputOwner.reset();

    Config cfgSnapshot;
    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        cfgSnapshot = config;
    }

    auto newMouseInputOwner = CreateMouseInputDevice(cfgSnapshot);
    IMouseInput* newMouseInput = newMouseInputOwner.get();

    KmboxNetConnection* newKmboxNetSerial = newMouseInput ? newMouseInput->kmboxNet() : nullptr;
    KmboxAConnection* newKmboxASerial = newMouseInput ? newMouseInput->kmboxA() : nullptr;
    MakcuConnection* newMakcuSerial = newMouseInput ? newMouseInput->makcu() : nullptr;

    std::string message = std::string("[鼠标] 使用 ") + (newMouseInput ? newMouseInput->name() : "unknown") + " 输入。";
    if (!newMouseInput || !newMouseInput->isOpen())
        message += " 设备未连接；输入已禁用，等待所选方式可用。";

    {
        std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
        activeMouseInputOwner = std::move(newMouseInputOwner);
        kmboxNetSerial = newKmboxNetSerial;
        kmboxASerial = newKmboxASerial;
        makcuSerial = newMakcuSerial;
    }

    std::cout << message << std::endl;
}

void assignInputDevices()
{
    if (globalMouseThread)
    {
        std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
        globalMouseThread->setMouseInput(activeMouseInputOwner.get());
    }
}


int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetRandomConsoleTitle();
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_FATAL);
    SetWorkingDirectoryToExecutable();

    std::cout << "========== 启动 ==========" << std::endl;
    std::cout << "正在初始化..." << std::endl;
    std::cout << "如果程序长时间无响应，请按 Ctrl+C 终止。" << std::endl;
    std::cout << std::endl;

    if (benchmarks::IsProviderBenchmarkRequested(argc, argv))
    {
        return benchmarks::RunProviderBenchmarkCli(argc, argv);
    }

    if (!config.loadConfig())
    {
        std::cerr << "[配置] 加载配置文件出错！" << std::endl;
        std::cin.get();
        return -1;
    }

    CPUAffinityManager cpuManager;

    // CPU 核心 / 系统内存预留属于可选优化项，失败不应阻断启动：
    // 例如配置的核心数大于实际核心数、或系统可用内存不足时，程序仍可正常工作。
    if (config.cpuCoreReserveCount > 0)
    {
        if (!cpuManager.reserveCPUCores(config.cpuCoreReserveCount))
            std::cerr << "[主程序] 预留CPU核心失败，已跳过该优化项，继续启动。" << std::endl;
    }

    if (config.systemMemoryReserveMB > 0)
    {
        if (!cpuManager.reserveSystemMemory(static_cast<std::size_t>(config.systemMemoryReserveMB)))
            std::cerr << "[主程序] 预留系统内存失败，已跳过该优化项，继续启动。" << std::endl;
    }

    try
    {
        int cuda_runtime_version = 0;
        cudaError_t runtime_status = cudaRuntimeGetVersion(&cuda_runtime_version);

        if (runtime_status != cudaSuccess)
        {
            std::cerr << "[硬件检测] CUDA运行库检查失败: " << cudaGetErrorString(runtime_status) << std::endl;
            std::cerr << "[硬件检测] 您的系统可能缺少NVIDIA显卡驱动或CUDA运行库。" << std::endl;
            std::cerr << "[硬件检测] 如需使用TensorRT加速，请安装NVIDIA显卡驱动和CUDA Toolkit。" << std::endl;
            std::cerr << "[硬件检测] 本程序仅支持 NVIDIA CUDA/TensorRT。" << std::endl;
            std::cin.get();
            return -1;
        }

        int cuda_devices = 0;
        cudaError_t device_count_status = cudaGetDeviceCount(&cuda_devices);
        if (device_count_status != cudaSuccess || cuda_devices == 0)
        {
            std::cerr << "[硬件检测] 未检测到支持CUDA的NVIDIA显卡: "
                      << cudaGetErrorString(device_count_status) << std::endl;
            std::cerr << "[硬件检测] 如需使用TensorRT加速，请安装NVIDIA显卡驱动。" << std::endl;
            std::cerr << "[硬件检测] 本程序仅支持 NVIDIA CUDA/TensorRT。" << std::endl;
            std::cin.get();
            return -1;
        }

        if (config.cuda_device_index < 0 || config.cuda_device_index >= cuda_devices)
            return FatalExit("[硬件检测] cuda_device_index 超出可用设备范围。");

        std::cout << "\n========== CUDA 硬件 ==========" << std::endl;
        for (int deviceIndex = 0; deviceIndex < cuda_devices; ++deviceIndex)
        {
            cudaDeviceProp deviceProp{};
            if (cudaGetDeviceProperties(&deviceProp, deviceIndex) != cudaSuccess)
                continue;
            char pciBusId[32]{};
            cudaDeviceGetPCIBusId(pciBusId, static_cast<int>(sizeof(pciBusId)), deviceIndex);
            std::cout << "  [CUDA设备 " << deviceIndex << "] " << deviceProp.name
                      << ", CC " << deviceProp.major << "." << deviceProp.minor
                      << ", 显存 " << (deviceProp.totalGlobalMem / (1024ULL * 1024ULL))
                      << " MB, PCI " << pciBusId << std::endl;
        }

        cudaError_t set_device_status = cudaSetDevice(config.cuda_device_index);
        if (set_device_status != cudaSuccess)
            return FatalExit("[硬件检测] 无法选择 CUDA 设备: " + std::string(cudaGetErrorString(set_device_status)));

        cudaDeviceProp prop{};
        cudaError_t property_status = cudaGetDeviceProperties(&prop, config.cuda_device_index);
        if (property_status != cudaSuccess)
            return FatalExit("[硬件检测] 无法读取 NVIDIA GPU 属性: " + std::string(cudaGetErrorString(property_status)));

        if (prop.major < 7 || (prop.major == 7 && prop.minor < 5))
        {
            std::cerr << "[硬件检测] 当前版本要求 Compute Capability 7.5 或更高。" << std::endl;
            std::cerr << "[硬件检测] 当前 GPU 为 " << prop.major << "." << prop.minor << "，不受支持。" << std::endl;
            return FatalExit("[硬件检测] 请使用 NVIDIA GTX 16、RTX 20 或更新型号。");
        }
        std::cout << "  [硬件检测] 检测到 NVIDIA 显卡" << std::endl;
        std::cout << "  [硬件检测] 型号: " << prop.name << std::endl;
        std::cout << "  [硬件检测] 计算能力: " << prop.major << "." << prop.minor << std::endl;
        std::cout << "  [硬件检测] 显存: " << (prop.totalGlobalMem / (1024ULL * 1024ULL)) << " MB" << std::endl;
        std::cout << "  [硬件检测] CUDA版本: " << (cuda_runtime_version / 1000) << "." 
                  << ((cuda_runtime_version % 1000) / 10) << std::endl;

        int cuda_driver_version = 0;
        cudaError_t driver_status = cudaDriverGetVersion(&cuda_driver_version);
        if (driver_status != cudaSuccess)
            return FatalExit("[硬件检测] 无法读取 NVIDIA 驱动版本: " + std::string(cudaGetErrorString(driver_status)));

        std::cout << "  [硬件检测] CUDA驱动版本: " << (cuda_driver_version / 1000) << "."
                  << ((cuda_driver_version % 1000) / 10) << std::endl;
        if (cuda_runtime_version < 12000)
        {
            int runtime_major = cuda_runtime_version / 1000;
            int runtime_minor = (cuda_runtime_version % 1000) / 10;
            std::cout << "[硬件检测] CUDA Runtime " << runtime_major << "." << runtime_minor
                      << " 低于推荐版本，但未因版本号强制阻止启动。" << std::endl;
        }

        // 自动切换到 TRT 后端
        if (config.backend != "TRT")
        {
            config.backend = "TRT";
            config.saveConfig("config.ini");
            std::cout << "[配置] 检测到 NVIDIA 显卡，已切换到 TensorRT 后端。" << std::endl;
        }
        else
        {
            std::cout << "[配置] 使用 TensorRT 后端。" << std::endl;
        }

        // 显存预留只用于抢占启动窗口期的显存，稍后会在创建 TensorRT 检测器前归还。
        // 失败同样不应阻断启动：游戏运行时空闲显存不足是常态，此时直接放弃预留即可。
        GPUResourceManager gpuManager;
        if (config.gpuMemoryReserveMB > 0)
        {
            if (!gpuManager.reserveGPUMemory(static_cast<std::size_t>(config.gpuMemoryReserveMB)))
                std::cerr << "[主程序] 预留GPU内存失败，已跳过该优化项，继续启动。" << std::endl;
        }
        
        if (config.enableGpuExclusiveMode)
        {
            if (!gpuManager.setGPUExclusiveMode())
                std::cerr << "[主程序] 设置GPU独占模式失败，已跳过该优化项，继续启动。" << std::endl;
        }
        if (!CreateDirectory(L"models", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cerr << "[主程序] models 文件夹创建失败。" << std::endl;
            std::cin.get();
            return -1;
        }

        if (config.capture_method == "virtual_camera")
        {
            auto cams = VirtualCameraCapture::GetAvailableVirtualCameras(true);
            if (!cams.empty())
            {
                if (config.virtual_camera_name != "None" &&
                    std::find(cams.begin(), cams.end(), config.virtual_camera_name) == cams.end())
                {
                    config.virtual_camera_name = "None";
                    config.saveConfig("config.ini");
                    std::cout << "[主程序] 虚拟摄像头名称已重置为None（自动选择）。" << std::endl;
                }
                std::cout << "[主程序] 已加载虚拟摄像头: " << cams.size() << std::endl;
            }
            else
            {
                std::cerr << "[主程序] 未找到虚拟摄像头" << std::endl;
            }
        }

        if (!SelectCompatibleAiModel())
        {
            std::cin.get();
            return -1;
        }

        const std::filesystem::path selectedModelPath = ModelPath(config.ai_model);
        StartupOnnxReport startupReport = inspectLoadedEngineOnnx(
            selectedModelPath.string(), config.detection_resolution);
        publishStartupOnnxReport(startupReport);
        ApplyModelInputResolution(startupReport.width, startupReport.height);
        if (startupReport.success)
            std::cout << startupReport.text << std::endl;

        createInputDevices();

        MouseThread mouseThread(
            config.detection_resolution,
            config.predictionInterval,
            config.auto_shoot,
            config.bScope_multiplier,
            activeMouseInputOwner.get()
        );

        globalMouseThread = &mouseThread;
        assignInputDevices();

        // 关键：在 TensorRT 引擎反序列化/构建之前归还预留显存。
        // 否则这块显存整个运行期都被占住，TensorRT 只能在更小的显存里选择算子策略，
        // 直接导致引擎构建 OOM 风险上升、推理延迟变高、FPS 下降。
        if (gpuManager.hasReservation())
        {
            const std::size_t releasedMB = gpuManager.reservedBytes() / (1024ULL * 1024ULL);
            gpuManager.releaseReservation();
            std::cout << "[主程序] 已归还预留显存 " << releasedMB
                      << " MB 供 TensorRT 使用。" << std::endl;
        }

        try
        {
            trt_detector = new TrtDetector();
            if (!trt_detector->initialize(ModelPath(config.ai_model).string()))
            {
                // 与下方两个 catch 分支保持一致：失败时释放检测器，避免引擎/上下文/CUDA 缓冲泄漏。
                delete trt_detector;
                trt_detector = nullptr;
                return FatalExit("[主程序] TensorRT引擎与当前设备不兼容，初始化失败。");
            }
            std::cout << "[主程序] TensorRT 检测器已创建。" << std::endl;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[主程序] TensorRT检测器初始化失败: " << e.what() << std::endl;
            delete trt_detector;
            trt_detector = nullptr;
            return FatalExit("[主程序] TensorRT模型不可用，程序无法继续。");
        }
        catch (...)
        {
            std::cerr << "[主程序] TensorRT检测器初始化失败: 未知异常" << std::endl;
            delete trt_detector;
            trt_detector = nullptr;
            return FatalExit("[主程序] TensorRT模型不可用，程序无法继续。");
        }

        detection_resolution_changed.store(true);

        // 线程对象先全部默认构造（此时均为非 joinable，析构安全），随后再声明
        // ShutdownJoinGuard —— 局部对象按声明逆序析构，守卫声明在后即保证它先于
        // 这些 thread 对象析构，从而能在任何异常路径上抢先把它们 join 掉。
        // 若像原实现那样"边声明边启动"，一旦第 N 个线程的构造抛出，前 N-1 个
        // 已 joinable 的线程对象会直接析构 → std::terminate。
        std::thread keyThread;
        std::thread capThread;
        std::thread trt_detThread;
        std::thread mouseMovThread;
        std::thread overlayThread;

        ShutdownJoinGuard joinGuard{ { &keyThread, &capThread, &trt_detThread,
                                       &mouseMovThread, &overlayThread } };

        keyThread = StartThreadGuarded("KeyboardListener", [] {
            keyboardListener();
            });
        capThread = StartThreadGuarded("CaptureThread", [] {
            captureThread(config.detection_resolution, config.detection_resolution);
            });

        if (trt_detector)
        {
            trt_detThread = StartThreadGuarded("TrtDetector", [] {
                if (trt_detector) trt_detector->inferenceThread();
                });
        }
        mouseMovThread = StartThreadGuarded("MouseThread", [&mouseThread] {
            mouseThreadFunction(mouseThread);
            });
        overlayThread = StartThreadGuarded("OverlayThread", [] {
            OverlayThread();
            });

        welcome_message();

        keyThread.join();
        capThread.join();
        if (trt_detector)
        {
            trt_detector->requestStop();
        }
        if (trt_detThread.joinable())
            trt_detThread.join();

        mouseMovThread.join();
        overlayThread.join();

        // ── 输入设备拆除：顺序至关重要 ──
        //
        // (1) 先释放可能仍处于按下状态的鼠标按键。
        //     mouse_thread_loop 的主循环是 `while (!shouldExit)`。当用户恰好在
        //     auto_shoot 正在开火（mouse_pressed == true）时退出，循环从判定处直接
        //     跳出，**不会**再走到 releaseMouse() 分支；而 ~MouseThread() 只负责停
        //     moveWorker，各 IMouseInput 实现的析构也都不发送 leftUp。结果是进程退
        //     出后硬件盒子（makcu / kmboxNet / kmboxA）仍保持左键按下，游戏内表现
        //     为"程序关了还在一直开火"，用户必须拔插设备才能恢复。
        //     此处趁设备仍然有效补一次释放（releaseMouse 内部自带 inputDevicesMutex，
        //     故放在下方临界区之外，避免 std::mutex 不可重入导致的自死锁）。
        //
        // (2) 再把 MouseThread 持有的裸指针置空，最后才销毁设备对象。
        //     MouseThread 内部另有一个独立的 moveWorker 线程，它只在 ~MouseThread()
        //     里才被 stop + join，而那要等到本 try 作用域结束时才发生 —— 也就是说，
        //     执行到这里时 moveWorker 仍在运行，它会消费 moveQueue 并调用
        //     sendMovementToDriver() → mouseInput->move()。若像原实现那样直接
        //     reset(activeMouseInputOwner)，MouseThread::mouseInput 这个裸指针立刻
        //     悬垂，随后 moveWorker 一取到 inputDevicesMutex 就会对已析构对象发起
        //     虚调用 —— 退出期 use-after-free。且紧随其后的 delete trt_detector 要做
        //     CUDA/TensorRT 拆除（数十至数百毫秒），这个窗口足够长，队列里的移动完
        //     全来得及通过 generation/freshness 校验而被真正下发。
        //     运行期的 createInputDevices() 已经是"先置空、再替换"的正确写法，
        //     此处与之对齐，消除同一资源在两条路径上的拆除语义不一致。
        //
        // (3) globalMouseThread 指向 main 栈上的 mouseThread，作用域结束后即悬垂；
        //     退出前显式置空，避免后续任何路径（线程崩溃处理器、静态析构）误用。
        if (globalMouseThread)
        {
            globalMouseThread->releaseMouse();
        }
        {
            std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
            if (globalMouseThread)
                globalMouseThread->setMouseInput(nullptr);
            activeMouseInputOwner.reset();
            kmboxNetSerial = nullptr;
            kmboxASerial = nullptr;
            makcuSerial = nullptr;
        }
        globalMouseThread = nullptr;

        if (trt_detector)
        {
            delete trt_detector;
            trt_detector = nullptr;
        }

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[主程序] 主流程发生错误: " << e.what() << std::endl;
        std::cout << "按回车键退出...";
        std::cin.get();
        return -1;
    }
}
