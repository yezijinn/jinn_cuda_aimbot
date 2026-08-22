#include "OnnxProtoParser.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

// ---- 极简 protobuf 线格式读取器 ----
// 仅支持本项目所需字段类型：
//   wire type 0 = varint, 1 = 64-bit, 2 = 长度分隔, 5 = 32-bit
struct Cursor {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    uint64_t varint = 0;  // 仅 wire type 0 时有效
};

bool ReadVarint(Cursor& c, uint64_t& out) {
    out = 0;
    int shift = 0;
    while (c.p < c.end) {
        uint8_t b = *c.p++;
        out |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
        if (shift >= 64) return false;  // 防溢出
    }
    return false;
}

// 读取下一个字段标签，并将 payload 指向字段数据。返回 false 表示已到消息末尾。
bool NextField(Cursor& c, uint32_t& field_num, uint32_t& wire_type, Cursor& payload) {
    if (c.p >= c.end) return false;
    uint64_t tag = 0;
    if (!ReadVarint(c, tag)) return false;
    field_num = static_cast<uint32_t>(tag >> 3);
    wire_type = static_cast<uint32_t>(tag & 0x7);
    payload.p = c.p;
    payload.end = c.end;
    payload.varint = 0;

    switch (wire_type) {
        case 0: {  // varint
            uint64_t v = 0;
            if (!ReadVarint(c, v)) return false;
            payload.varint = v;
            payload.end = c.p;
            break;
        }
        case 1:  // 64-bit
            if (c.p + 8 > c.end) return false;
            c.p += 8;
            payload.end = c.p;
            break;
        case 2: {  // 长度分隔
            uint64_t len = 0;
            if (!ReadVarint(c, len)) return false;
            if (len > static_cast<uint64_t>(c.end - c.p)) return false;
            payload.p = c.p;
            payload.end = c.p + static_cast<size_t>(len);
            c.p = payload.end;
            break;
        }
        case 5:  // 32-bit
            if (c.p + 4 > c.end) return false;
            c.p += 4;
            payload.end = c.p;
            break;
        default:
            return false;  // 不支持的 wire type
    }
    return true;
}

std::string ReadString(const Cursor& pl) {
    if (pl.p >= pl.end) return std::string();
    return std::string(reinterpret_cast<const char*>(pl.p),
                       static_cast<size_t>(pl.end - pl.p));
}

// ---- ONNX proto 字段号（依据 onnx 官方 .proto 并已实测确认）----
// ModelProto（现代 schema，IR >= 4）:
//   ir_version=1, model_version=5, opset_import=8, producer_name=2,
//   graph=7, metadata_props=14
// ModelProto（旧版 schema，IR < 4）:
//   ir_version=1, model_version=10, opset_import=2, graph=14, metadata_props=15
constexpr uint32_t F_MODEL_IR_VERSION = 1;

// 现代 schema（IR >= 4）：经实测确认
constexpr uint32_t F_MODEL_GRAPH_MODERN = 7;
constexpr uint32_t F_MODEL_OPSET_IMPORT_MODERN = 8;
constexpr uint32_t F_MODEL_METADATA_MODERN = 14;
constexpr uint32_t F_MODEL_MODEL_VERSION_MODERN = 5;

// 旧版 schema（IR < 4）
constexpr uint32_t F_MODEL_OPSET_IMPORT_LEGACY = 2;
constexpr uint32_t F_MODEL_GRAPH_LEGACY = 14;
constexpr uint32_t F_MODEL_METADATA_LEGACY = 15;
constexpr uint32_t F_MODEL_MODEL_VERSION_LEGACY = 10;

// GraphProto 字段号（依 onnx 官方 .proto，已实测确认）
//  - 现代 schema：initializer=5, input=11, output=12, value_info=13
//  - 旧版 schema：input=5, output=6, initializer=7
constexpr uint32_t F_GRAPH_NAME = 2;
constexpr uint32_t F_GRAPH_NODE = 1;
constexpr uint32_t F_GRAPH_INITIALIZER = 5;
constexpr uint32_t F_GRAPH_INPUT = 11;
constexpr uint32_t F_GRAPH_OUTPUT = 12;
constexpr uint32_t F_GRAPH_INITIALIZER_LEGACY = 7;
constexpr uint32_t F_GRAPH_INPUT_LEGACY = 5;
constexpr uint32_t F_GRAPH_OUTPUT_LEGACY = 6;

// NodeProto
constexpr uint32_t F_NODE_NAME = 1;       // 节点名（版本指纹用）
constexpr uint32_t F_NODE_OP_TYPE = 2;     // 算子类型（Conv/C2PSA 等，版本指纹用）
constexpr uint32_t F_NODE_ATTRIBUTE = 5;

// AttributeProto
constexpr uint32_t F_ATTR_TENSOR = 21;        // t (TensorProto)
constexpr uint32_t F_ATTR_TENSORS = 22;       // repeated tensors

// ValueInfoProto
constexpr uint32_t F_VALUEINFO_NAME = 1;
constexpr uint32_t F_VALUEINFO_TYPE = 2;

// TypeProto
constexpr uint32_t F_TYPE_TENSOR = 1;

// TypeProto.Tensor
constexpr uint32_t F_TENSORTYPE_ELEM_TYPE = 1;
constexpr uint32_t F_TENSORTYPE_SHAPE = 2;

// TensorShapeProto / Dimension
constexpr uint32_t F_SHAPE_DIM = 1;
constexpr uint32_t F_DIM_VALUE = 1;
constexpr uint32_t F_DIM_PARAM = 2;

// TensorProto
constexpr uint32_t F_TENSOR_NAME = 8;     // 初始化器名称（首层 Conv 宽度指纹用）
constexpr uint32_t F_TENSOR_DIMS = 1;

// OperatorSetIdProto
constexpr uint32_t F_OPSET_DOMAIN = 1;
constexpr uint32_t F_OPSET_VERSION = 2;

// StringStringEntryProto
constexpr uint32_t F_ENTRY_KEY = 1;
constexpr uint32_t F_ENTRY_VALUE = 2;

// ---- 各消息解析 ----

void ParseDimension(Cursor buf, std::vector<std::string>& dims) {
    Cursor c = buf;
    uint32_t fn = 0, wt = 0;
    Cursor pl{};
    while (NextField(c, fn, wt, pl)) {
        if (wt == 0 && fn == F_DIM_VALUE) {
            dims.push_back(std::to_string(static_cast<int64_t>(pl.varint)));
        } else if (wt == 2 && fn == F_DIM_PARAM) {
            dims.push_back(ReadString(pl));
        }
    }
}

void ParseShape(Cursor buf, std::vector<std::string>& dims) {
    Cursor c = buf;
    uint32_t fn = 0, wt = 0;
    Cursor pl{};
    while (NextField(c, fn, wt, pl)) {
        if (wt == 2 && fn == F_SHAPE_DIM) {
            ParseDimension(pl, dims);
        }
    }
}

void ParseTensorType(Cursor buf, std::vector<std::string>& dims) {
    Cursor c = buf;
    uint32_t fn = 0, wt = 0;
    Cursor pl{};
    while (NextField(c, fn, wt, pl)) {
        if (wt == 2 && fn == F_TENSORTYPE_SHAPE) {
            ParseShape(pl, dims);
        }
        // F_TENSORTYPE_ELEM_TYPE 忽略（类型由 ONNX Runtime 提供）
    }
}

void ParseType(Cursor buf, std::vector<std::string>& dims) {
    Cursor c = buf;
    uint32_t fn = 0, wt = 0;
    Cursor pl{};
    while (NextField(c, fn, wt, pl)) {
        if (wt == 2 && fn == F_TYPE_TENSOR) {
            ParseTensorType(pl, dims);
        }
    }
}

void ParseValueInfo(Cursor buf, std::map<std::string, std::vector<std::string>>& sym) {
    std::string name;
    std::vector<std::string> dims;
    Cursor c = buf;
    uint32_t fn = 0, wt = 0;
    Cursor pl{};
    while (NextField(c, fn, wt, pl)) {
        if (wt == 2 && fn == F_VALUEINFO_NAME) {
            name = ReadString(pl);
        } else if (wt == 2 && fn == F_VALUEINFO_TYPE) {
            ParseType(pl, dims);
        }
    }
    if (!name.empty()) sym[name] = std::move(dims);
}

void ParseTensor(Cursor buf, ModelReport& report);  // 前向声明

void ParseAttribute(Cursor buf, ModelReport& report) {
    Cursor c = buf;
    uint32_t fn = 0, wt = 0;
    Cursor pl{};
    while (NextField(c, fn, wt, pl)) {
        if (wt == 2 && (fn == F_ATTR_TENSOR || fn == F_ATTR_TENSORS)) {
            ParseTensor(pl, report);
        }
    }
}

void ParseNode(Cursor buf, ModelReport& report) {
    Cursor c = buf;
    uint32_t fn = 0, wt = 0;
    Cursor pl{};
    std::string node_name;
    std::string op_type;
    while (NextField(c, fn, wt, pl)) {
        if (wt == 2 && fn == F_NODE_NAME) {
            node_name = ReadString(pl);
        } else if (wt == 2 && fn == F_NODE_OP_TYPE) {
            op_type = ReadString(pl);
        } else if (wt == 2 && fn == F_NODE_ATTRIBUTE) {
            ParseAttribute(pl, report);
        }
    }
    // 收集节点名与算子类型，供版本指纹（one2one/A2C2f/C2PSA/C2f/C3 等）判断
    if (!node_name.empty()) {
        report.fingerprint.node_names.push_back(node_name);
    }
    if (!op_type.empty()) {
        report.fingerprint.node_names.push_back(op_type);
    }
}

void ParseTensor(Cursor buf, ModelReport& report) {
    Cursor c = buf;
    uint32_t fn = 0, wt = 0;
    Cursor pl{};
    long long elems = 1;
    bool has_dims = false;
    std::string name;
    std::vector<int64_t> dims;
    while (NextField(c, fn, wt, pl)) {
        if (wt == 0 && fn == F_TENSOR_DIMS) {
            const int64_t d = static_cast<int64_t>(pl.varint);
            elems *= d;
            has_dims = true;
            dims.push_back(d);
        } else if (wt == 2 && fn == F_TENSOR_NAME) {
            name = ReadString(pl);
        }
        // F_TENSOR_DATA_TYPE / raw_data 等忽略
    }
    if (has_dims) {
        report.parameters += elems;
    } else {
        report.parameters += 1;  // 标量 initializer 计 1 个参数
    }
    report.tensor_count++;

    // 收集初始化器名，供版本指纹与首层 Conv 宽度指纹使用
    if (!name.empty()) {
        report.fingerprint.initializer_names.push_back(name);
        // 首层 Conv 输出通道数（宽度指纹）：名字含 "conv.weight"、4 维、输入通道=3(RGB)
        // 常见路径：model.0.conv.weight, /model/0/conv/weight
        if (report.fingerprint.first_conv_cout < 0 &&
            dims.size() == 4 &&
            name.find("conv.weight") != std::string::npos &&
            dims[1] == 3) {
            report.fingerprint.first_conv_cout = static_cast<int>(dims[0]);
            report.fingerprint.first_conv_name = name;
        }
    }
}

void ParseOpSetId(Cursor buf, ModelReport& report) {
    std::string domain;
    int64_t version = 0;
    Cursor c = buf;
    uint32_t fn = 0, wt = 0;
    Cursor pl{};
    while (NextField(c, fn, wt, pl)) {
        if (wt == 2 && fn == F_OPSET_DOMAIN) {
            domain = ReadString(pl);
        } else if (wt == 0 && fn == F_OPSET_VERSION) {
            version = static_cast<int64_t>(pl.varint);
        }
    }
    report.opset_import.emplace_back(domain, version);
}

void ParseGraph(Cursor buf, ModelReport& report) {
    // 检测 GraphProto schema：检测 input/output 字段号（11/12=现代，5/6=旧版）
    // 对于 IR=0 的 Torch JIT 模型，ModelProto 和 GraphProto 可能使用不同 schema，
    // 因此同时检查 initializer 字段号（5=现代，7=旧版）以确定初始 schema。
    bool modern = false;
    {
        Cursor c = buf;
        uint32_t fn = 0, wt = 0;
        Cursor pl{};
        while (NextField(c, fn, wt, pl)) {
            if (fn == F_GRAPH_INPUT || fn == F_GRAPH_OUTPUT) { modern = true; break; }
            if (fn == F_GRAPH_INPUT_LEGACY || fn == F_GRAPH_OUTPUT_LEGACY) break;  // 确定为 legacy
        }
    }

    // 执行一次解析的辅助函数
    auto try_parse_graph = [](Cursor buf, bool use_modern, ModelReport& rpt) -> int {
        const uint32_t f_in = use_modern ? F_GRAPH_INPUT : F_GRAPH_INPUT_LEGACY;
        const uint32_t f_out = use_modern ? F_GRAPH_OUTPUT : F_GRAPH_OUTPUT_LEGACY;
        const uint32_t f_init = use_modern ? F_GRAPH_INITIALIZER : F_GRAPH_INITIALIZER_LEGACY;
        int n_init = 0;  // 本次解析到的 initializer 数量
        Cursor c = buf;
        uint32_t fn = 0, wt = 0;
        Cursor pl{};
        while (NextField(c, fn, wt, pl)) {
            if (wt != 2) continue;
            if (fn == F_GRAPH_NAME) {
                if (rpt.graph_name.empty()) rpt.graph_name = ReadString(pl);
            } else if (fn == f_in) {
                ParseValueInfo(pl, rpt.input_symbolic);
            } else if (fn == f_out) {
                ParseValueInfo(pl, rpt.output_symbolic);
            } else if (fn == f_init) {
                ParseTensor(pl, rpt);
                ++n_init;
            } else if (fn == F_GRAPH_NODE) {
                ParseNode(pl, rpt);
            }
        }
        return n_init;
    };

    int old_tc = report.tensor_count;
    int init_count = try_parse_graph(buf, modern, report);

    // 若未找到 initializer，尝试另一种 schema（Torch JIT IR=0 常见混合 schema）
    if (init_count == 0) {
        report.tensor_count = old_tc;  // 回退 tensor count（ParseTensor 的累加）
        try_parse_graph(buf, !modern, report);
    }
}

void ParseModel(Cursor buf, ModelReport& report) {
    // 第一遍：读取 ir_version
    {
        Cursor c = buf;
        uint32_t fn = 0, wt = 0;
        Cursor pl{};
        while (NextField(c, fn, wt, pl)) {
            if (wt == 0 && fn == F_MODEL_IR_VERSION) {
                report.ir_version = static_cast<int64_t>(pl.varint);
                break;
            }
        }
    }

    // ir_version: >= 4 现代, 1-3 旧版, 0 未设置（尝试 auto-detect）
    bool use_modern = report.ir_version >= 4;
    if (report.ir_version == 0) use_modern = true;     // 先按 modern 尝试
    if (report.ir_version <= 3 && report.ir_version >= 1) use_modern = false; // 确定为 legacy

    // 辅助：按指定 schema 遍历（只扫描，不清空已有数据以便对比）
    auto try_parse = [](Cursor buf, bool modern, ModelReport& rpt) -> bool {
        const uint32_t f_opset = modern ? F_MODEL_OPSET_IMPORT_MODERN : F_MODEL_OPSET_IMPORT_LEGACY;
        const uint32_t f_graph = modern ? F_MODEL_GRAPH_MODERN : F_MODEL_GRAPH_LEGACY;
        const uint32_t f_meta  = modern ? F_MODEL_METADATA_MODERN : F_MODEL_METADATA_LEGACY;
        const uint32_t f_mver  = modern ? F_MODEL_MODEL_VERSION_MODERN : F_MODEL_MODEL_VERSION_LEGACY;
        (void)f_meta;

        Cursor c = buf;
        uint32_t fn = 0, wt = 0;
        Cursor pl{};
        bool found_opset = false, found_graph = false;
        while (NextField(c, fn, wt, pl)) {
            if (wt == 0) {
                if (fn == F_MODEL_IR_VERSION) {
                    rpt.ir_version = static_cast<int64_t>(pl.varint);
                } else if (fn == f_mver) {
                    rpt.model_version = static_cast<int64_t>(pl.varint);
                }
            } else if (wt == 2) {
                if (fn == f_opset) { ParseOpSetId(pl, rpt); found_opset = true; }
                else if (fn == f_graph) { ParseGraph(pl, rpt); found_graph = true; }
            }
        }
        return found_opset || found_graph;
    };

    // 备份 initializer 计数以便回退
    auto old_params = report.parameters;
    auto old_tensor_count = report.tensor_count;
    auto old_opset = report.opset_import;
    auto old_mver = report.model_version;
    auto old_ir = report.ir_version;
    auto old_input_sym = report.input_symbolic;
    auto old_output_sym = report.output_symbolic;
    auto old_graph_name = report.graph_name;

    if (!try_parse(buf, use_modern, report)) {
        // schema 不匹配：清空误解析数据，换另一套 schema 重试
        report.parameters = old_params;
        report.tensor_count = static_cast<int>(old_tensor_count);
        report.opset_import = std::move(old_opset);
        report.model_version = old_mver;
        report.ir_version = old_ir;
        report.input_symbolic = std::move(old_input_sym);
        report.output_symbolic = std::move(old_output_sym);
        report.graph_name = std::move(old_graph_name);
        try_parse(buf, !use_modern, report);
    }

    // 暴力兜底：若仍未找到 initializer（IR=0 Torch JIT 非标准布局），
    // 遍历所有 length-delimited 字段，逐个尝试解析为 GraphProto。
    // 判据：解析后 tensor_count 增加 → 该字段就是 graph。
    if (report.tensor_count == 0 && report.parameters == 0) {
        Cursor c = buf;
        uint32_t fn = 0, wt = 0;
        Cursor pl{};
        while (NextField(c, fn, wt, pl)) {
            if (wt != 2) continue;
            // 跳过已知非 graph 的字段（opset / metadata 各 schema 下的字段号）
            if (fn == F_MODEL_OPSET_IMPORT_MODERN || fn == F_MODEL_OPSET_IMPORT_LEGACY ||
                fn == F_MODEL_METADATA_MODERN || fn == F_MODEL_METADATA_LEGACY) continue;
            int tc_before = report.tensor_count;
            ParseGraph(pl, report);
            if (report.tensor_count > tc_before) break;  // 找到了
            // 没找到，回退 tensor_count 防止误解析
            report.tensor_count = tc_before;
        }
    }
}

}  // namespace

bool OnnxProtoParser::Parse(const std::wstring& model_path, ModelReport& report) {
    // 用宽字符 API 打开，支持中文路径（std::ifstream 窄字符在 Windows 下无法打开中文路径）
    FILE* fp = _wfopen(model_path.c_str(), L"rb");
    if (!fp) return false;

    std::vector<uint8_t> bytes;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz > 0) {
        bytes.resize(static_cast<size_t>(sz));
        if (fread(bytes.data(), 1, static_cast<size_t>(sz), fp) != static_cast<size_t>(sz)) {
            fclose(fp);
            return false;
        }
    }
    fclose(fp);

    if (bytes.size() < 4) return false;

    Cursor root{bytes.data(), bytes.data() + bytes.size(), 0};
    ParseModel(root, report);
    return true;
}
