#ifdef USE_CUDA
#ifndef NVINF_H
#define NVINF_H

#include "NvInfer.h"
#include "mybot.h"

class Logger : public nvinfer1::ILogger
{
public:
    void log(Severity severity, const char* msg) noexcept override;
    static const char* severityLevelName(Severity severity);
};

extern Logger gLogger;

nvinfer1::IBuilder* createInferBuilder();
nvinfer1::INetworkDefinition* createNetwork(nvinfer1::IBuilder* builder);
nvinfer1::IBuilderConfig* createBuilderConfig(nvinfer1::IBuilder* builder);

nvinfer1::ICudaEngine* loadEngineFromFile(const std::string& engineFile, nvinfer1::IRuntime* runtime);
nvinfer1::ICudaEngine* buildEngineFromOnnx(const std::string& onnxFile, nvinfer1::ILogger& logger);

// ---------------------------------------------------------------------------
// 引擎内嵌模型信息块（engine-embedded metadata）
//
// 背景：TensorRT .engine 文件是纯 plan 字节，不携带 ONNX 的自定义信息
//（类别名 names / 输入尺寸 imgsz / 类别数 nc / 作者等）。此前模型信息推断
// 完全依赖同目录 .onnx 文件；一旦只部署 .engine，UI 只能显示"类别数量：未知"。
// 本方案在构建 engine 时把 ONNX 自定义元数据序列化后追加到 plan 字节尾部，
// 使"只有 .engine"时同样可以推断模型信息。旧 engine（无此块）不受影响，整文件
// 仍按 plan 反序列化，完全向后兼容。
//
// 文件布局（从文件头向后）：
//   [plan bytes ...]
//   [meta bytes (metaLen)]
//   [magic1 'K','M','X','1' (4 bytes)]
//   [metaLen (uint32, little-endian, 4 bytes)]
//   [magic2 'K','M','X','2' (4 bytes)]
// 因此文件末尾 12 字节固定为 KMX1 + len + KMX2。
// ---------------------------------------------------------------------------
inline constexpr char kEngineMetaMagic1[] = { 'K', 'M', 'X', '1' };
inline constexpr char kEngineMetaMagic2[] = { 'K', 'M', 'X', '2' };

// 从 .engine 文件读取尾部嵌入的模型元数据（"key=value\n" 格式，UTF-8）。
// 文件不存在 / 无嵌入块 / 魔数或长度校验失败时返回空字符串。
std::string readEngineEmbeddedMetadata(const std::string& engineFile);

// 从已加载的 ICudaEngine 提取输入/输出张量名称与形状，序列化为 "key=value\n"
// 文本（key 形如 input_0_name / input_0_shape / output_0_name / output_0_shape，
// 以及由首个 4 维输入推导的 imgsz=<H>x<W>）。用于"只有 .engine、无 .onnx、
// 也无构建期嵌入块"时，仍能从引擎本身恢复模型分辨率等基本信息。
std::string buildEngineBindingMetadata(nvinfer1::ICudaEngine* engine);

// 把 metaText 按统一布局追加到 engineFile 尾部（plan | meta | "KMX1" | len | "KMX2"）。
// 文件已含嵌入块时不重复追加。返回是否写入成功。
bool appendEmbeddedMetadataBlock(const std::string& engineFile, const std::string& metaText);

#endif // NVINF_H
#endif
