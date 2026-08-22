#include "JsonWriter.h"

#include <cstdio>   // std::snprintf：下方 JsonEscape 转义控制字符时使用。
                    // 原先仅靠 <sstream> 的传递包含侥幸编译，标准并不保证 <sstream>
                    // 会声明 std::snprintf，更换标准库实现或升级 MSVC 后会编译失败。
#include <sstream>
#include <string>

namespace {

std::string JsonEscape(const std::string& s) {
    std::ostringstream os;
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
    return os.str();
}

std::string JsonStr(const std::string& s) {
    return "\"" + JsonEscape(s) + "\"";
}

std::string ShapeToJson(const std::vector<int64_t>& shape) {
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i) os << ",";
        os << shape[i];
    }
    os << "]";
    return os.str();
}

void WriteIoArray(std::ostringstream& os, const std::vector<IoInfo>& ios) {
    os << "[";
    for (size_t i = 0; i < ios.size(); ++i) {
        if (i) os << ",";
        const IoInfo& io = ios[i];
        os << "{";
        os << JsonStr("name") << ":" << JsonStr(io.name) << ",";
        os << JsonStr("shape") << ":" << ShapeToJson(io.shape) << ",";
        os << JsonStr("type") << ":" << JsonStr(io.type) << ",";
        os << JsonStr("dynamic") << ":" << (io.dynamic ? "true" : "false");
        os << "}";
    }
    os << "]";
}

}  // namespace

void JsonWriter::Write(const ModelReport& r, std::ostream& os) {
    std::ostringstream out;
    out << "{\n";

    out << "  " << JsonStr("file") << ":" << JsonStr(r.file_path) << ",\n";
    out << "  " << JsonStr("size") << ":" << JsonStr(r.file_size) << ",\n";
    out << "  " << JsonStr("modified") << ":" << JsonStr(r.file_mtime) << ",\n";
    out << "  " << JsonStr("producer") << ":" << JsonStr(r.producer) << ",\n";
    out << "  " << JsonStr("domain") << ":" << JsonStr(r.domain) << ",\n";
    out << "  " << JsonStr("graph_name") << ":" << JsonStr(r.graph_name) << ",\n";
    out << "  " << JsonStr("model_version") << ":" << r.model_version << ",\n";
    out << "  " << JsonStr("ir_version") << ":" << r.ir_version << ",\n";

    // opset
    out << "  " << JsonStr("opset") << ":[";
    for (size_t i = 0; i < r.opset_import.size(); ++i) {
        if (i) out << ",";
        out << "{" << JsonStr("domain") << ":" << JsonStr(r.opset_import[i].first)
            << "," << JsonStr("version") << ":" << r.opset_import[i].second << "}";
    }
    out << "],\n";

    // metadata (custom)
    out << "  " << JsonStr("metadata") << ":{";
    bool first = true;
    for (const auto& kv : r.custom_metadata) {
        if (!first) out << ",";
        out << JsonStr(kv.first) << ":" << JsonStr(kv.second);
        first = false;
    }
    out << "},\n";

    out << "  " << JsonStr("inputs") << ":";
    WriteIoArray(out, r.inputs);
    out << ",\n";

    out << "  " << JsonStr("outputs") << ":";
    WriteIoArray(out, r.outputs);
    out << ",\n";

    out << "  " << JsonStr("parameters") << ":" << r.parameters << ",\n";
    out << "  " << JsonStr("tensor_count") << ":" << r.tensor_count << ",\n";
    out << "  " << JsonStr("dynamic") << ":" << (r.is_dynamic ? "true" : "false") << ",\n";
    out << "  " << JsonStr("export_method") << ":" << JsonStr(r.export_method) << ",\n";
    out << "  " << JsonStr("export_time") << ":" << JsonStr(r.export_time) << ",\n";

    // yolo
    const YoloInfo& y = r.yolo;
    out << "  " << JsonStr("yolo") << ":{";
    out << JsonStr("is_yolo") << ":" << (y.is_yolo ? "true" : "false") << ",";
    out << JsonStr("task") << ":" << JsonStr(y.task) << ",";
    out << JsonStr("classes") << ":" << y.classes << ",";
    out << JsonStr("input_size") << ":{";
    out << JsonStr("width") << ":" << y.width << "," << JsonStr("height") << ":" << y.height;
    out << "},";
    out << JsonStr("scale") << ":" << JsonStr(y.scale) << ",";
    out << JsonStr("scale_estimated") << ":" << (y.scale_estimated ? "true" : "false") << ",";
    out << JsonStr("version") << ":" << JsonStr(y.version);
    out << "}\n";

    out << "}\n";
    os << out.str();
}
