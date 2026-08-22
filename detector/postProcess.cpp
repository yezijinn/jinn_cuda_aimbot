#include <algorithm>
#include <numeric>
#include <chrono>
#include <limits>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "postProcess.h"
#include "mybot.h"
#include "config.h"
#ifdef USE_CUDA
#include "trt_detector.h"
#endif

namespace
{
void RunBoundedNms(
    std::vector<Detection>& detections,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime,
    NmsTelemetry* telemetry);

bool TryPositiveInt64ToInt(int64_t value, int* out)
{
    if (!out || value <= 0 ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    *out = static_cast<int>(value);
    return true;
}

bool ExtractRowsCols(const std::vector<int64_t>& shape, int* rows, int* cols)
{
    if (shape.size() < 2)
        return false;

    return TryPositiveInt64ToInt(shape[shape.size() - 2], rows) &&
        TryPositiveInt64ToInt(shape[shape.size() - 1], cols);
}

bool TryResolveClassLayout(
    int extent,
    int numClassesHint,
    bool preferObjectness,
    int* numClasses,
    bool* usesObjectness)
{
    if (!numClasses || !usesObjectness || extent <= 4)
        return false;

    auto tryHint = [&](int hint) -> bool
    {
        if (hint <= 0 || hint > 10000)
            return false;

        if (extent == 5 + hint)
        {
            *numClasses = hint;
            *usesObjectness = true;
            return true;
        }

        if (extent == 4 + hint)
        {
            *numClasses = hint;
            *usesObjectness = false;
            return true;
        }

        return false;
    };

    if (tryHint(numClassesHint))
        return true;

    if (preferObjectness && extent > 5)
    {
        *numClasses = extent - 5;
        *usesObjectness = true;
        return *numClasses > 0;
    }

    *numClasses = extent - 4;
    *usesObjectness = false;
    return *numClasses > 0;
}

bool LooksLikeXyxyDetections(const float* output, int rows, int cols)
{
    if (!output || rows <= 0 || cols != 6)
        return false;

    const int maxSamples = std::min(rows, 256);
    const int step = std::max(1, rows / maxSamples);
    int considered = 0;
    int validXyxy = 0;
    int integerClassId = 0;

    for (int i = 0; i < rows && considered < maxSamples; i += step)
    {
        const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);
        const float x1 = det[0];
        const float y1 = det[1];
        const float x2 = det[2];
        const float y2 = det[3];
        const float confidence = det[4];
        const float classId = det[5];

        if (!std::isfinite(x1) || !std::isfinite(y1) ||
            !std::isfinite(x2) || !std::isfinite(y2) ||
            !std::isfinite(confidence) || !std::isfinite(classId))
        {
            continue;
        }

        ++considered;

        if (x2 >= x1 && y2 >= y1)
            ++validXyxy;

        const float roundedClassId = std::round(classId);
        if (classId >= -1.0f &&
            classId <= 10000.0f &&
            std::fabs(classId - roundedClassId) <= 1e-3f)
        {
            ++integerClassId;
        }
    }

    if (considered == 0)
        return false;

    return validXyxy * 3 >= considered * 2 &&
        integerClassId * 4 >= considered * 3;
}

// 检测框像素坐标的合理上界。
// 作用：① 阻断 static_cast<int>(NaN/Inf) —— 该转换在 C++ 中为未定义行为；
//       ② 阻断超大有限框，避免后续 cv::Rect::area()（返回 int）发生有符号溢出 UB，
//          进而使 NMS 的 IoU 计算得到无意义结果。
// 取值 1e5 远大于任何真实屏幕/检测分辨率，对合法检测结果不产生任何影响。
constexpr float kMaxBoxCoord = 1.0e5f;

// classId 的合法解析上界，与 LooksLikeXyxyDetections 中的判定保持一致。
constexpr float kMaxClassIdValue = 10000.0f;

bool IsCoordinateInRange(float value) noexcept
{
    return std::isfinite(value) && std::fabs(value) <= kMaxBoxCoord;
}

void AddXyxyDetection(
    std::vector<Detection>& detections,
    float x1,
    float y1,
    float x2,
    float y2,
    float confidence,
    int classId,
    float scale,
    float confThreshold)
{
    // 用取反形式书写比较，使 NaN 一律走 return 分支；
    // 对有限输入与原 "confidence <= confThreshold || x2 <= x1 || y2 <= y1" 完全等价。
    if (!std::isfinite(confidence) || !(confidence > confThreshold))
        return;
    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2))
        return;
    if (!(x2 > x1) || !(y2 > y1))
        return;

    const float sx1 = x1 * scale;
    const float sy1 = y1 * scale;
    const float sw = (x2 - x1) * scale;
    const float sh = (y2 - y1) * scale;
    if (!IsCoordinateInRange(sx1) || !IsCoordinateInRange(sy1) ||
        !IsCoordinateInRange(sw) || !IsCoordinateInRange(sh))
    {
        return;
    }

    cv::Rect box;
    box.x = static_cast<int>(sx1);
    box.y = static_cast<int>(sy1);
    box.width = static_cast<int>(sw);
    box.height = static_cast<int>(sh);

    if (box.width <= 0 || box.height <= 0)
        return;

    detections.push_back(Detection{ box, confidence, classId });
}

void AddCxcywhDetection(
    std::vector<Detection>& detections,
    float cx,
    float cy,
    float width,
    float height,
    float confidence,
    int classId,
    float scale,
    float confThreshold)
{
    // 同 AddXyxyDetection：取反比较使 NaN 走 return；对有限输入行为等价。
    if (!std::isfinite(confidence) || !(confidence > confThreshold))
        return;
    if (!std::isfinite(cx) || !std::isfinite(cy) ||
        !std::isfinite(width) || !std::isfinite(height))
        return;
    if (!(width > 0.0f) || !(height > 0.0f))
        return;

    const float halfWidth = 0.5f * width;
    const float halfHeight = 0.5f * height;

    const float sx = (cx - halfWidth) * scale;
    const float sy = (cy - halfHeight) * scale;
    const float sw = width * scale;
    const float sh = height * scale;
    if (!IsCoordinateInRange(sx) || !IsCoordinateInRange(sy) ||
        !IsCoordinateInRange(sw) || !IsCoordinateInRange(sh))
    {
        return;
    }

    cv::Rect box;
    box.x = static_cast<int>(sx);
    box.y = static_cast<int>(sy);
    box.width = static_cast<int>(sw);
    box.height = static_cast<int>(sh);

    if (box.width <= 0 || box.height <= 0)
        return;

    detections.push_back(Detection{ box, confidence, classId });
}

void DecodeXyxyDetections(
    const float* output,
    int rows,
    int cols,
    float confThreshold,
    float scale,
    std::vector<Detection>& detections)
{
    detections.reserve(detections.size() + static_cast<size_t>(rows));

    for (int i = 0; i < rows; ++i)
    {
        const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);

        // LooksLikeXyxyDetections 只对最多 256 行做抽样判定，
        // 未被抽样的行仍可能含 NaN/Inf（FP16 溢出常见）；
        // static_cast<int>(NaN) 在 C++ 中为未定义行为，必须先行拦截。
        const float rawClassId = det[5];
        if (!std::isfinite(rawClassId))
            continue;

        // 保留负 classId（部分导出会用 -1 占位），交由下游 isClassEnabled() 统一过滤，
        // 以维持与原实现完全一致的 NMS 输入集合；此处仅拦截无法安全转换的取值。
        const float roundedClassId = std::round(rawClassId);
        if (!(roundedClassId >= -kMaxClassIdValue) || !(roundedClassId <= kMaxClassIdValue))
            continue;

        AddXyxyDetection(
            detections,
            det[0],
            det[1],
            det[2],
            det[3],
            det[4],
            static_cast<int>(roundedClassId),
            scale,
            confThreshold);
    }
}

void DecodeFeatureMajorPredictions(
    const float* output,
    int rows,
    int cols,
    int numClasses,
    bool usesObjectness,
    float confThreshold,
    float scale,
    std::vector<Detection>& detections)
{
    const int classBase = usesObjectness ? 5 : 4;
    if (!output || rows < classBase + numClasses || cols <= 0 || numClasses <= 0)
        return;

    detections.reserve(detections.size() + 256);

    // 【性能】原实现内层按类别遍历 output[(classBase + c) * cols + i]，
    // 访存步长为 cols（YOLOv8 典型 8400，即 33KB），几乎每次读取都跨越缓存行，
    // 在 cols × numClasses 规模下形成密集 cache miss。
    // 这里改为「类别外层、锚点内层」的两趟顺序扫描：所有访存均为连续地址。
    // 类别取胜比较仍是升序扫描 + 严格大于，最大值与并列时的 id 选择结果与原实现逐位一致。
    // 中间缓冲使用 thread_local 复用，稳态下每帧零堆分配。
    thread_local std::vector<float> bestClassScore;
    thread_local std::vector<int> bestClassId;
    const size_t anchorCount = static_cast<size_t>(cols);
    bestClassScore.assign(anchorCount, 0.0f);
    bestClassId.assign(anchorCount, 0);

    for (int c = 0; c < numClasses; ++c)
    {
        const float* scoreRow =
            output + static_cast<size_t>(classBase + c) * anchorCount;
        for (size_t i = 0; i < anchorCount; ++i)
        {
            const float score = scoreRow[i];
            if (score > bestClassScore[i])
            {
                bestClassScore[i] = score;
                bestClassId[i] = c;
            }
        }
    }

    const float* cxRow = output;
    const float* cyRow = output + anchorCount;
    const float* wRow = output + 2 * anchorCount;
    const float* hRow = output + 3 * anchorCount;
    const float* objRow = usesObjectness ? (output + 4 * anchorCount) : nullptr;

    for (size_t i = 0; i < anchorCount; ++i)
    {
        const float objectness = objRow ? objRow[i] : 1.0f;
        AddCxcywhDetection(
            detections,
            cxRow[i],
            cyRow[i],
            wRow[i],
            hRow[i],
            objectness * bestClassScore[i],
            bestClassId[i],
            scale,
            confThreshold);
    }
}

void DecodePredictionMajorPredictions(
    const float* output,
    int rows,
    int cols,
    int numClasses,
    bool usesObjectness,
    float confThreshold,
    float scale,
    std::vector<Detection>& detections)
{
    const int classBase = usesObjectness ? 5 : 4;
    if (!output || cols < classBase + numClasses || rows <= 0 || numClasses <= 0)
        return;

    detections.reserve(detections.size() + 256);

    for (int i = 0; i < rows; ++i)
    {
        const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);
        const float objectness = usesObjectness ? det[4] : 1.0f;

        float maxClassScore = 0.0f;
        int maxClassId = 0;
        for (int c = 0; c < numClasses; ++c)
        {
            const float score = det[classBase + c];
            if (score > maxClassScore)
            {
                maxClassScore = score;
                maxClassId = c;
            }
        }

        AddCxcywhDetection(
            detections,
            det[0],
            det[1],
            det[2],
            det[3],
            objectness * maxClassScore,
            maxClassId,
            scale,
            confThreshold);
    }
}

std::vector<Detection> DecodeYoloOutput(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClassesHint,
    float confThreshold,
    float nmsThreshold,
    int maxDetections,
    float scale,
    std::chrono::duration<double, std::milli>* nmsTime,
    NmsTelemetry* telemetry)
{
    std::vector<Detection> detections;
    if (!output)
        return detections;

    int rows = 0;
    int cols = 0;
    if (!ExtractRowsCols(shape, &rows, &cols))
        return detections;

    // Debug: sample first few output values
    thread_local int debugCount = 0;
    if (config.verbose && ++debugCount % 30 == 1)
    {
        std::cout << "[PostProcess] 输出形状=[" << shape[0];
        for (size_t i = 1; i < shape.size(); ++i) std::cout << "," << shape[i];
        std::cout << "] rows=" << rows << " cols=" << cols << " scale=" << scale << " confThresh=" << confThreshold << std::endl;
        int sampleCount = std::min(5, rows);
        for (int i = 0; i < sampleCount; ++i)
        {
            const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);
            std::cout << "  [" << i << "]: ";
            for (int j = 0; j < std::min(cols, 6); ++j)
                std::cout << det[j] << " ";
            std::cout << std::endl;
        }
    }

    if (cols == 6 && LooksLikeXyxyDetections(output, rows, cols))
    {
        DecodeXyxyDetections(output, rows, cols, confThreshold, scale, detections);
    }
    else if (rows <= cols)
    {
        int classes = 0;
        bool usesObjectness = false;
        if (TryResolveClassLayout(rows, numClassesHint, false, &classes, &usesObjectness))
        {
            DecodeFeatureMajorPredictions(
                output,
                rows,
                cols,
                classes,
                usesObjectness,
                confThreshold,
                scale,
                detections);
        }
    }
    else
    {
        int classes = 0;
        bool usesObjectness = false;
        if (TryResolveClassLayout(cols, numClassesHint, true, &classes, &usesObjectness))
        {
            DecodePredictionMajorPredictions(
                output,
                rows,
                cols,
                classes,
                usesObjectness,
                confThreshold,
                scale,
                detections);
        }
    }

    RunBoundedNms(detections, nmsThreshold, maxDetections, nmsTime, telemetry);
    return detections;
}

void SortDetectionsByConfidence(std::vector<Detection>& detections)
{
    std::sort(
        detections.begin(),
        detections.end(),
        [](const Detection& a, const Detection& b)
        {
            return a.confidence > b.confidence;
        });
}

void LimitDetectionsByConfidence(std::vector<Detection>& detections, size_t limit)
{
    if (limit == 0 || detections.size() <= limit)
        return;

    const auto kth = detections.begin() + static_cast<std::vector<Detection>::difference_type>(limit);
    std::nth_element(
        detections.begin(),
        kth,
        detections.end(),
        [](const Detection& a, const Detection& b)
        {
            return a.confidence > b.confidence;
        });
    detections.resize(limit);
}

void RunBoundedNms(
    std::vector<Detection>& detections,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime,
    NmsTelemetry* telemetry)
{
    constexpr size_t kPreNmsHardLimit = 1000;

    size_t preNmsLimit = kPreNmsHardLimit;
    if (maxDetections > 0)
    {
        const size_t requested = static_cast<size_t>(maxDetections);
        preNmsLimit = std::min(kPreNmsHardLimit, std::max(requested, requested * 8));
    }

    if (telemetry)
        telemetry->preLimitCount = static_cast<uint32_t>(detections.size());
    LimitDetectionsByConfidence(detections, preNmsLimit);
    if (telemetry)
        telemetry->preNmsCount = static_cast<uint32_t>(detections.size());
    NMS(detections, nmsThreshold, nmsTime);

    // NMS 正常路径内部已按置信度降序输出；
    // 仅当 nmsThreshold <= 0（NMS 提前返回、未排序）时才需要补一次排序。
    if (!(nmsThreshold > 0.0f))
        SortDetectionsByConfidence(detections);

    // 原实现此处用 nth_element 打乱已排序序列，再整体 std::sort 复原，属重复工作。
    // 序列既已有序，直接截断即可，结果集合与顺序与原实现完全一致。
    if (maxDetections > 0 && detections.size() > static_cast<size_t>(maxDetections))
        detections.resize(static_cast<size_t>(maxDetections));

    if (telemetry)
        telemetry->postNmsCount = static_cast<uint32_t>(detections.size());
}
}

int InferYoloClassCountFromShape(const std::vector<int64_t>& shape)
{
    if (shape.size() < 2)
        return -1;

    const int64_t rows = shape[shape.size() - 2];
    const int64_t cols = shape[shape.size() - 1];
    if (rows <= 0 || cols <= 0)
        return -1;

    if (rows == 6 || cols == 6)
        return -1;

    const int64_t extent = std::min(rows, cols);
    const int64_t noObjectness = extent - 4;
    const int64_t withObjectness = extent - 5;
    const int64_t inferred = rows > cols ? withObjectness
        : (noObjectness <= Config::MAX_CLASSES ? noObjectness : withObjectness);
    if (inferred <= 0 || inferred > Config::MAX_CLASSES)
        return -1;
    return static_cast<int>(inferred);
}

void NMS(std::vector<Detection>& detections, float nmsThreshold, std::chrono::duration<double, std::milli>* nmsTime)
{
    if (detections.empty()) return;

    if (nmsThreshold <= 0.0f)
    {
        if (nmsTime)
        {
            *nmsTime = std::chrono::duration<double, std::milli>(0);
        }
        return;
    }

    auto t0 = std::chrono::steady_clock::now();

    SortDetectionsByConfidence(detections);

    const size_t count = detections.size();

    // 【正确性】面积改用 float 计算。
    // cv::Rect::area() 返回 int，当框尺寸较大时 width * height 会发生有符号溢出（UB），
    // 使 IoU 变成负值或随机值，导致错误抑制或漏抑制。
    // 【性能】面积预先算好，避免内层循环对同一框重复求面积；
    // 抑制位改用 vector<unsigned char>（vector<bool> 的位打包访问带有额外掩码运算）；
    // 两个缓冲均为 thread_local 复用，稳态下每帧零堆分配。
    thread_local std::vector<float> boxAreas;
    thread_local std::vector<unsigned char> suppress;
    boxAreas.resize(count);
    suppress.assign(count, 0u);

    for (size_t i = 0; i < count; ++i)
    {
        const cv::Rect& box = detections[i].box;
        boxAreas[i] = static_cast<float>(box.width) * static_cast<float>(box.height);
    }

    size_t kept = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (suppress[i]) continue;

        const cv::Rect& box_i = detections[i].box;
        const int right_i = box_i.x + box_i.width;
        const int bottom_i = box_i.y + box_i.height;
        const float area_i = boxAreas[i];

        for (size_t j = i + 1; j < count; ++j)
        {
            if (suppress[j]) continue;

            const cv::Rect& box_j = detections[j].box;
            const int overlapLeft = std::max(box_i.x, box_j.x);
            const int overlapTop = std::max(box_i.y, box_j.y);
            const int overlapRight = std::min(right_i, box_j.x + box_j.width);
            const int overlapBottom = std::min(bottom_i, box_j.y + box_j.height);
            const int overlapWidth = overlapRight - overlapLeft;
            const int overlapHeight = overlapBottom - overlapTop;

            if (overlapWidth > 0 && overlapHeight > 0)
            {
                const float intersection_area =
                    static_cast<float>(overlapWidth) * static_cast<float>(overlapHeight);
                const float union_area = area_i + boxAreas[j] - intersection_area;

                if (union_area > 0.0f && intersection_area / union_area > nmsThreshold)
                {
                    suppress[j] = 1u;
                }
            }
        }

        // 原地压缩替代额外的 result 向量：kept <= i，写入位置始终位于已处理区间内，
        // 不会影响后续 j > i 的读取。省去每次调用一次的向量堆分配与整体拷贝。
        if (kept != i)
            detections[kept] = detections[i];
        ++kept;
    }

    detections.resize(kept);

    auto t1 = std::chrono::steady_clock::now();
    if (nmsTime)
    {
        *nmsTime = t1 - t0;
    }
}

#ifdef USE_CUDA
std::vector<Detection> postProcessYolo(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime,
    NmsTelemetry* telemetry
)
{
    return DecodeYoloOutput(
        output,
        shape,
        numClasses,
        confThreshold,
        nmsThreshold,
        maxDetections,
        trt_detector->img_scale,
        nmsTime,
        telemetry);
}
#endif

#ifndef USE_CUDA
std::vector<Detection> postProcessYoloDML(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime
)
{
    return DecodeYoloOutput(
        output,
        shape,
        numClasses,
        confThreshold,
        nmsThreshold,
        maxDetections,
        1.0f,
        nmsTime,
        nullptr);
}
#endif
