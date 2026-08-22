#pragma once

#include <string>

#include "Common.h"

// 自包含的 ONNX protobuf 线格式解析器。
//
// ONNX Runtime C++ API 无法直接读取 graph.initializer / 参数量 / opset / 符号维度，
// 而预编译包未附带 onnx 的 .proto 生成代码，也不依赖 protoc/libprotobuf。
// 本解析器仅实现 ONNX 模型所需的最小 protobuf 子集（变长整型、嵌套消息、长度分隔字段），
// 足以提取：opset_import、initializer 参数量、以及输入/输出的符号维度名。
class OnnxProtoParser {
public:
    // 解析 model_path 指向的 .onnx 文件，填充 report 的：
    //   opset_import / parameters / initializer_count / input_symbolic / output_symbolic / ir_version / model_version
    // 使用宽字符路径（_wfopen）以支持中文路径；失败（文件损坏/非 protobuf）时返回 false。
    static bool Parse(const std::wstring& model_path, ModelReport& report);
};
