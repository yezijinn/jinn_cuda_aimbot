#pragma once

#include <onnxruntime_cxx_api.h>

#include "Common.h"

// 通过 Ort::Session::GetModelMetadata() 读取模型元信息：
// producer、graph name、domain、description 及自定义 metadata（names/stride/task/date 等）
class MetadataReader {
public:
    bool Read(Ort::Session& session, ModelReport& report);
};
