#pragma once
#include <cstdint>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <opencv2/opencv.hpp>

struct DetectionBuffer
{
    std::mutex mutex;
    std::condition_variable cv;
    // 【第 28 轮修复】原类型为 int。version 只增不减，消费者
    // （runtime/mouse_thread_loop.cpp:309/315）用 `version > lastVersion` 判定新帧：
    //   1. 有符号整数溢出本身是未定义行为；
    //   2. 一旦回绕为负，`version > lastVersion` 将**永久为假**，
    //      鼠标线程再也取不到任何新检测（现象：程序照常运行但完全不瞄准），
    //      且需再走完约 2^32 帧才可能自愈。
    // 改用 uint64_t：按 1000 FPS 计需连续运行 5.8 亿年才回绕，
    // 彻底消除 UB 与挂死路径；64 位平台上存储与比较开销无变化。
    std::uint64_t version = 0;
    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    std::vector<float> confidences;
    std::chrono::steady_clock::time_point frameTimestamp{};
    std::chrono::steady_clock::time_point publishTimestamp{};

    void set(const std::vector<cv::Rect>& newBoxes, const std::vector<int>& newClasses)
    {
        set(newBoxes, newClasses, std::vector<float>(), {});
    }

    void set(
        const std::vector<cv::Rect>& newBoxes,
        const std::vector<int>& newClasses,
        const std::vector<float>& newConfidences,
        std::chrono::steady_clock::time_point newFrameTimestamp = {})
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        boxes = newBoxes;
        classes = newClasses;
        confidences = newConfidences;
        frameTimestamp = (newFrameTimestamp.time_since_epoch().count() != 0) ? newFrameTimestamp : now;
        publishTimestamp = now;
        ++version;
        cv.notify_all();
    }

    void clear()
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        boxes.clear();
        classes.clear();
        confidences.clear();
        frameTimestamp = now;
        publishTimestamp = now;
        ++version;
        cv.notify_all();
    }

    void get(
        std::vector<cv::Rect>& outBoxes,
        std::vector<int>& outClasses,
        std::uint64_t& outVersion,
        std::chrono::steady_clock::time_point* outFrameTimestamp = nullptr,
        std::chrono::steady_clock::time_point* outPublishTimestamp = nullptr)
    {
        std::vector<float> ignoredConfidences;
        get(outBoxes, outClasses, ignoredConfidences, outVersion, outFrameTimestamp, outPublishTimestamp);
    }

    void get(
        std::vector<cv::Rect>& outBoxes,
        std::vector<int>& outClasses,
        std::vector<float>& outConfidences,
        std::uint64_t& outVersion,
        std::chrono::steady_clock::time_point* outFrameTimestamp = nullptr,
        std::chrono::steady_clock::time_point* outPublishTimestamp = nullptr)
    {
        std::lock_guard<std::mutex> lock(mutex);
        outBoxes = boxes;
        outClasses = classes;
        outConfidences = confidences;
        outVersion = version;
        if (outFrameTimestamp)
            *outFrameTimestamp = frameTimestamp;
        if (outPublishTimestamp)
            *outPublishTimestamp = publishTimestamp;
    }
};
