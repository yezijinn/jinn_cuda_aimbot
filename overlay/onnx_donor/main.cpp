#include <windows.h>
#include <shellapi.h>

#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "Common.h"
#include "JsonWriter.h"
#include "MetadataReader.h"
#include "OnnxProtoParser.h"
#include "OnnxRuntimeReader.h"
#include "YoloAnalyzer.h"

namespace {

std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string FormatInt(long long v) {
    std::ostringstream os;
    os.imbue(std::locale(""));
    os << v;
    return os.str();
}

bool GetFileInfo(const std::wstring& path, std::string& size_str, std::string& mtime_str) {
    struct _stat64 st {};
    if (_wstat64(path.c_str(), &st) != 0) return false;
    double s = static_cast<double>(st.st_size);
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (s >= 1024.0 && u < 4) { s /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << s << " " << units[u];
    size_str = os.str();

    time_t t = st.st_mtime;
    struct tm tm_val {};
    localtime_s(&tm_val, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_val);
    mtime_str = buf;
    return true;
}

std::string ShapeToString(const std::vector<int64_t>& shape) {
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i) os << ",";
        os << shape[i];
    }
    os << "]";
    return os.str();
}

void MergeSymbolic(ModelReport& report) {
    auto merge = [&](std::vector<IoInfo>& ios,
                     const std::map<std::string, std::vector<std::string>>& sym) {
        for (auto& io : ios) {
            auto it = sym.find(io.name);
            if (it == sym.end()) continue;
            for (size_t d = 0; d < io.symbolic.size(); ++d) {
                if (io.symbolic[d].empty() && d < it->second.size() && !it->second[d].empty()) {
                    io.symbolic[d] = it->second[d];
                }
            }
        }
    };
    merge(report.inputs, report.input_symbolic);
    merge(report.outputs, report.output_symbolic);
}

// 报告输出统一走显式 std::ostream& 参数，禁止在函数内部直接写 std::cout。
// 原因：宿主程序（aim）会在后台线程中调用这些函数抓取报告文本；若沿用
// std::cout 再由调用方替换其 rdbuf，会全进程改写标准输出缓冲区，与采集/
// 推理/鼠标线程的并发日志写入构成数据竞争（std::stringbuf 无内部同步），
// 且恢复 rdbuf 后缓冲区析构还会造成悬垂写入。默认实参保证 CLI 行为不变。
void PrintIo(const std::string& label, const std::vector<IoInfo>& ios, bool is_output,
             std::ostream& os = std::cout) {
    os << "\n========== " << label << " ==========\n\n";
    os << (is_output ? "Output" : "Input") << " Count:\n";
    os << ios.size() << "\n\n";

    for (size_t i = 0; i < ios.size(); ++i) {
        const IoInfo& io = ios[i];
        os << (is_output ? "output" : "input") << i << "\n\n";
        os << "Name:\n" << io.name << "\n\n";
        os << "Shape:\n" << ShapeToString(io.shape) << "\n\n";
        os << "Type:\n" << io.type << "\n\n";

        if (io.dynamic) {
            os << "Dynamic:\n" << "yes\n\n";
            bool any_sym = false;
            for (size_t d = 0; d < io.symbolic.size(); ++d) {
                if (!io.symbolic[d].empty()) {
                    if (!any_sym) { os << "Symbolic Dimensions:\n"; any_sym = true; }
                    os << "  dim[" << d << "] = " << io.symbolic[d] << "\n";
                }
            }
            if (any_sym) os << "\n";
        } else {
            os << "Dynamic:\n" << "no\n\n";
        }
    }
}

void PrintReport(const ModelReport& r, std::ostream& os = std::cout) {
    os << "========== ONNX MODEL INFO ==========\n\n";
    os << "File:\n" << r.file_path << "\n\n";
    os << "Size:\n" << r.file_size << "\n\n";
    os << "Modified:\n" << r.file_mtime << "\n\n";
    os << "Producer:\n" << (r.producer.empty() ? "(unknown)" : r.producer) << "\n\n";
    os << "Domain:\n" << (r.domain.empty() ? "(default)" : r.domain) << "\n\n";
    os << "Graph Name:\n" << (r.graph_name.empty() ? "(unnamed)" : r.graph_name) << "\n\n";
    os << "Model Version:\n" << r.model_version << "\n\n";
    os << "IR Version:\n" << r.ir_version << "\n\n";

    os << "Opset:\n";
    for (const auto& op : r.opset_import) {
        const std::string dom = op.first.empty() ? "(default)" : op.first;
        os << "  " << dom << " = " << op.second << "\n";
    }
    if (r.opset_import.empty()) os << "  (unknown)\n";
    os << "\n";

    os << "========== METADATA ==========\n\n";
    if (r.custom_metadata.empty()) {
        os << "(none)\n\n";
    } else {
        for (const auto& kv : r.custom_metadata) {
            os << kv.first << ":\n" << kv.second << "\n\n";
        }
    }

    PrintIo("INPUT", r.inputs, /*is_output=*/false, os);
    PrintIo("OUTPUT", r.outputs, /*is_output=*/true, os);

    os << "========== YOLO ANALYSIS ==========\n\n";
    if (!r.yolo.is_yolo) {
        os << "Detected:\nNot a YOLO model (or undetermined)\n\n";
    } else {
        os << "Detected:\nYOLO series\n\n";
    }
    os << "Task:\n"
              << (r.yolo.task.empty() ? "(unknown)" : r.yolo.task) << "\n\n";
    os << "Classes:\n"
              << (r.yolo.classes >= 0 ? std::to_string(r.yolo.classes) : "(unknown)") << "\n\n";

    if (r.yolo.width > 0 && r.yolo.height > 0) {
        os << "Input Size:\n" << r.yolo.width << "x" << r.yolo.height << "\n\n";
    } else {
        os << "Input Size:\nDynamic (cannot determine exact size)\n\n";
    }
    os << "Model Scale:\n"
              << r.yolo.scale << (r.yolo.scale_estimated ? " (estimated)" : "") << "\n\n";
    os << "Version:\n"
              << (r.yolo.version.empty() ? "unknown" : r.yolo.version) << "\n\n";

    if (!r.export_method.empty()) {
        os << "Export Method:\n" << r.export_method << "\n\n";
    }
    if (!r.export_time.empty()) {
        os << "Export Time:\n" << r.export_time << "\n\n";
    }

    os << "========== PARAMETER ==========\n\n";
    os << "Parameters:\n" << FormatInt(r.parameters) << "\n\n";
    os << "Tensor Count:\n" << r.tensor_count << "\n\n";
    os << "=====================================\n";
}

std::string ScaleToChinese(const std::string& scale, bool estimated) {
    if (scale == "n") return estimated ? "nano（估算）" : "nano";
    if (scale == "s") return estimated ? "small（估算）" : "small";
    if (scale == "m") return estimated ? "medium（估算）" : "medium";
    if (scale == "l") return estimated ? "large（估算）" : "large";
    if (scale == "x") return estimated ? "xlarge（估算）" : "xlarge";
    return estimated ? scale + "（估算）" : scale;
}

std::string VersionToSimple(const std::string& v) {
    // YOLOv5→v5, YOLOv8→v8, YOLO11→v11, YOLO26→v26
    if (v.empty()) return "未知";
    std::string s = v;
    // 去掉前缀 "YOLOv" 或 "YOLO"
    if (s.size() >= 5 && s.substr(0, 5) == "YOLOv") return "v" + s.substr(5);
    if (s.size() >= 4 && s.substr(0, 4) == "YOLO") return "v" + s.substr(4);
    return s;
}

void PrintNaturalLanguage(const ModelReport& r, std::ostream& os = std::cout) {
    os << "========================================================\n";
    os << "  ONNX 模型自然语言分析报告\n";
    os << "========================================================\n\n";

    // 基本信息
    os << "◆ 基本信息\n\n";
    os << "文件路径：" << r.file_path << "\n";
    os << "文件体积：" << r.file_size << "\n";
    os << "文件修改时间：" << r.file_mtime << "\n";
    if (!r.producer.empty())
        os << "导出框架：" << r.producer << "\n";
    if (!r.graph_name.empty())
        os << "计算图名称：" << r.graph_name << "\n";
    os << "IR 版本：" << r.ir_version << "\n";
    if (!r.opset_import.empty()) {
        os << "算子集版本：opset ";
        for (size_t i = 0; i < r.opset_import.size(); ++i) {
            if (i) os << ", ";
            os << r.opset_import[i].first << "=" << r.opset_import[i].second;
        }
        os << "\n";
    }
    os << "\n";

    // 模型作者/导出信息
    auto author_it = r.custom_metadata.find("author");
    auto date_it = r.custom_metadata.find("date");
    auto desc_it = r.custom_metadata.find("description");
    auto task_it = r.custom_metadata.find("task");

    if (author_it != r.custom_metadata.end())
        os << "模型作者：" << author_it->second << "\n";
    if (!r.export_time.empty())
        os << "模型导出时间：" << r.export_time << "\n";
    if (desc_it != r.custom_metadata.end())
        os << "模型描述：" << desc_it->second << "\n";
    if (!r.export_method.empty())
        os << "导出方法：" << r.export_method << "\n";
    os << "\n";

    // YOLO 分析
    if (r.yolo.is_yolo) {
        os << "◆ YOLO 模型分析\n\n";
        if (!r.yolo.task.empty())
            os << "任务类型：" << r.yolo.task
                      << (r.yolo.task == "detect" ? "（目标检测）" :
                          r.yolo.task == "segment" ? "（实例分割）" :
                          r.yolo.task == "pose" ? "（姿态估计）" :
                          r.yolo.task == "classify" ? "（图像分类）" :
                          r.yolo.task == "obb" ? "（旋转框检测）" : "")
                      << "\n";
        if (r.yolo.width > 0 && r.yolo.height > 0)
            os << "模型导出尺寸：" << r.yolo.width << "x" << r.yolo.height << "\n";
        if (!r.yolo.version.empty())
            os << "模型构建版本：" << r.yolo.version
                      << "（等价简写：" << VersionToSimple(r.yolo.version) << "）\n";
        else
            os << "模型构建版本：未知\n";
        os << "模型规模（Model Scale）：" << ScaleToChinese(r.yolo.scale, r.yolo.scale_estimated) << "\n";
        os << "模型类别数量（Classes）："
                  << (r.yolo.classes >= 0 ? std::to_string(r.yolo.classes) : "未知") << "\n";
        os << "\n";
    }

    // 参数统计
    os << "◆ 参数统计\n\n";
    os << "训练权重总数（Parameters）：" << FormatInt(r.parameters) << "\n";
    os << "模型张量计数（Tensor Count）：" << r.tensor_count << "\n";
    if (r.is_dynamic)
        os << "动态模型：是（包含动态维度）\n";
    os << "\n";

    // 输入张量
    os << "◆ 模型输入（" << r.inputs.size() << " 个）\n\n";
    for (size_t i = 0; i < r.inputs.size(); ++i) {
        const auto& io = r.inputs[i];
        os << "[" << i << "] " << io.name << "\n";
        os << "    形状：" << ShapeToString(io.shape) << "\n";
        os << "    数据类型：" << io.type << "\n";
        if (io.dynamic) os << "    动态维度：是\n";
        os << "\n";
    }

    // 输出张量
    os << "◆ 模型输出（" << r.outputs.size() << " 个）\n\n";
    for (size_t i = 0; i < r.outputs.size(); ++i) {
        const auto& io = r.outputs[i];
        os << "[" << i << "] " << io.name << "\n";
        os << "    形状：" << ShapeToString(io.shape) << "\n";
        os << "    数据类型：" << io.type << "\n";
        if (io.dynamic) os << "    动态维度：是\n";
        os << "\n";
    }

    // 导出参数
    auto args_it = r.custom_metadata.find("args");
    if (args_it != r.custom_metadata.end() && !args_it->second.empty()) {
        os << "◆ 模型导出参数\n\n";
        os << args_it->second << "\n\n";
    }

    // 全部 METADATA
    os << "◆ 模型内置信息（METADATA）\n\n";
    if (r.custom_metadata.empty()) {
        os << "（无）\n";
    } else {
        for (const auto& kv : r.custom_metadata) {
            // args/date/author/description 等已有单独展示的可跳过或仍然展示
            os << kv.first << " = " << kv.second << "\n";
        }
    }
    os << "\n========================================================\n";
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::cout << std::boolalpha;

    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wargv == nullptr || argc < 2) {
        std::cerr << "Usage: model_info.exe <model.onnx> [--json] [--text] [--out <file>]\n";
        if (wargv) LocalFree(wargv);
        return 1;
    }

    std::wstring model_w;
    bool json_mode = false;
    bool text_mode = false;
    std::string json_out;
    std::wstring json_out_w;
    bool out_specified = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = wargv[i];
        if (a == L"--json") {
            json_mode = true;
        } else if (a == L"--text") {
            text_mode = true;
        } else if (a == L"--out") {
            if (i + 1 < argc) { const std::wstring w = wargv[++i]; json_out = WToUtf8(w); json_out_w = w; out_specified = true; }
        } else if (model_w.empty()) {
            model_w = a;
        }
    }
    if (wargv) LocalFree(wargv);

    if (model_w.empty()) {
        std::cerr << "Error: missing model path.\n";
        return 1;
    }

    const std::string model_path = WToUtf8(model_w);

    // 文件存在性检查
    {
        struct _stat64 st {};
        if (_wstat64(model_w.c_str(), &st) != 0) {
            std::cerr << "Error: file not found: " << model_path << "\n";
            return 1;
        }
    }

    ModelReport report;
    report.file_path = model_path;
    if (!GetFileInfo(model_w, report.file_size, report.file_mtime)) {
        report.file_size = "(unknown)";
        report.file_mtime = "(unknown)";
    }

    try {
        OnnxRuntimeReader rt_reader;
        if (!rt_reader.Read(model_w, report)) {
            std::cerr << "Error: failed to load model.\n";
            return 1;
        }

        Ort::Session* session = rt_reader.GetSession();
        if (session) {
            MetadataReader meta_reader;
            meta_reader.Read(*session, report);
        }

        OnnxProtoParser::Parse(model_w, report);
        MergeSymbolic(report);
        YoloAnalyzer::Analyze(report);
    } catch (const Ort::Exception& e) {
        std::cerr << "ONNX Runtime Error:\n" << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error:\n" << e.what() << "\n";
        return 1;
    }

    if (json_mode) {
        if (!out_specified) {
            std::wstring p = model_w;
            const size_t dot = p.find_last_of(L'.');
            if (dot != std::wstring::npos) p = p.substr(0, dot);
            p += L".json";
            json_out = WToUtf8(p);
            json_out_w = p;
        }
        // 中文路径修复：json_out 是 UTF-8 字符串，std::ofstream 的窄字符重载按 GBK 解释，
        // 含中文时打不开输出文件。改用 std::filesystem::path 构造 ofstream —— 这是 C++17
        // 标准重载（非 MSVC 扩展），在 MSVC/GCC/Clang 上一致地按平台宽字符打开，任意
        // Unicode 路径都能写入，且可移植。注意：直接 std::ofstream(std::wstring,...) 是
        // MSVC 非标准扩展，GCC/Clang 不可编译。
        std::ofstream ofs(std::filesystem::path(json_out_w), std::ios::binary);
        if (!ofs) {
            std::cerr << "Error: cannot write JSON file: " << json_out << "\n";
            return 1;
        }
        JsonWriter::Write(report, ofs);
        std::cout << "JSON written to: " << json_out << "\n";
    } else if (text_mode) {
        PrintNaturalLanguage(report);
    } else {
        PrintReport(report);
    }

    return 0;
}
