#include "OnnxRuntimeReader.h"

#include <stdexcept>

namespace {

// ONNX 元素类型 -> 显示名
std::string MapElementType(ONNXTensorElementDataType type) {
    switch (type) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:   return "float32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return "float16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:return "bfloat16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:  return "float64";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:    return "int8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:   return "int16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:   return "int32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:   return "int64";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:   return "uint8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:  return "uint16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:  return "uint32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:  return "uint64";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:    return "bool";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING:  return "string";
        default:                                     return "unknown";
    }
}

}  // namespace

std::string OnnxRuntimeReader::ElementTypeToString(ONNXTensorElementDataType type) {
    return MapElementType(type);
}

bool OnnxRuntimeReader::Read(const std::wstring& model_path, ModelReport& report) {
    env_ = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "model_info");
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);
    session_options.SetLogSeverityLevel(ORT_LOGGING_LEVEL_ERROR);

    session_ = std::make_unique<Ort::Session>(env_, model_path.c_str(), session_options);

    ReadIo(*session_, /*is_input=*/true, report);
    ReadIo(*session_, /*is_input=*/false, report);

    for (const auto& in : report.inputs) {
        if (in.dynamic) report.is_dynamic = true;
    }
    for (const auto& out : report.outputs) {
        if (out.dynamic) report.is_dynamic = true;
    }
    return true;
}

void OnnxRuntimeReader::ReadIo(Ort::Session& session, bool is_input, ModelReport& report) {
    const size_t count = is_input ? session.GetInputCount() : session.GetOutputCount();
    std::vector<std::string> names = is_input ? session.GetInputNames() : session.GetOutputNames();

    for (size_t i = 0; i < count; ++i) {
        IoInfo io;
        if (i < names.size()) {
            io.name = names[i];
        } else {
            io.name = (is_input ? "input" : "output") + std::to_string(i);
        }

        Ort::TypeInfo type_info = is_input ? session.GetInputTypeInfo(i) : session.GetOutputTypeInfo(i);
        auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
        io.type = MapElementType(tensor_info.GetElementType());

        io.shape = tensor_info.GetShape();
        std::vector<const char*> sym = tensor_info.GetSymbolicDimensions();
        io.symbolic.resize(io.shape.size());
        for (size_t d = 0; d < io.shape.size(); ++d) {
            if (io.shape[d] < 0) {
                io.dynamic = true;
            }
            if (d < sym.size() && sym[d] != nullptr) {
                io.symbolic[d] = sym[d];
            }
        }

        if (is_input) {
            report.inputs.push_back(std::move(io));
        } else {
            report.outputs.push_back(std::move(io));
        }
    }
}
