#pragma once

#include <ostream>
#include <string>

#include "Common.h"

// 将模型报告序列化为 JSON 并写入给定输出流。
class JsonWriter {
public:
    static void Write(const ModelReport& report, std::ostream& os);
};
