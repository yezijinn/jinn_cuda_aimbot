#ifdef USE_CUDA
#ifndef TRT_DETECTOR_H
#define TRT_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda.hpp>
#include <NvInfer.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cuda_fp16.h>
#include <memory>
#include <thread>
#include <chrono>
#include <functional>
#include <cuda_runtime_api.h>

#include "postProcess.h"

class TrtDetector
{
public:
    TrtDetector();
    ~TrtDetector();
    bool initialize(const std::string& modelFile);
    void processFrame(
        const cv::Mat& detection_frame,
        const cv::Mat& source_frame = cv::Mat(),
        std::chrono::steady_clock::time_point frameTimestamp = {});
    void processFrameGpu(
        const cv::cuda::GpuMat& frame,
        std::chrono::steady_clock::time_point frameTimestamp = {});
    void inferenceThread();
    void requestStop();
    bool isInitialized() const;
    std::vector<Detection> detect(const cv::Mat& frame);
    int getNumClasses() const { return numClasses; }

    float img_scale;
    int modelInputWidth = -1;  // 引擎实际输入宽，由 initialize() 写入
    int modelInputHeight = -1; // 引擎实际输入高，由 initialize() 写入

    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    std::unordered_map<std::string, size_t> outputSizes;

    std::chrono::duration<double, std::milli> lastPreprocessTime;
    std::chrono::duration<double, std::milli> lastInferenceTime;
    std::chrono::duration<double, std::milli> lastCopyTime;
    std::chrono::duration<double, std::milli> lastPostprocessTime;
    std::chrono::duration<double, std::milli> lastNmsTime;
    std::atomic<double> lastPreprocessTimeMs{ 0.0 };
    std::atomic<double> lastInferenceTimeMs{ 0.0 };
    std::atomic<double> lastCopyTimeMs{ 0.0 };
    std::atomic<double> lastPostprocessTimeMs{ 0.0 };
    std::atomic<double> lastNmsTimeMs{ 0.0 };
    std::atomic<uint64_t> submittedFrameSequence{ 0 };
    std::atomic<uint64_t> overwrittenFrameCount{ 0 };
    std::atomic<uint32_t> lastPreLimitCount{ 0 };
    std::atomic<uint32_t> lastPreNmsCount{ 0 };
    std::atomic<uint32_t> lastPostNmsCount{ 0 };

private:
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;

    cudaStream_t stream;

    bool useCudaGraph;
    bool cudaGraphCaptured;
    cudaGraph_t cudaGraph;
    cudaGraphExec_t cudaGraphExec;
    void captureCudaGraph();
    // 【修复·图启动失败被吞】原返回 void，cudaGraphLaunch 失败仅打印日志。
    // 图内录制了输出的 D2H 拷贝，一旦启动失败这些拷贝不执行，pinned 缓冲仍是
    // 上一帧结果；若随后事件同步恰好成功（空流），downloadOk 仍为真 → 解码陈旧
    // 数据产出幽灵检测框，且推理线程不会累计失败、自恢复永不触发。改为返回是否
    // 成功，调用方据此置 downloadOk=false，与非图路径 enqueueV3 判定语义对齐。
    bool launchCudaGraph();
    void destroyCudaGraph();

    std::unordered_map<std::string, void*> pinnedOutputBuffers;
    void allocatePinnedOutputs();
    void freePinnedOutputs();

    std::mutex inferenceMutex;
    std::condition_variable inferenceCV;
    std::atomic<bool> shouldExit;

    // ── 推理链路连续失败自恢复状态 ──
    // 语义详见 trt_detector.cpp::noteInferenceOutcome()。
    // 除 consecutiveInferenceFailures（供 UI/诊断只读观测，故用 atomic）外，
    // 其余成员仅由推理线程自身读写，无需同步。
    std::atomic<uint32_t> consecutiveInferenceFailures{ 0 };
    uint32_t inferenceRecoveryAttempts = 0;
    std::chrono::steady_clock::time_point lastInferenceRecoveryTime{};
    bool inferenceRecoveryExhausted = false;
    void noteInferenceOutcome(bool ok);

    cv::Mat currentFrame;
    cv::Mat currentSourceFrame;
    cv::cuda::GpuMat currentFrameGpu;
    std::chrono::steady_clock::time_point currentFrameTimestamp{};
    bool frameReady;

    enum class PendingFrameType
    {
        None = 0,
        Cpu = 1,
        Gpu = 2
    };
    PendingFrameType pendingFrameType = PendingFrameType::None;

    void loadEngine(const std::string& engineFile);

    // 【修复·脏输入推理】原返回 void，CPU 预处理失败（输入 binding 为空、
    // 通道数不受支持、输入维度非法）时调用方无从感知，仍会 enqueueV3，
    // 从而用设备端上一帧的残留数据推理并产出错误检测框。改为返回是否成功。
    bool preProcess(const cv::Mat& frame);
    bool preProcess(const cv::cuda::GpuMat& frame);
    bool copyCpuTensorToDevice(const cv::Mat& bgrFloatFrame, int width, int height, void* inputBuffer);

    cv::Mat cpuBgrBuffer;
    cv::Mat cpuResizedBuffer;
    cv::Mat cpuFloatBuffer;
    std::vector<float> inputHostBuffer;

    std::vector<Detection> postProcess(
        const float* output,
        const std::string& outputName,
        std::chrono::duration<double, std::milli>* nmsTime,
        NmsTelemetry* telemetry
    );

    void getInputNames();
    void getOutputNames();
    // 【修复·静默失效】原返回 void，cudaMalloc 失败时仅打印日志并清空绑定表，
    // initialize() 无从感知仍返回 true，后续 setTensorAddress 会绑定空指针，
    // 推理阶段直接对空设备指针读写。改为返回分配是否全部成功。
    bool getBindings();

    std::unordered_map<std::string, size_t> inputSizes;
    std::unordered_map<std::string, void*> inputBindings;
    std::unordered_map<std::string, void*> outputBindings;
    std::unordered_map<std::string, std::vector<int64_t>> outputShapes;
    int numClasses;

    size_t getSizeByDim(const nvinfer1::Dims& dims);
    size_t getElementSize(nvinfer1::DataType dtype);

    std::string inputName;
    // 【修复·未初始化指针 UB】该成员既未在构造函数初始化列表中赋值，
    // 也无任何赋值点，但析构函数会执行 `if (inputBufferDevice) cudaFree(...)`。
    // `new TrtDetector()` 不会清零成员，读取其不确定值属 UB，且当堆残留数据
    // 恰为非零时会对非法指针调用 cudaFree，可污染 CUDA 上下文。
    // 此处补类内初始化器（构造函数初始化列表同步补充，双重保险）。
    void* inputBufferDevice = nullptr;

    std::unordered_map<std::string, nvinfer1::DataType> outputTypes;
    std::unordered_map<std::string, std::vector<float>> fp16OutputScratch;

    // CUDA Events
    cudaEvent_t preprocessStartEvent = nullptr;
    cudaEvent_t inferenceStartEvent = nullptr;
    cudaEvent_t inferenceCompleteEvent = nullptr;
    cudaEvent_t copyCompleteEvent = nullptr;
    bool asyncInferenceInProgress = false;
};

#endif // TRT_DETECTOR_H
#endif
