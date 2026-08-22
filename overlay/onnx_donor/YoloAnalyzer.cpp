#include "YoloAnalyzer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace {

// 前向声明：MetaVal 在文件后部定义，但 DetermineVersion 等需要提前使用
const std::string& MetaVal(const std::map<std::string, std::string>& m, const std::string& k);

bool ContainsNoCase(const std::string& hay, const std::string& needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    std::string h, n;
    h.reserve(hay.size());
    n.reserve(needle.size());
    for (char c : hay) h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (char c : needle) n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return h.find(n) != std::string::npos;
}

// 统计 Python dict 中顶层键的数量。兼容三种格式：
//   "key": value  (JSON)
//   'key': value  (Python 单引号)
//   0: value      (Python 整数键，如 YOLO names)
int CountDictKeys(const std::string& s) {
    int n = 0;
    const size_t L = s.size();
    size_t i = 0;
    while (i < L) {
        // 双引号键: "key":
        if (s[i] == '"') {
            size_t j = i + 1;
            bool closed = false;
            while (j < L) {
                if (s[j] == '\\') { j += 2; continue; }
                if (s[j] == '"') { closed = true; break; }
                ++j;
            }
            if (!closed) break;
            size_t k = j + 1;
            while (k < L && (s[k] == ' ' || s[k] == '\t')) ++k;
            if (k < L && s[k] == ':') ++n;
            i = j + 1;
        }
        // 单引号键: 'key':
        else if (s[i] == '\'') {
            size_t j = i + 1;
            bool closed = false;
            while (j < L) {
                if (s[j] == '\\') { j += 2; continue; }
                if (s[j] == '\'') { closed = true; break; }
                ++j;
            }
            if (!closed) break;
            size_t k = j + 1;
            while (k < L && (s[k] == ' ' || s[k] == '\t')) ++k;
            if (k < L && s[k] == ':') ++n;
            i = j + 1;
        }
        // 整数键: 0: 'value' 或 0: "value"
        // 仅在 { 或 , 或空格 之后出现数字才视为键
        else if (std::isdigit(static_cast<unsigned char>(s[i])) &&
                 (i == 0 || s[i-1] == '{' || s[i-1] == ',' || s[i-1] == ' ' || s[i-1] == '\t')) {
            size_t j = i;
            while (j < L && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
            size_t k = j;
            while (k < L && (s[k] == ' ' || s[k] == '\t')) ++k;
            if (k < L && s[k] == ':') ++n;
            i = j;
        } else {
            ++i;
        }
    }
    return n;
}

int ParseIntMeta(const std::string& v) {
    try {
        return std::stoi(v);
    } catch (...) {
        return -1;
    }
}

// 从单个 output 形状推断类别数。
// 现代 YOLO（YOLO8/11/26）：无 objectness，最后一维 = 4 + classes
// YOLOv5：含 objectness，最后一维 = 5 + classes
// 无法仅从形状区分；优先现代格式（4+classes），因其更常见于当前模型。
// 此函数仅在没有 metadata 时作为最后兜底；有 metadata 时由 CountDictKeys/CountListItems 确定。
int InferClassesFromShape(const std::vector<int64_t>& shape) {
    if (shape.size() < 3) return -1;
    const int64_t a = shape[1];
    const int64_t b = shape[2];
    const int64_t feature = std::min(a, b);
    if (feature <= 4) return -1;
    // 现代 YOLO 优先（4 + classes）
    int c = static_cast<int>(feature - 4);
    if (c > 0 && c < feature) return c;
    // YOLOv5 兜底（5 + classes）
    c = static_cast<int>(feature - 5);
    if (c > 0 && c < feature) return c;
    (void)a; (void)b;
    return -1;
}

// 从文本中提取 YOLO 版本，如 YOLO26 / YOLOv8 / YOLO11
std::string ExtractVersion(const std::string& text) {
    const std::string low = [&]() {
        std::string s;
        for (char c : text) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return s;
    }();
    const size_t pos = low.find("yolo");
    if (pos == std::string::npos) return std::string();
    size_t i = pos + 4;
    // low 已在上方逐字符 tolower，不可能出现 'V'，原先的 `|| low[i] == 'V'` 是恒假的死条件。
    const bool has_v = (i < low.size() && low[i] == 'v');
    if (has_v) ++i;
    std::string digits;
    while (i < low.size() && std::isdigit(static_cast<unsigned char>(low[i]))) {
        digits.push_back(text[i]);
        ++i;
    }
    if (digits.empty()) return std::string();
    // 前缀：是否带 'v' 完全由原文是否真的写了 'v' 决定，不再凭大小写猜测。
    // 原实现只有在原文恰好是全大写 "YOLO" 时才尊重 has_v，其余一律强制拼 "YOLOv"，
    // 于是 "Yolo11" / "yolo26" 这类元数据会被输出成 "YOLOv11" / "YOLOv26"，
    // 而 YOLO11、YOLO26 的官方命名本就不带 v，导致模型分析报告显示错误型号名。
    const std::string prefix = has_v ? "YOLOv" : "YOLO";
    return prefix + digits;
}

// 扫描 node/initializer 名是否含指定子串（版本指纹）
bool NameContains(const std::vector<std::string>& names, const std::string& needle) {
    for (const auto& n : names) {
        if (n.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ---- 版本判定（S3）：结构指纹优先，metadata 兜底 ----
// 严格参考 Analyze_onnx_model_info 的 DetermineVersion 逻辑：
// 匹配顺序 26 -> 12 -> 11 -> 8 -> 5u -> 5（新版本兼容旧模块名）
std::string DetermineVersion(const ModelReport& report) {
    const VersionFingerprint& fp = report.fingerprint;

    const bool has_one2one_node = NameContains(fp.initializer_names, "one2one") ||
                                  NameContains(fp.node_names, "one2one");
    const bool has_A2C2f = NameContains(fp.node_names, "A2C2f") ||
                           NameContains(fp.node_names, "AAttn") ||
                           NameContains(fp.initializer_names, "A2C2f") ||
                           NameContains(fp.initializer_names, "AAttn");
    const bool has_C2PSA = NameContains(fp.node_names, "C2PSA") ||
                           NameContains(fp.initializer_names, "C2PSA");
    const bool has_C2f  = NameContains(fp.node_names, "C2f") ||
                          NameContains(fp.initializer_names, "C2f");
    const bool has_C3   = NameContains(fp.node_names, "C3") ||
                          NameContains(fp.initializer_names, "C3");
    const bool has_anchor_grid = NameContains(fp.initializer_names, "anchor_grid");
    const bool has_dfl = NameContains(fp.initializer_names, "dfl.conv.weight");
    const bool has_cv2_cv3 = NameContains(fp.node_names, "cv2") ||
                             NameContains(fp.node_names, "cv3") ||
                             NameContains(fp.initializer_names, "cv2") ||
                             NameContains(fp.initializer_names, "cv3");

    // 输出形状检查：[1, 300, 6] -> YOLO26 end2end
    bool shape_one2one = false;
    if (!report.outputs.empty()) {
        const auto& s = report.outputs[0].shape;
        if (s.size() == 3 && s[1] > 100 && s[1] < 500 && s[2] == 6) {
            shape_one2one = true;
        }
    }

    if (has_one2one_node || shape_one2one) return "YOLO26";
    if (has_A2C2f) return "YOLO12";
    if (has_C2PSA && !has_A2C2f && !has_one2one_node) return "YOLO11";
    if (has_C2f && !has_C2PSA && !has_A2C2f && !has_one2one_node) return "YOLOv8";
    if (has_C3 && has_cv2_cv3 && has_dfl && !has_anchor_grid) return "YOLOv5u";
    if (has_C3 && has_anchor_grid) return "YOLOv5";

    // metadata 兜底：producer / graph_name / description / version 中提取 YOLOxx
    const std::string hay = report.producer + " " + report.graph_name + " " +
                            MetaVal(report.custom_metadata, "description") + " " +
                            MetaVal(report.custom_metadata, "version");
    const std::string from_meta = ExtractVersion(hay);
    if (!from_meta.empty()) return from_meta;

    return "Unknown";
}

// ---- 参数量容差匹配（±5%）----
// 严格参考 Analyze_onnx_model_info 的 MatchParams 逻辑。
// version 已知时仅匹配对应版本；命中记录首个匹配并收集全部候选。
ParamsMatch MatchParams(long long params, const std::string& version) {
    ParamsMatch result;
    if (params <= 0) return result;

    constexpr double tolerance = 0.05;  // ±5%

    for (size_t i = 0; i < kYoloRefCount; ++i) {
        const auto& entry = kYoloRefTable[i];

        if (!version.empty() && version != "Unknown") {
            if (entry.version != version) continue;
        }

        int64_t ref = entry.params_exact;
        if (ref <= 0) ref = static_cast<int64_t>(entry.params_m * 1'000'000);
        if (ref <= 0) continue;

        const double dev = std::abs(static_cast<double>(params) - static_cast<double>(ref))
                           / static_cast<double>(ref);
        if (dev <= tolerance) {
            if (!result.matched) {
                result.matched = true;
                result.ref_version = entry.version;
                result.ref_scale = entry.scale;
                result.params_reference = ref;
                result.deviation_pct = dev * 100.0;
            }
            result.candidates.push_back({entry.version, entry.scale, ref, dev * 100.0});
        }
    }
    return result;
}

// 旧的估算方法（fallback，数据不足时）
std::string EstimateScaleByParams(long long params) {
    if (params < 3'000'000LL) return "n";
    if (params < 10'000'000LL) return "s";
    if (params < 30'000'000LL) return "m";
    if (params < 70'000'000LL) return "l";
    return "x";
}

// 统计 Python list 中字符串元素个数，如 "['a', 'b', 'c']" -> 3
int CountListItems(const std::string& s) {
    if (s.empty() || s[0] != '[') return 0;
    int n = 0;
    bool in_str = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (!in_str) {
            if (c == '\'' || c == '"') { in_str = true; quote = c; ++n; }
        } else {
            if (c == '\\') { ++i; continue; }  // 跳过转义字符
            if (c == quote) { in_str = false; quote = 0; }
        }
    }
    return n;
}

bool IsYoloTask(const std::string& t) {
    return t == "detect" || t == "segment" || t == "pose" ||
           t == "classify" || t == "obb";
}

// 解析 JSON 整数数组，如 "[512, 512]" -> w=512, h=512
// 支持 "[H, W]" 或 "[W, H]" 格式；两个值相等时最可靠
bool ParseImgszArray(const std::string& s, int& w, int& h) {
    if (s.empty() || s[0] != '[') return false;
    size_t i = 1;
    int vals[2] = { -1, -1 };
    int idx = 0;
    while (i < s.size() && idx < 2) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') { ++i; continue; }
        if (s[i] == ']') break;
        if (s[i] == ',') { ++i; ++idx; continue; }
        if (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-') {
            char* end = nullptr;
            long long v = std::strtoll(s.c_str() + i, &end, 10);
            if (end == s.c_str() + i) return false;
            vals[idx] = static_cast<int>(v);
            i = static_cast<size_t>(end - s.c_str());
            continue;
        }
        break;
    }
    if (vals[0] > 0 && vals[1] > 0) {
        w = vals[1];  // Ultralytics imgsz = [H, W]，索引 1 为宽度
        h = vals[0];
        return true;
    }
    if (vals[0] > 0) { w = h = vals[0]; return true; }
    return false;
}

const std::string& MetaVal(const std::map<std::string, std::string>& m, const std::string& k) {
    static const std::string empty;
    auto it = m.find(k);
    return it == m.end() ? empty : it->second;
}

}  // namespace

void YoloAnalyzer::Analyze(ModelReport& report) {
    YoloInfo& y = report.yolo;

    // ---- task ----
    auto it = report.custom_metadata.find("task");
    if (it != report.custom_metadata.end() && !it->second.empty()) {
        y.task = it->second;
        std::string low;
        for (char c : y.task) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        y.task = low;
    }

    // ---- classes ----
    int classes = -1;
    // 方法1：metadata（names dict/lists / classes / nc）
    auto names_it = report.custom_metadata.find("names");
    if (names_it != report.custom_metadata.end()) {
        // JSON dict: {"0": "cat", "1": "dog", ...}
        int n = CountDictKeys(names_it->second);
        if (n <= 0) {
            // Python list: ['cat', 'dog', ...]
            n = CountListItems(names_it->second);
        }
        if (n > 0) classes = n;
    }
    if (classes < 0) {
        for (const char* k : {"classes", "nc"}) {
            auto m = report.custom_metadata.find(k);
            if (m != report.custom_metadata.end()) {
                const int v = ParseIntMeta(m->second);
                if (v > 0) { classes = v; break; }
            }
        }
    }
    // 方法2：output shape 推断（取第一个 output）
    if (classes < 0 && !report.outputs.empty()) {
        classes = InferClassesFromShape(report.outputs[0].shape);
    }
    y.classes = classes;

    // ---- input size ----
    // 方法1：input shape [N,C,H,W]
    if (!report.inputs.empty()) {
        const auto& shape = report.inputs[0].shape;
        if (shape.size() >= 4 && shape[2] > 0 && shape[3] > 0) {
            y.height = static_cast<int>(shape[2]);
            y.width = static_cast<int>(shape[3]);
        }
    }
    // 方法2：metadata imgsz / img_size（先 JSON 数组 "[H,W]"，再纯整数 "640"）
    if (y.width <= 0 || y.height <= 0) {
        for (const char* k : {"imgsz", "img_size", "image_size"}) {
            auto m = report.custom_metadata.find(k);
            if (m == report.custom_metadata.end()) continue;
            // 尝试 JSON 数组格式：如 "[512, 512]"
            int w = -1, h = -1;
            if (ParseImgszArray(m->second, w, h) && w > 0 && h > 0) {
                y.width = w;
                y.height = h;
                break;
            }
            // 尝试纯整数："640"
            const int v = ParseIntMeta(m->second);
            if (v > 0) { y.width = v; y.height = v; break; }
        }
    }
    // 方法3：动态 -> 不臆测

    // ---- version & scale（严格参考 Analyze_onnx_model_info 的 S3/S4 判定逻辑）----
    // S3 版本：结构指纹（节点/初始化器名）优先，metadata 兜底
    y.version = DetermineVersion(report);

    // S4 规模：首层 Conv 宽度指纹为主、参数量 ±5% 容差为辅
    y.first_conv_cout = report.fingerprint.first_conv_cout;
    std::string scale_from_width;
    if (y.first_conv_cout > 0) {
        for (size_t i = 0; i < kScaleCount; ++i) {
            if (y.first_conv_cout == kWidthFingerprints[i]) {
                scale_from_width = kScaleLetters[i];
                break;
            }
        }
    }
    y.params_match = MatchParams(report.parameters, y.version);

    // 仲裁：YOLO26 参数量优先；其它版本首层通道为主、参数量为辅；数据不足走粗粒度估算
    const bool yolo26_path = (y.version == "YOLO26");
    if (yolo26_path && y.params_match.matched) {
        y.scale = y.params_match.ref_scale;
        y.scale_estimated = false;
    } else if (!scale_from_width.empty()) {
        y.scale = scale_from_width;
        y.scale_estimated = false;
    } else if (y.params_match.matched) {
        y.scale = y.params_match.ref_scale;
        y.scale_estimated = false;
    } else {
        y.scale = EstimateScaleByParams(report.parameters);
        y.scale_estimated = true;
    }

    // ---- is_yolo 判定 ----
    if (IsYoloTask(y.task)) {
        y.is_yolo = true;
    } else if (ContainsNoCase(report.producer, "ultralytics") ||
               ContainsNoCase(report.graph_name, "yolo") ||
               !y.version.empty()) {
        y.is_yolo = true;
    } else if (classes > 0 && !report.outputs.empty()) {
        // 形状匹配检测头：残留判定
        const auto& s = report.outputs[0].shape;
        if (s.size() >= 3) {
            const int64_t feature = std::min(s[1], s[2]);
            if (feature >= 5) y.is_yolo = true;
        }
    }

    // ---- model.export() 识别 ----
    {
        const bool has_ultra_sig =
            report.custom_metadata.find("task") != report.custom_metadata.end() &&
            report.custom_metadata.find("stride") != report.custom_metadata.end() &&
            (report.custom_metadata.find("names") != report.custom_metadata.end() ||
             report.custom_metadata.find("nc") != report.custom_metadata.end());
        if (ContainsNoCase(report.producer, "pytorch") && has_ultra_sig) {
            report.export_method = "Ultralytics model.export()";
        }
    }

    // ---- 导出时间 ----
    auto date_it = report.custom_metadata.find("date");
    if (date_it != report.custom_metadata.end() && !date_it->second.empty()) {
        report.export_time = date_it->second;
    } else {
        report.export_time = report.file_mtime;
    }
}
