#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 单个输入/输出张量的描述信息
struct IoInfo {
    std::string name;                                  // 名称
    std::vector<int64_t> shape;                        // 各维度尺寸，-1 表示动态维度
    std::vector<std::string> symbolic;                 // 与 shape 对齐的符号维度名（如 batch/height/width），无则为空串
    std::string type;                                  // 元素类型显示名，如 float32/float16
    bool dynamic = false;                              // 是否包含动态维度
};

// ---- 版本指纹（来自 protobuf 节点/初始化器名扫描）----
struct VersionFingerprint {
    bool has_one2one = false;      // one2one_cv2/cv3 初始化器 -> YOLO26
    bool has_A2C2f = false;        // A2C2f/AAttn 节点名 -> YOLO12
    bool has_C2PSA = false;        // C2PSA 节点名 -> YOLO11
    bool has_C2f = false;          // C2f 节点名 -> YOLOv8
    bool has_C3 = false;           // C3 节点名 -> YOLOv5/5u
    bool has_anchor_grid = false;  // anchor_grid -> YOLOv5 原版
    bool has_dfl = false;          // dfl.conv.weight -> YOLOv5u/v8+
    bool has_cv2_cv3 = false;      // head 含 cv2/cv3

    std::vector<std::string> node_names;          // 全部节点名 + 算子类型
    std::vector<std::string> initializer_names;   // 全部初始化器名
    int first_conv_cout = -1;                     // 首层 Conv 输出通道数（宽度指纹），-1=未提取到
    std::string first_conv_name;                  // 首层卷积权重名称
};

// ---- YOLO 官方参数量基准表（Detect/80类/640x640/fused）----
struct YoloRefEntry {
    const char* version;   // "YOLOv5" / "YOLOv5u" / "YOLOv8" / "YOLO11" / "YOLO12" / "YOLO26"
    const char* scale;     // "n" / "s" / "m" / "l" / "x"
    double params_m;       // 官方公布值（M）
    int64_t params_exact;  // 精确值（个），0 表示无精确值
    const char* note;
};

constexpr YoloRefEntry kYoloRefTable[] = {
    {"YOLOv5",  "n", 1.9, 0,        "anchor-based v7.0"},
    {"YOLOv5",  "s", 7.2, 0,        "anchor-based v7.0"},
    {"YOLOv5",  "m", 21.2, 0,       "anchor-based v7.0"},
    {"YOLOv5",  "l", 46.5, 0,       "anchor-based v7.0"},
    {"YOLOv5",  "x", 86.7, 0,       "anchor-based v7.0"},
    {"YOLOv5u", "n", 2.6, 2654816,  "anchor-free"},
    {"YOLOv5u", "s", 9.1, 9153152,  "anchor-free"},
    {"YOLOv5u", "m", 25.1, 25111456,"anchor-free"},
    {"YOLOv5u", "l", 53.2, 53225024,"anchor-free"},
    {"YOLOv5u", "x", 97.2, 97276448,"anchor-free"},
    {"YOLOv8",  "n", 3.2, 3157200,  ""},
    {"YOLOv8",  "s", 11.2, 11166560,""},
    {"YOLOv8",  "m", 25.9, 25902640,""},
    {"YOLOv8",  "l", 43.7, 43691520,""},
    {"YOLOv8",  "x", 68.2, 68229648,""},
    {"YOLO11",  "n", 2.6, 2624080,  ""},
    {"YOLO11",  "s", 9.4, 9458752,  ""},
    {"YOLO11",  "m", 20.1, 20114688,""},
    {"YOLO11",  "l", 25.3, 25372160,""},
    {"YOLO11",  "x", 56.9, 56966176,""},
    {"YOLO12",  "n", 2.6, 2602288,  ""},
    {"YOLO12",  "s", 9.3, 9284096,  ""},
    {"YOLO12",  "m", 20.2, 20199168,""},
    {"YOLO12",  "l", 26.4, 26450784,""},
    {"YOLO12",  "x", 59.1, 59210784,""},
    {"YOLO26",  "n", 2.4, 2417452,  "end2end=False fused"},
    {"YOLO26",  "s", 9.5, 9512892,  "end2end=False fused"},
    {"YOLO26",  "m", 20.4, 20435260,"end2end=False fused"},
    {"YOLO26",  "l", 24.8, 24838716,"end2end=False fused"},
    {"YOLO26",  "x", 55.7, 55772892,"end2end=False fused"},
};
constexpr size_t kYoloRefCount = sizeof(kYoloRefTable) / sizeof(kYoloRefTable[0]);

// 宽度指纹映射：首层 Conv 输出通道数 -> 规模字母（C_out = 64 * 宽度倍数 0.25/0.50/0.75/1.00/1.25）
constexpr int kWidthFingerprints[]   = {16, 32, 48, 64, 80};
constexpr const char* kScaleLetters[] = {"n", "s", "m", "l", "x"};
constexpr size_t kScaleCount = 5;

// ---- 参数量容差匹配结果 ----
struct ParamsMatch {
    bool matched = false;               // 是否命中
    std::string ref_version;           // 命中的基准表版本
    std::string ref_scale;             // 命中的基准表规模
    int64_t params_reference = 0;      // 基准值
    double deviation_pct = 0.0;        // 偏差百分比 |实测-基准|/基准

    struct Candidate {
        std::string version;
        std::string scale;
        int64_t params;
        double dev;
    };
    std::vector<Candidate> candidates;
};

// YOLO 模型分析结果
struct YoloInfo {
    bool is_yolo = false;     // 是否识别为 YOLO 系列模型
    std::string task;         // detect / segment / classify / pose / obb
    int classes = -1;         // 类别数量
    int width = -1;           // 输入宽度
    int height = -1;          // 输入高度
    std::string scale;        // 明确规模 n/s/m/l/x
    std::string version;      // YOLO26 / YOLOv8 等，或 "Unknown"
    bool scale_estimated = false;   // true=由参数量粗略估算（数据不足）
    int first_conv_cout = -1;       // 首层 Conv 输出通道数（宽度指纹）
    ParamsMatch params_match;       // 参数量容差匹配结果
};

// 完整模型报告（聚合所有读取器的产出）
struct ModelReport {
    // 文件信息
    std::string file_path;
    std::string file_size;    // 人类可读大小，如 "12.6 MB"
    std::string file_mtime;   // 修改时间

    // ONNX Runtime Metadata
    std::string producer;
    std::string producer_version_proto;  // 仅来自 protobuf（可选）
    std::string domain;
    std::string graph_name;
    std::string graph_description;
    int64_t model_version = 0;
    int64_t ir_version = 0;

    // opset（来自 protobuf）
    std::vector<std::pair<std::string, int64_t>> opset_import;  // (domain, version)

    // 自定义 metadata（来自 ONNX Runtime）
    std::map<std::string, std::string> custom_metadata;

    // 输入/输出
    std::vector<IoInfo> inputs;
    std::vector<IoInfo> outputs;

    // 参数统计（来自 protobuf initializer）
    long long parameters = 0;
    int tensor_count = 0;  // 权重张量数量（initializer + Constant 节点属性张量）

    // 符号维度 fallback（来自 protobuf，key=张量名）
    std::map<std::string, std::vector<std::string>> input_symbolic;
    std::map<std::string, std::vector<std::string>> output_symbolic;

    // 版本指纹（来自 protobuf 节点/初始化器名扫描）
    VersionFingerprint fingerprint;

    // YOLO 分析
    YoloInfo yolo;

    // 是否为动态模型
    bool is_dynamic = false;

    // 导出方法（如 "Ultralytics model.export()"）
    std::string export_method;

    // 模型导出时间（优先使用 metadata "date"，其次文件修改时间）
    std::string export_time;
};
