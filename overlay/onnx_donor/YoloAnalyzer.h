#pragma once

#include "Common.h"

// YOLO 模型自动分析：
//  - task（detect/segment/classify/pose/obb）
//  - 类别数量（metadata 优先，其次由 output shape 推断）
//  - 输入尺寸（input shape 优先，其次 metadata imgsz，动态则标记）
//  - 模型规模 n/s/m/l/x（版本字符串优先，否则由参数量估计）
//  - 版本识别（YOLO26 / YOLOv8 ...，否则 unknown）
class YoloAnalyzer {
public:
    static void Analyze(ModelReport& report);
};
