#ifdef USE_CUDA
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>
#include <limits>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>
#include <onnxruntime_cxx_api.h>

#include "nvinf.h"
#include "mybot.h"
#include "trt_monitor.h"
#include "other_tools.h"

Logger gLogger;

namespace
{
// 【修复·生命周期 UB】TensorRT 明确要求 IRuntime 的生命周期必须覆盖由它
// 反序列化出的所有 ICudaEngine。原实现在 buildEngineFromOnnx 内 `delete runtime;`
// 之后仍把 engine 返回给调用方，调用方随即对该 engine 调用 serialize() 并 delete，
// 此时其父 runtime 已析构 —— 属未定义行为（TensorRT 10 上可直接崩溃或静默产出
// 损坏的序列化数据）。
// 修复方式：把构建期 runtime 交由进程级持有器保管，直到下一次构建时才回收
//（届时上一个 engine 早已被调用方释放）。额外常驻内存仅一个 IRuntime 对象。
std::mutex gBuildRuntimeMutex;
std::unique_ptr<nvinfer1::IRuntime> gBuildRuntimeHolder;

void RetainBuildRuntime(nvinfer1::IRuntime* runtime)
{
    std::lock_guard<std::mutex> lock(gBuildRuntimeMutex);
    gBuildRuntimeHolder.reset(runtime);
}

// 构建引擎时从 ONNX 文件提取模型自定义信息，序列化为 "key=value\n" 文本
//（UTF-8，value 中的换行被替换为空格，避免破坏逐行格式）。返回空串表示提取失败
//（模型损坏 / 无 metadata / onnxruntime 异常），此时引擎不追加嵌入块，行为同旧版。
// 这些字段后续会被 onnx_inspector 解析用于"只有 .engine 时推断模型信息"。
std::string BuildEmbeddedMetadata(const std::string& onnxFile)
{
    std::string out;
    try
    {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "engine_meta");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        options.SetLogSeverityLevel(ORT_LOGGING_LEVEL_ERROR);
        // Windows 上 Ort::Session 的路径参数为宽字符（ORTCHAR_T=wchar_t），
        // 而 onnxFile 是本地代码页(GBK)窄字符串，经 filesystem::path 转宽字符路径。
        const std::wstring widePath = std::filesystem::path(onnxFile).wstring();
        Ort::Session session(env, widePath.c_str(), options);
        const Ort::ModelMetadata meta = session.GetModelMetadata();

        Ort::AllocatorWithDefaultOptions alloc;
        const auto push = [&out](const std::string& key, std::string value) {
            if (value.empty())
                return;
            std::replace(value.begin(), value.end(), '\n', ' ');
            out += key;
            out += '=';
            out += value;
            out += '\n';
        };

        if (const auto s = meta.GetProducerNameAllocated(alloc)) push("author", s.get());
        if (const auto s = meta.GetGraphNameAllocated(alloc)) push("graph", s.get());
        if (const auto s = meta.GetDomainAllocated(alloc)) push("domain", s.get());
        if (const auto s = meta.GetDescriptionAllocated(alloc)) push("description", s.get());
        if (const auto s = meta.GetGraphDescriptionAllocated(alloc)) push("graph_description", s.get());
        const int64_t version = meta.GetVersion();
        if (version > 0)
            push("version", std::to_string(version));
        // 自定义元数据（YOLO 导出器写入的 names / imgsz / nc / date / email 等）。
        // GetCustomMetadataMapKeysAllocated 返回 std::vector，不能作 bool 判定，直接遍历。
        const auto keys = meta.GetCustomMetadataMapKeysAllocated(alloc);
        for (const auto& key : keys)
        {
            if (!key)
                continue;
            if (const auto value = meta.LookupCustomMetadataMapAllocated(key.get(), alloc))
                push(key.get(), value.get());
        }
    }
    catch (const std::exception& error)
    {
        // 提取失败时打印真实原因（如 onnxruntime 无法打开模型/路径乱码），
        // 便于定位；此时引擎不追加嵌入块，行为同旧版。
        std::cerr << "[TensorRT] 读取 ONNX 元数据失败，模型信息将不嵌入: " << error.what() << std::endl;
        return std::string();
    }
    catch (...)
    {
        std::cerr << "[TensorRT] 读取 ONNX 元数据失败（未知异常），模型信息将不嵌入。" << std::endl;
        return std::string();
    }
    return out;
}

// 从字节流末尾剥离内嵌模型信息块。布局见 nvinf.h。
// 返回 plan 字节数；无嵌入块时返回原始长度。
size_t StripEmbeddedMetadata(const char* data, size_t size)
{
    if (size < 12)
        return size;
    if (std::memcmp(data + size - 4, kEngineMetaMagic2, 4) != 0)
        return size;
    std::uint32_t metaLen = 0;
    std::memcpy(&metaLen, data + size - 8, sizeof(metaLen));
    if (size < 12ULL + metaLen)
        return size;
    if (std::memcmp(data + size - 12, kEngineMetaMagic1, 4) != 0)
        return size;
    return size - 12ULL - metaLen;
}
} // namespace

void Logger::log(nvinfer1::ILogger::Severity severity, const char* msg) noexcept
{
    if (severity <= nvinfer1::ILogger::Severity::kWARNING)
    {
        std::string devMsg = msg;

        std::string magicTag = "Serialization assertion plan->header.magicTag == rt::kPLAN_MAGIC_TAG failed.";
        std::string old_deserialization = "Using old deserialization call on a weight-separated plan file.";
        if (devMsg.find(magicTag) != std::string::npos || devMsg.find(old_deserialization) != std::string::npos)
        {
            std::cout << "[TensorRT] ERROR: This engine model is not suitable for execution. Please delete this engine model and set the ONNX version of this model in the settings. The program will export the model automatically." << std::endl;
        }
        else
        {
            std::cout << "[TensorRT] " << severityLevelName(severity) << ": " << msg << std::endl;
        }
    }
}

const char* Logger::severityLevelName(nvinfer1::ILogger::Severity severity)
{
    switch (severity)
    {
        case nvinfer1::ILogger::Severity::kINTERNAL_ERROR: return "INTERNAL_ERROR";
        case nvinfer1::ILogger::Severity::kERROR:          return "ERROR";
        case nvinfer1::ILogger::Severity::kWARNING:        return "WARNING";
        case nvinfer1::ILogger::Severity::kINFO:           return "INFO";
        case nvinfer1::ILogger::Severity::kVERBOSE:        return "VERBOSE";
        default:                                           return "UNKNOWN";
    }
}

nvinfer1::IBuilder* createInferBuilder()
{
    return nvinfer1::createInferBuilder(gLogger);
}

nvinfer1::INetworkDefinition* createNetwork(nvinfer1::IBuilder* builder)
{
    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    return builder->createNetworkV2(explicitBatch);
}

nvinfer1::IBuilderConfig* createBuilderConfig(nvinfer1::IBuilder* builder)
{
    return builder->createBuilderConfig();
}

nvinfer1::ICudaEngine* loadEngineFromFile(const std::string& engineFile, nvinfer1::IRuntime* runtime)
{
    std::ifstream file(engineFile, std::ios::binary);
    if (!file.good())
    {
        std::cerr << "[TensorRT] 无法打开引擎文件:" << std::endl;
        std::cerr << engineFile << std::endl;
        return nullptr;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize <= 0)
    {
        std::cerr << "[TensorRT] 引擎文件为空:" << std::endl;
        std::cerr << engineFile << std::endl;
        return nullptr;
    }
    const size_t size = static_cast<size_t>(fileSize);
    file.seekg(0, std::ios::beg);
    std::vector<char> engineData(size);
    file.read(engineData.data(), size);
    file.close();

    // 剥离尾部内嵌的模型信息块：只有 plan 字节应交给 deserializeCudaEngine，
    // 否则 TensorRT 会把 metadata 尾部当作 plan 的一部分解析并报错。
    const size_t planBytes = StripEmbeddedMetadata(engineData.data(), size);

    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(engineData.data(), planBytes);
    if (!engine)
    {
        std::cerr << "[TensorRT] 引擎文件反序列化失败:" << std::endl;
        std::cerr << engineFile << std::endl;
        return nullptr;
    }

    if (config.verbose)
    {
        std::cout << "[TensorRT] 引擎加载成功:" << std::endl;
        std::cout << engineFile << std::endl;
    }
    return engine;
}

std::string readEngineEmbeddedMetadata(const std::string& engineFile)
{
    std::ifstream file(engineFile, std::ios::binary);
    if (!file.good())
        return std::string();
    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < 12)
        return std::string();
    const size_t size = static_cast<size_t>(fileSize);
    file.seekg(0, std::ios::beg);
    std::vector<char> data(size);
    file.read(data.data(), size);
    file.close();

    // 布局：plan | meta(metaLen) | "KMX1"(4) | len(4,LE) | "KMX2"(4)
    if (std::memcmp(data.data() + size - 4, kEngineMetaMagic2, 4) != 0)
        return std::string();
    std::uint32_t metaLen = 0;
    std::memcpy(&metaLen, data.data() + size - 8, sizeof(metaLen));
    if (size < 12ULL + metaLen)
        return std::string();
    if (std::memcmp(data.data() + size - 12, kEngineMetaMagic1, 4) != 0)
        return std::string();
    return std::string(data.data() + size - 12 - metaLen, metaLen);
}

std::string buildEngineBindingMetadata(nvinfer1::ICudaEngine* engine)
{
    if (!engine)
        return std::string();
    std::string out;
    // TensorRT 10 以 IO tensor 概念替代了旧版 binding：getNbIOTensors / getIOTensorName /
    // getTensorShape / getTensorIOMode。
    const int32_t nbTensors = engine->getNbIOTensors();
    for (int32_t i = 0; i < nbTensors; ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (!name || !*name)
            continue;
        const bool isInput = engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT;
        const nvinfer1::Dims dims = engine->getTensorShape(name);

        const std::string prefix = std::string(isInput ? "input_" : "output_") + std::to_string(i);
        out += prefix;
        out += "_name=";
        out += name;
        out += '\n';
        out += prefix;
        out += "_shape=[";
        for (int32_t d = 0; d < dims.nbDims; ++d)
        {
            if (d != 0)
                out += ',';
            out += std::to_string(dims.d[d]);
        }
        out += "]\n";

        // 首个 4 维输入推导 imgsz=<H>x<W>，供 onnx_inspector 直接解析分辨率。
        if (isInput && dims.nbDims >= 4 && dims.d[2] > 0 && dims.d[3] > 0)
        {
            out += "imgsz=";
            out += std::to_string(dims.d[2]);
            out += 'x';
            out += std::to_string(dims.d[3]);
            out += '\n';
            break;   // 只取第一个输入的分辨率
        }
    }
    return out;
}

bool appendEmbeddedMetadataBlock(const std::string& engineFile, const std::string& metaText)
{
    if (metaText.empty())
        return false;
    // 已含嵌入块（新构建引擎或此前已补充）则不重复追加，避免叠加多个块。
    if (!readEngineEmbeddedMetadata(engineFile).empty())
        return false;

    std::ofstream file(engineFile, std::ios::binary | std::ios::app);
    if (!file.good())
        return false;
    const std::uint32_t metaLen = static_cast<std::uint32_t>(metaText.size());
    file.write(metaText.data(), metaText.size());
    file.write(kEngineMetaMagic1, 4);
    file.write(reinterpret_cast<const char*>(&metaLen), sizeof(metaLen));
    file.write(kEngineMetaMagic2, 4);
    file.flush();
    const bool ok = static_cast<bool>(file);
    file.close();
    return ok;
}

nvinfer1::ICudaEngine* buildEngineFromOnnx(const std::string& onnxFile, nvinfer1::ILogger& logger)
{
    // 【修复·空指针解引用】TensorRT 工厂函数在 CUDA 设备不可用、显存耗尽或
    // 插件注册失败时返回 nullptr。原实现直接 `builder->createNetworkV2(...)`，
    // 首个失败点即触发空指针解引用崩溃（用户侧表现为"点击导出模型后程序直接退出"）。
    nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(logger);
    if (!builder)
    {
        std::cerr << "[TensorRT] 创建 IBuilder 失败（CUDA 设备不可用或驱动异常）。" << std::endl;
        return nullptr;
    }

    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    nvinfer1::INetworkDefinition* network = builder->createNetworkV2(explicitBatch);
    if (!network)
    {
        std::cerr << "[TensorRT] 创建 INetworkDefinition 失败。" << std::endl;
        delete builder;
        return nullptr;
    }

    nvinfer1::IBuilderConfig* cfg = builder->createBuilderConfig();
    if (!cfg)
    {
        std::cerr << "[TensorRT] 创建 IBuilderConfig 失败。" << std::endl;
        delete network;
        delete builder;
        return nullptr;
    }

    nvonnxparser::IParser* parser = nvonnxparser::createParser(*network, logger);
    if (!parser)
    {
        std::cerr << "[TensorRT] 创建 ONNX Parser 失败。" << std::endl;
        delete cfg;
        delete network;
        delete builder;
        return nullptr;
    }

    ImGuiProgressMonitor progressMonitor;
    cfg->setProgressMonitor(&progressMonitor);
    TrtExportResetState();
    gIsTrtExporting = true;
    struct ScopedExportState
    {
        ~ScopedExportState()
        {
            std::lock_guard<std::mutex> lock(gProgressMutex);
            gProgressPhases.clear();
            gIsTrtExporting = false;
            gTrtExportCancelRequested = false;
            gTrtExportLastUpdateMs = TrtNowMs();
        }
    } exportState;

    if (!parser->parseFromFile(onnxFile.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)))
    {
        std::cerr << "[TensorRT] 解析 ONNX 文件失败:" << std::endl;
        std::cerr << onnxFile << std::endl;
        delete parser;
        delete network;
        delete builder;
        delete cfg;
        return nullptr;
    }

    // 【修复·空指针解引用】parseFromFile 成功不代表网络一定含输入张量
    // （常量折叠后无输入、或 ONNX 被裁剪损坏时 getNbInputs()==0）。
    // 原实现无条件对 getInput(0) 解引用，此类模型会直接崩溃而非报错回退。
    if (network->getNbInputs() <= 0)
    {
        std::cerr << "[TensorRT] ONNX 网络不含任何输入张量，无法构建引擎:" << std::endl;
        std::cerr << onnxFile << std::endl;
        delete parser;
        delete network;
        delete builder;
        delete cfg;
        return nullptr;
    }

    nvinfer1::ITensor* inputTensor = network->getInput(0);
    if (!inputTensor)
    {
        std::cerr << "[TensorRT] 读取输入张量失败。" << std::endl;
        delete parser;
        delete network;
        delete builder;
        delete cfg;
        return nullptr;
    }
    const char* inName = inputTensor->getName();
    nvinfer1::Dims inDims = inputTensor->getDimensions();
    // TensorRT 的 Dims::d 为 int64_t，而下游 Dims4 构造与日志一律按 int 使用。
    // 直接隐式窄化会触发 C4244，且在异常大的维度上会静默截断（例如 2^32+320
    // 会被截成 320，从而误判为"模型已固定输入"）。此处显式判定值域：
    // 非法（<=0）或超出 int 表达范围的维度统一视为 -1，走动态 shape 分支。
    const int64_t rawH = (inDims.nbDims >= 4) ? inDims.d[2] : -1;
    const int64_t rawW = (inDims.nbDims >= 4) ? inDims.d[3] : -1;
    constexpr int64_t kMaxDim = static_cast<int64_t>(std::numeric_limits<int>::max());
    int H = (rawH > 0 && rawH <= kMaxDim) ? static_cast<int>(rawH) : -1;
    int W = (rawW > 0 && rawW <= kMaxDim) ? static_cast<int>(rawW) : -1;

    bool fixedByModel = (H > 0 && W > 0);
    // 修复：config.fixed_input_size / detection_resolution 此前无锁读取，
    // 与运行期推理线程（trt_detector/dml_detector 持 configMutex 写）及 UI
    // 线程构成数据竞争。本函数在引擎构建线程执行，同样需持锁取快照。
    bool fixedByConfig = false;
    int detectionResolution = 0;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        fixedByConfig = config.fixed_input_size;
        detectionResolution = config.detection_resolution;
    }
    bool makeStatic = fixedByModel || fixedByConfig;

    if (fixedByConfig && (H <= 0 || W <= 0))
        H = W = detectionResolution;

    nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
    if (makeStatic)
    {
        nvinfer1::Dims4 d{ 1, 3, H, W };
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMIN, d);
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kOPT, d);
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMAX, d);
        if (config.verbose)
            std::cout << "[TensorRT] Static profile " << H << "x" << W << std::endl;
    }
    else
    {
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMIN, nvinfer1::Dims4{ 1, 3, 160, 160 });
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kOPT, nvinfer1::Dims4{ 1, 3, 320, 320 });
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMAX, nvinfer1::Dims4{ 1, 3, 640, 640 });
        if (config.verbose)
            std::cout << "[TensorRT] Dynamic profile 160/320/640" << std::endl;
    }

    cfg->addOptimizationProfile(profile);


    if (config.export_enable_fp16)
    {
        if (config.verbose)
            std::cout << "[TensorRT] Set FP16" << std::endl;
        cfg->setFlag(nvinfer1::BuilderFlag::kFP16);
    }
    if (config.export_enable_fp8)
    {
        if (config.verbose)
            std::cout << "[TensorRT] Set FP8" << std::endl;
        cfg->setFlag(nvinfer1::BuilderFlag::kFP8);
    }

    std::cout << "[TensorRT] Building engine (this may take several minutes)..." << std::endl;

    auto plan = builder->buildSerializedNetwork(*network, *cfg);
    if (!plan)
    {
        std::cerr << "[TensorRT] ERROR: Could not build the engine" << std::endl;
        delete parser;
        delete network;
        delete builder;
        delete cfg;
        return nullptr;
    }

    nvinfer1::IRuntime* runtime = nvinfer1::createInferRuntime(logger);
    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(plan->data(), plan->size());

    if (!engine)
    {
        std::cerr << "[TensorRT] ERROR: Could not create engine" << std::endl;
        delete plan;
        delete runtime;
        delete parser;
        delete network;
        delete builder;
        delete cfg;
        return nullptr;
    }

    // 【修复·冗余序列化】原实现在此调用 engine->serialize() 得到 serializedModel，
    // 但落盘写入用的是 plan->data()，serializedModel 从未被读取，仅被 delete。
    // 对一个 50~200 MB 的引擎而言，这是一次完整的重复序列化：
    // 额外耗时数百毫秒至数秒，且峰值内存多占用一份完整引擎字节。
    // 该计算结果无任何消费者，直接移除，落盘内容与行为完全不变。
    std::string engineFile = onnxFile.substr(0, onnxFile.find_last_of('.')) + ".engine";
    std::ofstream p(engineFile, std::ios::binary);
    if (!p)
    {
        std::cerr << "[TensorRT] 无法写入引擎文件:" << std::endl;
        std::cerr << engineFile << std::endl;
        delete engine;
        delete plan;
        delete runtime;
        delete parser;
        delete network;
        delete builder;
        delete cfg;
        return nullptr;
    }
    p.write(static_cast<const char*>(plan->data()), plan->size());
    // 【功能·模型信息内嵌】把 ONNX 自定义元数据追加到 plan 尾部，
    // 使"只有 .engine 文件"时也能推断模型信息（类别数/分辨率/names/版本等），
    // 不再强制依赖同目录 .onnx。文件布局（与 readEngineEmbeddedMetadata 一致）：
    //   plan | meta(metaLen) | "KMX1"(4) | metaLen(4,LE) | "KMX2"(4)
    // 末尾 12 字节固定，旧引擎无此块，加载时自动兼容。
    const std::string embeddedMeta = BuildEmbeddedMetadata(onnxFile);
    if (!embeddedMeta.empty())
    {
        const std::uint32_t metaLen = static_cast<std::uint32_t>(embeddedMeta.size());
        p.write(embeddedMeta.data(), embeddedMeta.size());
        p.write(kEngineMetaMagic1, 4);
        p.write(reinterpret_cast<const char*>(&metaLen), sizeof(metaLen));
        p.write(kEngineMetaMagic2, 4);
    }
    p.flush();
    // 【修复·静默写失败】原实现不检查流状态：磁盘满或路径只读时会写出
    // 一个截断/空的 .engine 文件，下次启动加载失败并再次触发整轮重编译。
    const bool engineWriteOk = static_cast<bool>(p);
    p.close();
    if (!engineWriteOk)
    {
        std::cerr << "[TensorRT] 引擎文件写入失败（磁盘空间或权限问题）:" << std::endl;
        std::cerr << engineFile << std::endl;
        std::error_code removeEc;
        std::filesystem::remove(engineFile, removeEc);
        delete engine;
        delete plan;
        delete runtime;
        delete parser;
        delete network;
        delete builder;
        delete cfg;
        return nullptr;
    }

    delete plan;
    delete parser;
    delete network;
    delete cfg;
    delete builder;
    // runtime 必须比它反序列化出的 engine 活得久，交给进程级持有器（见文件头注释）。
    RetainBuildRuntime(runtime);

    // 新引擎文件已落盘，立即失效模型列表 TTL 缓存，使 UI 下一帧即可看到新 .engine。
    invalidateModelListCache();

    std::cout << "[TensorRT] 引擎已构建并保存至:" << std::endl;
    std::cout << engineFile << std::endl;
    return engine;
}
#endif
