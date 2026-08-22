#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <sstream>
#include <string>
#include <filesystem>
#include <utility>
#include <fstream>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <vector>
#include <optional>

#include "onnx_inspector.h"

// Keep the donor CLI source unmodified while making its anonymous report functions available here.
#define main onnx_model_info_donor_main
#include "onnx_donor/main.cpp"
#undef main

namespace
{
// 说明（第 22 轮修复）：此处原有一个 CoutCapture 类，通过
// std::cout.rdbuf(stream_.rdbuf()) 把进程级标准输出缓冲区临时换成局部
// ostringstream 来"抓取"报告文本。该做法存在三重缺陷：
//   1) inspectOnnxModel 由 draw_capture.cpp 用 std::async 投递到后台线程执行，
//      抓取窗口内采集/推理/鼠标线程仍在向 std::cout 写日志，而 std::stringbuf
//      没有任何内部同步 —— 并发 sputn 会破坏 pptr/堆块，属未定义行为；
//   2) 析构时先恢复 rdbuf 再销毁 stream_，已取得旧 rdbuf 指针的其它线程会
//      写入已析构对象，构成悬垂写入；
//   3) 其它线程本该进控制台的日志被吞进报告文本，UI 上显示的模型信息被污染。
// 现改为向 PrintReport / PrintNaturalLanguage 显式传入局部 std::ostringstream，
// 完全不触碰全局 std::cout，上述三个问题一并消除。
OnnxInspectionResult failure(const std::string& message)
{
    return { false, message, message, message };
}

StartupOnnxReport g_startup_report;

std::string metadataValue(const ModelReport& report, const char* key)
{
    const auto it = report.custom_metadata.find(key);
    return it == report.custom_metadata.end() ? std::string() : it->second;
}

std::string shapeText(const std::vector<int64_t>& shape)
{
    std::ostringstream output;
    output << "[";
    for (std::size_t index = 0; index < shape.size(); ++index)
    {
        if (index != 0) output << ",";
        output << shape[index];
    }
    output << "]";
    return output.str();
}

std::string simpleVersion(const std::string& version)
{
    if (version.rfind("YOLOv", 0) == 0) return "v" + version.substr(5);
    if (version.rfind("YOLO", 0) == 0) return "v" + version.substr(4);
    return version.empty() ? "未知" : version;
}

std::string scaleText(const YoloInfo& yolo)
{
    if (yolo.scale == "s") return "small";
    if (yolo.scale == "n") return "nano";
    if (yolo.scale == "m") return "medium";
    if (yolo.scale == "l") return "large";
    if (yolo.scale == "x") return "xlarge";
    return yolo.scale.empty() ? "未知" : yolo.scale;
}

std::string decimalGrouped(long long value)
{
    const std::string digits = std::to_string(value);
    std::string output;
    output.reserve(digits.size() + digits.size() / 3);
    for (std::size_t index = 0; index < digits.size(); ++index)
    {
        if (index != 0 && (digits.size() - index) % 3 == 0) output += ',';
        output += digits[index];
    }
    return output;
}

std::string modelClassNames(const ModelReport& report)
{
    std::string names = metadataValue(report, "names");
    if (names.size() >= 2 && names.front() == '{' && names.back() == '}')
        names = names.substr(1, names.size() - 2);
    return names;
}

// ---------------------------------------------------------------------------
// 引擎内嵌模型信息（engine-embedded metadata）读取与解析。
//
// 布局与 tensorrt/nvinf.h 保持一致：plan | meta(metaLen) | "KMX1"(4) | len(4,LE) | "KMX2"(4)。
// 构建 engine 时由 nvinf.cpp 写入 ONNX 自定义元数据（"key=value\n" UTF-8），
// 这里在 .onnx 缺失时用同一协议把信息读回来，使"只部署 .engine"也能推断模型信息。
// ---------------------------------------------------------------------------
constexpr char kEmbeddedMetaMagic1[] = { 'K', 'M', 'X', '1' };
constexpr char kEmbeddedMetaMagic2[] = { 'K', 'M', 'X', '2' };

std::string readEngineEmbeddedMetadata(const std::filesystem::path& enginePath)
{
    std::ifstream file(enginePath, std::ios::binary);
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

    if (std::memcmp(data.data() + size - 4, kEmbeddedMetaMagic2, 4) != 0)
        return std::string();
    std::uint32_t metaLen = 0;
    std::memcpy(&metaLen, data.data() + size - 8, sizeof(metaLen));
    if (size < 12ULL + metaLen)
        return std::string();
    if (std::memcmp(data.data() + size - 12, kEmbeddedMetaMagic1, 4) != 0)
        return std::string();
    return std::string(data.data() + size - 12 - metaLen, metaLen);
}

// 把 "key=value\n" 文本解析进 custom_metadata。
void parseEmbeddedMetadata(const std::string& text, ModelReport& report)
{
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        report.custom_metadata[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

// 从 custom_metadata 推导 YoloInfo 字段（类别数 nc / 输入尺寸 imgsz / names / task），
// 覆盖 readModelReport 路径缺失的信息；未知字段保持原值。
void applyEmbeddedMetadataToYolo(ModelReport& report)
{
    YoloInfo& yolo = report.yolo;

    auto metadataInt = [&report](const std::string& key, int fallback) -> int {
        const auto it = report.custom_metadata.find(key);
        if (it == report.custom_metadata.end())
            return fallback;
        try
        {
            return std::stoi(it->second);
        }
        catch (...)
        {
            return fallback;
        }
    };

    if (yolo.classes < 0)
    {
        yolo.classes = metadataInt("nc", -1);
        // nc 缺失时按 names 字典 {0: 'a', 1: 'b'} 的索引数统计
        if (yolo.classes < 0)
        {
            const auto it = report.custom_metadata.find("names");
            if (it != report.custom_metadata.end())
            {
                int count = 0;
                const std::string& s = it->second;
                for (std::size_t i = 0; i + 1 < s.size(); ++i)
                {
                    if (std::isdigit(static_cast<unsigned char>(s[i])) && s[i + 1] == ':')
                    {
                        ++count;
                        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                            ++i;
                    }
                }
                yolo.classes = count > 0 ? count : -1;
            }
        }
    }

    if (yolo.width < 0 || yolo.height < 0)
    {
        const auto it = report.custom_metadata.find("imgsz");
        if (it != report.custom_metadata.end())
        {
            std::vector<int> dims;
            std::string current;
            // safeStoi：嵌入元数据 imgsz 字段可能被人为篡改为超出 int 范围的字符串，
            // std::stoi 会抛 std::out_of_range。此处加 try/catch 兜底跳过该字段，
            // 与 metadataInt lambda(L162-168) 防御策略一致。
            auto safeStoi = [](const std::string& s) -> std::optional<int>
            {
                try { return std::stoi(s); }
                catch (...) { return std::nullopt; }
            };
            for (const char c : it->second)
            {
                if (std::isdigit(static_cast<unsigned char>(c)))
                    current.push_back(c);
                else if (!current.empty())
                {
                    if (const auto v = safeStoi(current))
                        dims.push_back(*v);
                    current.clear();
                }
            }
            if (!current.empty())
            {
                if (const auto v = safeStoi(current))
                    dims.push_back(*v);
            }
            if (dims.size() == 1)
            {
                yolo.width = dims[0];
                yolo.height = dims[0];
            }
            else if (dims.size() >= 2)
            {
                yolo.height = dims[0];
                yolo.width = dims[1];
            }
        }
    }

    if (const auto it = report.custom_metadata.find("task"); it != report.custom_metadata.end())
        yolo.task = it->second;
    if (yolo.version.empty() || yolo.version == "unknown")
        yolo.version = "YOLO";   // 无 ONNX 元数据时无法确认具体大版本，用泛称
    if (yolo.classes >= 0 && yolo.width > 0 && yolo.height > 0)
        yolo.is_yolo = true;
}

std::string exactStartupText(const ModelReport& report, const std::string& summary)
{
    std::ostringstream output;
    output << "========================================================\n"
        << ".onnx模型信息输出\n"
        << summary << "\n"
        << "模型作者：" << metadataValue(report, "author") << "\n"
        << "导出时间：" << report.export_time << "\n"
        << "导出尺寸：" << report.yolo.width << "x" << report.yolo.height << "\n"
        << "构建版本：" << report.yolo.version << "（等价简写：" << simpleVersion(report.yolo.version) << "）\n"
        << "模型规模（Model Scale）：" << scaleText(report.yolo) << "\n"
        << "模型类别数量（Classes）：" << report.yolo.classes << "\n"
        << "训练权重总数（Parameters）：" << decimalGrouped(report.parameters) << "\n"
        << "模型张量计数（Tensor Count）：" << report.tensor_count << "\n";
    for (std::size_t index = 0; index < report.inputs.size(); ++index)
        output << "[" << index << "] " << report.inputs[index].name << "形状："
            << shapeText(report.inputs[index].shape) << "\n";
    for (std::size_t index = 0; index < report.outputs.size(); ++index)
        output << "[" << index << "] " << report.outputs[index].name << "形状："
            << shapeText(report.outputs[index].shape) << "\n";
    const auto metadata = [&report](const char* key) { return metadataValue(report, key); };
    output << "date = " << metadata("date") << "\n"
        << "author = " << metadata("author") << "\n"
        << "email = " << metadata("email") << "\n"
        << "imgsz = " << metadata("imgsz") << "\n"
        << "nc = " << metadata("nc") << "\n"
        << "names = " << metadata("names") << "\n"
        << "project = " << metadata("project") << "\n";
    output << "========================================================";
    return output.str();
}

bool readModelReport(const std::filesystem::path& modelPath, ModelReport& report, std::string& error)
{
    // 【编码修复】原实现 `Utf8ToW(modelPath.string())` 把 modelPath.string() 的
    // ANSI/GBK 窄字符串按 CP_UTF8 转换——含中文的路径会被解成乱码，onnxruntime
    // 打开失败，导致"放回 .onnx 也推断不了模型信息"。filesystem::path 内部保存
    // 正确的宽字符，直接 wstring() 返回最可靠的宽路径，与 GBK/UTF-8 无关。
    const std::wstring pathWide = modelPath.wstring();
    if (pathWide.empty())
    {
        error = "读取 ONNX 模型失败: 路径无效。";
        return false;
    }

    struct _stat64 fileStatus {};
    if (_wstat64(pathWide.c_str(), &fileStatus) != 0)
    {
        error = "找不到 ONNX 文件。";
        return false;
    }

    report.file_path = WToUtf8(pathWide);
    if (!GetFileInfo(pathWide, report.file_size, report.file_mtime))
    {
        report.file_size = "(unknown)";
        report.file_mtime = "(unknown)";
    }

    OnnxRuntimeReader runtimeReader;
    if (!runtimeReader.Read(pathWide, report))
    {
        error = "ONNX Runtime 无法读取该模型。";
        return false;
    }
    if (Ort::Session* session = runtimeReader.GetSession())
    {
        MetadataReader metadataReader;
        metadataReader.Read(*session, report);
    }
    OnnxProtoParser::Parse(pathWide, report);
    MergeSymbolic(report);
    YoloAnalyzer::Analyze(report);
    return true;
}
}

OnnxInspectionResult inspectOnnxModel(const std::string& modelPath)
{
    try
    {
        ModelReport report;
        std::string error;
        if (!readModelReport(std::filesystem::path(modelPath), report, error)) return failure(error);

        std::ostringstream reportStream;
        PrintReport(report, reportStream);
        const std::string fullText = reportStream.str();

        std::ostringstream naturalLanguageStream;
        PrintNaturalLanguage(report, naturalLanguageStream);
        const std::string briefText = naturalLanguageStream.str();

        return { true, fullText, briefText, fullText };
    }
    catch (const Ort::Exception& error)
    {
        return failure(std::string("ONNX Runtime 无法读取该模型: ") + error.what());
    }
    catch (const std::exception& error)
    {
        return failure(std::string("读取 ONNX 模型失败: ") + error.what());
    }
    catch (...)
    {
        return failure("读取 ONNX 模型时发生未知异常。");
    }
}

static std::pair<int,int> modelResolutionFromOnnxInputs(const ModelReport& report, int fallback)
{
    for (const auto& in : report.inputs)
    {
        const auto& s = in.shape;
        if (s.size() == 4 && s[2] > 0 && s[3] > 0)
            return { static_cast<int>(s[2]), static_cast<int>(s[3]) };
    }
    if (report.yolo.width > 0 && report.yolo.height > 0)
        return { report.yolo.width, report.yolo.height };
    return { fallback, fallback };
}

StartupOnnxReport inspectLoadedEngineOnnx(const std::string& modelPath, int runtimeResolution)
{
    // 降级结果：仅代表"拿不到 .onnx 元信息"，不应影响主程序继续启动。
    const auto degraded = [runtimeResolution](const std::string& reason) -> StartupOnnxReport
    {
        return { false,
            "智能推断，当前模型分辨率：" + std::to_string(runtimeResolution) + "x" + std::to_string(runtimeResolution)
                + "，模型类别数量：未知，模型版本：未知",
            "智能推断，当前模型类别数量：未知",
            "",
            reason };
    };

    // 第 22 轮修复：本函数原先没有任何异常边界。
    // readModelReport -> OnnxRuntimeReader::Read 里 std::make_unique<Ort::Session>
    // 在模型损坏/算子不受支持/onnxruntime.dll 版本不匹配时会抛 Ort::Exception，
    // 且 Read() 从不返回 false（只抛），因此 "ONNX Runtime 无法读取该模型" 分支
    // 实际不可达，异常会一路逃到 main 的顶层 catch —— 结果是一个纯信息展示功能
    // 把整个程序的启动流程终止掉（打印错误后 return -1）。
    // 现就地捕获并降级，主链路（引擎加载/推理/瞄准）不受影响。
    try
    {
        const std::filesystem::path selected(modelPath);
        const std::filesystem::path onnx = selected.extension() == ".onnx"
            ? selected
            : selected.parent_path() / (selected.stem().string() + ".onnx");
        ModelReport report;
        std::string error;
        if (!readModelReport(onnx, report, error))
        {
            // 【功能·模型信息内嵌】.onnx 缺失或不可读时，从 .engine 文件尾部
            // 读取构建期嵌入的 ONNX 元数据，使只部署 .engine 也能推断模型信息
            //（类别数 / 分辨率 / names / 版本），不再强制依赖同目录 .onnx。
            if (selected.extension() == ".engine")
            {
                const std::string embedded = readEngineEmbeddedMetadata(selected);
                if (!embedded.empty())
                {
                    parseEmbeddedMetadata(embedded, report);
                    applyEmbeddedMetadataToYolo(report);
                    // 绑定维度补充块只有 imgsz 没有 nc，类别数可能仍为 -1，此时显示"未知"。
                    const std::string classesText = report.yolo.classes >= 0
                        ? std::to_string(report.yolo.classes) : "未知";
                    const auto [summaryWidth, summaryHeight] = modelResolutionFromOnnxInputs(report, runtimeResolution);
                    const std::string summary = "智能推断，当前模型分辨率：" + std::to_string(summaryWidth) + "x" + std::to_string(summaryHeight)
                        + "，模型类别数量：" + classesText + "，模型版本：" + simpleVersion(report.yolo.version);
                    return { true,
                        summary,
                        "智能推断，当前模型类别数量：" + classesText,
                        modelClassNames(report),
                        exactStartupText(report, summary) };
                }
            }
            return degraded(error);
        }

        const auto [summaryWidth, summaryHeight] = modelResolutionFromOnnxInputs(report, runtimeResolution);
        const std::string summary = "智能推断，当前模型分辨率：" + std::to_string(summaryWidth) + "x" + std::to_string(summaryHeight)
            + "，模型类别数量：" + std::to_string(report.yolo.classes) + "，模型版本：" + simpleVersion(report.yolo.version);
        return { true,
            summary,
            "智能推断，当前模型类别数量：" + std::to_string(report.yolo.classes),
            modelClassNames(report),
            exactStartupText(report, summary) };
    }
    catch (const Ort::Exception& error)
    {
        return degraded(std::string("ONNX Runtime 无法读取该模型: ") + error.what());
    }
    catch (const std::exception& error)
    {
        return degraded(std::string("读取 ONNX 模型失败: ") + error.what());
    }
    catch (...)
    {
        return degraded("读取 ONNX 模型时发生未知异常。");
    }
}

const StartupOnnxReport& startupOnnxReport()
{
    return g_startup_report;
}

void publishStartupOnnxReport(StartupOnnxReport report)
{
    g_startup_report = std::move(report);
}
