#pragma once

#include <memory>
#include <string>
#include <onnxruntime_cxx_api.h>

#include "Common.h"

// 基于 ONNX Runtime C++ API 读取会话信息：
// 输入/输出的名称、shape、动态维度、Tensor 元素类型。
class OnnxRuntimeReader {
public:
    // 加载模型并填充 report.inputs / report.outputs / report.is_dynamic
    // 成功返回 true；失败抛出异常（由调用方捕获）
    bool Read(const std::wstring& model_path, ModelReport& report);

    // 暴露已建立的会话，供 MetadataReader 复用
    Ort::Session* GetSession() { return session_ ? session_.get() : nullptr; }

private:
    static std::string ElementTypeToString(ONNXTensorElementDataType type);
    void ReadIo(Ort::Session& session, bool is_input, ModelReport& report);

    Ort::Env env_{nullptr};
    std::unique_ptr<Ort::Session> session_;
};
