#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "imgui/imgui.h"
#include "mybot.h"
#include "overlay.h"
#include "capture.h"
#include "other_tools.h"
#include "overlay/ui_sections.h"

static void draw_stats_content(bool drawTimingSummary, bool drawPerformanceDetails)
{
    static float preprocess_times[120] = {};
    static float inference_times[120] = {};
    static float copy_times[120] = {};
    static float postprocess_times[120] = {};
    static float nms_times[120] = {};
    static int index_inf = 0;

    static float capture_fps_vals[120] = {};
    static int index_fps = 0;

    static float avg_preprocess_cached = 0.0f;
    static float avg_inference_cached = 0.0f;
    static float avg_copy_cached = 0.0f;
    static float avg_post_cached = 0.0f;
    static float avg_nms_cached = 0.0f;
    static float avg_fps_cached = 0.0f;
    static double last_avg_update_time = 0.0;

    float current_preprocess = 0.0f;
    float current_inference = 0.0f;
    float current_copy = 0.0f;
    float current_post = 0.0f;
    float current_nms = 0.0f;
    uint64_t submittedFrameSequence = 0;
    uint64_t overwrittenFrameCount = 0;
    uint32_t preLimitCount = 0;
    uint32_t preNmsCount = 0;
    uint32_t postNmsCount = 0;

#ifdef USE_CUDA
    if (trt_detector)
    {
        current_preprocess = static_cast<float>(trt_detector->lastPreprocessTimeMs.load(std::memory_order_relaxed));
        current_inference = static_cast<float>(trt_detector->lastInferenceTimeMs.load(std::memory_order_relaxed));
        current_copy = static_cast<float>(trt_detector->lastCopyTimeMs.load(std::memory_order_relaxed));
        current_post = static_cast<float>(trt_detector->lastPostprocessTimeMs.load(std::memory_order_relaxed));
        current_nms = static_cast<float>(trt_detector->lastNmsTimeMs.load(std::memory_order_relaxed));
        submittedFrameSequence = trt_detector->submittedFrameSequence.load(std::memory_order_relaxed);
        overwrittenFrameCount = trt_detector->overwrittenFrameCount.load(std::memory_order_relaxed);
        preLimitCount = trt_detector->lastPreLimitCount.load(std::memory_order_relaxed);
        preNmsCount = trt_detector->lastPreNmsCount.load(std::memory_order_relaxed);
        postNmsCount = trt_detector->lastPostNmsCount.load(std::memory_order_relaxed);
    }
#else
    if (dml_detector)
    {
        current_preprocess = static_cast<float>(dml_detector->lastPreprocessTimeMs.load(std::memory_order_relaxed));
        current_inference = static_cast<float>(dml_detector->lastInferenceTimeMs.load(std::memory_order_relaxed));
        current_copy = static_cast<float>(dml_detector->lastCopyTimeMs.load(std::memory_order_relaxed));
        current_post = static_cast<float>(dml_detector->lastPostprocessTimeMs.load(std::memory_order_relaxed));
        current_nms = static_cast<float>(dml_detector->lastNmsTimeMs.load(std::memory_order_relaxed));
    }
#endif

    preprocess_times[index_inf] = current_preprocess;
    inference_times[index_inf] = current_inference;
    copy_times[index_inf] = current_copy;
    postprocess_times[index_inf] = current_post;
    nms_times[index_inf] = current_nms;
    index_inf = (index_inf + 1) % IM_ARRAYSIZE(inference_times);

    float current_fps = static_cast<float>(captureFps.load());
    capture_fps_vals[index_fps] = current_fps;
    index_fps = (index_fps + 1) % IM_ARRAYSIZE(capture_fps_vals);

    auto avg = [](const float* arr, int n) -> float {
        float sum = 0.0f; int cnt = 0;
        for (int i = 0; i < n; ++i)
            if (arr[i] > 0.0f) { sum += arr[i]; ++cnt; }
        return cnt ? (sum / cnt) : 0.0f;
        };

    const double now = ImGui::GetTime();
    if (last_avg_update_time == 0.0 || (now - last_avg_update_time) >= 1.0)
    {
        avg_preprocess_cached = avg(preprocess_times, IM_ARRAYSIZE(preprocess_times));
        avg_inference_cached = avg(inference_times, IM_ARRAYSIZE(inference_times));
        avg_copy_cached = avg(copy_times, IM_ARRAYSIZE(copy_times));
        avg_post_cached = avg(postprocess_times, IM_ARRAYSIZE(postprocess_times));
        avg_nms_cached = avg(nms_times, IM_ARRAYSIZE(nms_times));
        avg_fps_cached = avg(capture_fps_vals, IM_ARRAYSIZE(capture_fps_vals));

        last_avg_update_time = now;
    }

    const bool captureUsesMonitorRefresh =
        config.capture_method == "duplication_api" ||
        (config.capture_method == "winrt" && config.capture_target != "window");

    static int cachedRefreshMonitorIdx = -1;
    static double cachedRefreshQueryTime = -100.0;
    static double cachedMonitorRefreshHz = 0.0;
    if (captureUsesMonitorRefresh)
    {
        const int monitorIdx = std::max(0, config.monitor_idx);
        if (cachedRefreshMonitorIdx != monitorIdx || now - cachedRefreshQueryTime >= 2.0)
        {
            cachedMonitorRefreshHz = GetMonitorRefreshRateByIndex(monitorIdx);
            cachedRefreshMonitorIdx = monitorIdx;
            cachedRefreshQueryTime = now;
        }
    }

if (drawTimingSummary)
{
    ImGui::PushID("stats_section_time_breakdown");

    const float avgInferenceFps =
        avg_inference_cached > 0.0f ? 1000.0f / avg_inference_cached : 0.0f;

    ImGui::Text("可承载的推理帧率上限: %.1f FPS", avgInferenceFps);
    ImGui::Text("预处理 %.2f ms / 平均 %.2f ms", current_preprocess, avg_preprocess_cached);
    ImGui::Text("内存拷贝 %.2f ms / 平均 %.2f ms", current_copy, avg_copy_cached);
    ImGui::Text("后处理 %.2f ms / 平均 %.2f ms", current_post, avg_post_cached);

#ifdef USE_CUDA
    ImGui::Text(
        "NMS %.2f ms / 平均 %.2f ms | 数量：限制前 %u，NMS 前 %u，NMS 后 %u",
        current_nms,
        avg_nms_cached,
        preLimitCount,
        preNmsCount,
        postNmsCount
    );
#else
    ImGui::Text("NMS %.2f ms / 平均 %.2f ms", current_nms, avg_nms_cached);
#endif

    ImGui::PopID();
}

    if (!drawPerformanceDetails)
        return;

    ImGui::PushID("stats_section_capture_fps");
        if (captureUsesMonitorRefresh && cachedMonitorRefreshHz > 1.0)
        {
            const float refreshHz = static_cast<float>(cachedMonitorRefreshHz);
            const float fpsLoad = std::clamp(avg_fps_cached / refreshHz, 0.0f, 1.0f);
            char fpsCombinedText[160] = {};
            std::snprintf(fpsCombinedText, sizeof(fpsCombinedText),
                "当前: %.1f | 平均: %.1f | 显示器负载 %.1f / %.1f Hz (%.0f%%)", current_fps, avg_fps_cached,
                avg_fps_cached, refreshHz, fpsLoad * 100.0f);
            if (ImGui::CalcTextSize(fpsCombinedText).x <= ImGui::GetContentRegionAvail().x)
            {
                ImGui::Text("%s", fpsCombinedText);
            }
            else
            {
                ImGui::Text("当前: %.1f | 平均: %.1f", current_fps, avg_fps_cached);
                ImGui::Text("显示器负载 %.1f / %.1f Hz (%.0f%%)", avg_fps_cached, refreshHz, fpsLoad * 100.0f);
            }
        }
        else
            ImGui::Text("当前: %.1f | 平均: %.1f", current_fps, avg_fps_cached);
    ImGui::PopID();

    int latestWidth = 0;
    int latestHeight = 0;
    size_t queueDepth = 0;
    {
        std::lock_guard<std::mutex> lk(frameMutex);
        if (!latestFrame.empty())
        {
            latestWidth = latestFrame.cols;
            latestHeight = latestFrame.rows;
        }
        queueDepth = frameQueue.size();
    }

    const int captureFpsLimit = std::max(0, config.capture_fps);
    const float currentFrameTimeMs = (current_fps > 0.01f) ? (1000.0f / current_fps) : 0.0f;
    const float avgFrameTimeMs = (avg_fps_cached > 0.01f) ? (1000.0f / avg_fps_cached) : 0.0f;
    const int sourceWidth = screenWidth.load(std::memory_order_relaxed);
    const int sourceHeight = screenHeight.load(std::memory_order_relaxed);

    std::string captureSource = "未知";
    std::string sourceSizeLabel = "桌面尺寸";
    if (config.capture_method == "duplication_api")
    {
        captureSource = "显示器 " + std::to_string(std::max(0, config.monitor_idx) + 1);
    }
    else if (config.capture_method == "winrt")
    {
        if (config.capture_target == "window")
        {
            captureSource = config.capture_window_title.empty()
                ? "窗口目标为空"
                : "窗口: " + config.capture_window_title;
            sourceSizeLabel = "窗口尺寸";
        }
        else
        {
            captureSource = "显示器 " + std::to_string(std::max(0, config.monitor_idx) + 1);
        }
    }
    else if (config.capture_method == "virtual_camera")
    {
        captureSource =
            "摄像头: " + config.virtual_camera_name + " (" +
            std::to_string(config.virtual_camera_width) + "x" +
            std::to_string(config.virtual_camera_heigth) + ")";
        sourceSizeLabel = "摄像头尺寸";
    }
    else if (config.capture_method == "udp_capture")
    {
        captureSource = "UDP " + config.udp_ip + ":" + std::to_string(config.udp_port);
        sourceSizeLabel = "流尺寸";
    }

    auto drawMovedPerformanceSummary = [&]() {
        ImGui::SeparatorText(captureUsesMonitorRefresh ? "显示器捕获" : "采集源");
        if (captureUsesMonitorRefresh)
            ImGui::Text("显示器 %d", std::max(0, config.monitor_idx) + 1);
        else
            ImGui::TextUnformatted(captureSource.c_str());

        const float avgInferenceFps =
            avg_inference_cached > 0.0f ? 1000.0f / avg_inference_cached : 0.0f;

        ImGui::Text("可承载的推理帧率上限: %.1f FPS", avgInferenceFps);
        ImGui::Text("预处理 %.2f ms / 平均 %.2f ms", current_preprocess, avg_preprocess_cached);
        ImGui::Text("内存拷贝 %.2f ms / 平均 %.2f ms", current_copy, avg_copy_cached);
        ImGui::Text("后处理 %.2f ms / 平均 %.2f ms", current_post, avg_post_cached);

#ifdef USE_CUDA
        ImGui::Text(
            "NMS %.2f ms / 平均 %.2f ms | 数量：限制前 %u，NMS 前 %u，NMS 后 %u",
            current_nms,
            avg_nms_cached,
            preLimitCount,
            preNmsCount,
            postNmsCount
        );
#else
        ImGui::Text("NMS %.2f ms / 平均 %.2f ms", current_nms, avg_nms_cached);
#endif
    };

    ImGui::PushID("stats_section_capture_details");
        const std::string captureMethodBackend = "方式: " + config.capture_method + " | 后端: " + config.backend;
        if (ImGui::CalcTextSize(captureMethodBackend.c_str()).x <= ImGui::GetContentRegionAvail().x)
            ImGui::Text("%s", captureMethodBackend.c_str());
        else
        {
            ImGui::Text("方式: %s", config.capture_method.c_str());
            ImGui::Text("后端: %s", config.backend.c_str());
        }
        ImGui::TextWrapped("来源: %s", captureSource.c_str());

        const bool hasSourceSize = sourceWidth > 0 && sourceHeight > 0;
        const bool hasLatestFrame = latestWidth > 0 && latestHeight > 0;
        if (hasSourceSize && hasLatestFrame)
        {
            const std::string sourceAndLatest = sourceSizeLabel + ": " + std::to_string(sourceWidth) + "x" +
                std::to_string(sourceHeight) + " | 最新帧: " + std::to_string(latestWidth) + "x" +
                std::to_string(latestHeight);
            if (ImGui::CalcTextSize(sourceAndLatest.c_str()).x <= ImGui::GetContentRegionAvail().x)
                ImGui::Text("%s", sourceAndLatest.c_str());
            else
            {
                ImGui::Text("%s: %dx%d", sourceSizeLabel.c_str(), sourceWidth, sourceHeight);
                ImGui::Text("最新帧: %dx%d", latestWidth, latestHeight);
            }
        }
        else
        {
            if (hasSourceSize)
                ImGui::Text("%s: %dx%d", sourceSizeLabel.c_str(), sourceWidth, sourceHeight);
            else
                ImGui::TextDisabled("%s: 无", sourceSizeLabel.c_str());

            if (hasLatestFrame)
                ImGui::Text("最新帧: %dx%d", latestWidth, latestHeight);
            else
                ImGui::TextDisabled("最新帧: 无");
        }

        const std::string captureLimit = captureFpsLimit > 0
            ? "采集帧率限制: " + std::to_string(captureFpsLimit)
            : "采集帧率限制: 无限制";
        const std::string resolutionAndLimit = "检测分辨率: " + std::to_string(config.detection_resolution) +
            " | " + captureLimit;
        if (captureUsesMonitorRefresh && cachedMonitorRefreshHz > 0.0)
        {
            char refreshResolutionAndLimit[192] = {};
            std::snprintf(refreshResolutionAndLimit, sizeof(refreshResolutionAndLimit),
                "显示器刷新率: %.2f Hz | %s", cachedMonitorRefreshHz, resolutionAndLimit.c_str());
            if (ImGui::CalcTextSize(refreshResolutionAndLimit).x <= ImGui::GetContentRegionAvail().x)
                ImGui::Text("%s", refreshResolutionAndLimit);
            else
            {
                ImGui::Text("显示器刷新率: %.2f Hz", cachedMonitorRefreshHz);
                if (ImGui::CalcTextSize(resolutionAndLimit.c_str()).x <= ImGui::GetContentRegionAvail().x)
                    ImGui::Text("%s", resolutionAndLimit.c_str());
                else
                {
                    ImGui::Text("检测分辨率: %d", config.detection_resolution);
                    ImGui::Text("%s", captureLimit.c_str());
                }
            }
        }
        else if (captureUsesMonitorRefresh)
        {
            ImGui::TextDisabled("显示器刷新率: 无");
            if (ImGui::CalcTextSize(resolutionAndLimit.c_str()).x <= ImGui::GetContentRegionAvail().x)
                ImGui::Text("%s", resolutionAndLimit.c_str());
            else
            {
                ImGui::Text("检测分辨率: %d", config.detection_resolution);
                ImGui::Text("%s", captureLimit.c_str());
            }
        }
        else if (ImGui::CalcTextSize(resolutionAndLimit.c_str()).x <= ImGui::GetContentRegionAvail().x)
            ImGui::Text("%s", resolutionAndLimit.c_str());
        else
        {
            ImGui::Text("检测分辨率: %d", config.detection_resolution);
            ImGui::Text("%s", captureLimit.c_str());
        }

        if (currentFrameTimeMs > 0.0f || avgFrameTimeMs > 0.0f)
        {
            char frameTimeAndFov[160] = {};
            std::snprintf(frameTimeAndFov, sizeof(frameTimeAndFov),
                "帧时间: 当前 %.2f ms | 平均 %.2f ms | 圆形视野: %s", currentFrameTimeMs, avgFrameTimeMs,
                "开");
            if (ImGui::CalcTextSize(frameTimeAndFov).x <= ImGui::GetContentRegionAvail().x)
                ImGui::Text("帧时间: 当前 %.2f ms | 平均 %.2f ms | 圆形视野: %s", currentFrameTimeMs,
                    avgFrameTimeMs, "开");
            else
            {
                ImGui::Text("帧时间: 当前 %.2f ms | 平均 %.2f ms", currentFrameTimeMs, avgFrameTimeMs);
                ImGui::Text("圆形视野: 开");
            }
        }
        else
        {
            ImGui::TextDisabled("帧时间: 无");
            ImGui::Text("圆形视野: 开");
        }

#ifdef USE_CUDA
        char queueAndDetectionFrames[192] = {};
        std::snprintf(queueAndDetectionFrames, sizeof(queueAndDetectionFrames),
            "帧队列深度: %d | 检测最新帧: %llu | 覆盖未消费帧: %llu", static_cast<int>(queueDepth),
            static_cast<unsigned long long>(submittedFrameSequence),
            static_cast<unsigned long long>(overwrittenFrameCount));
        if (ImGui::CalcTextSize(queueAndDetectionFrames).x <= ImGui::GetContentRegionAvail().x)
            ImGui::Text("%s", queueAndDetectionFrames);
        else
        {
            ImGui::Text("帧队列深度: %d", static_cast<int>(queueDepth));
            ImGui::Text("检测最新帧: %llu | 覆盖未消费帧: %llu",
                static_cast<unsigned long long>(submittedFrameSequence),
                static_cast<unsigned long long>(overwrittenFrameCount));
        }
#else
        ImGui::Text("帧队列深度: %d", static_cast<int>(queueDepth));
#endif
        static bool winrtStatsInitialized = false;
        static uint64_t lastWinrtPolls = 0;
        static uint64_t lastWinrtDrained = 0;
        static uint64_t lastWinrtReturned = 0;
        static uint64_t lastWinrtEmpty = 0;
        static uint64_t lastWinrtReadbackMicros = 0;
        static uint64_t lastWinrtMapMicros = 0;
        static uint64_t lastWinrtPixelCopyMicros = 0;
        static double lastWinrtStatsTime = 0.0;
        static float winrtPollRate = 0.0f;
        static float winrtDrainedRate = 0.0f;
        static float winrtReturnedRate = 0.0f;
        static float winrtEmptyRate = 0.0f;
        static float winrtReadbackAvgMs = 0.0f;
        static float winrtMapAvgMs = 0.0f;
        static float winrtPixelCopyAvgMs = 0.0f;

        if (config.capture_method == "winrt")
        {
            const uint64_t winrtPolls = captureWinrtPollAttemptsTotal.load(std::memory_order_relaxed);
            const uint64_t winrtDrained = captureWinrtFramesDrainedTotal.load(std::memory_order_relaxed);
            const uint64_t winrtReturned = captureWinrtFramesReturnedTotal.load(std::memory_order_relaxed);
            const uint64_t winrtEmpty = captureWinrtEmptyPollsTotal.load(std::memory_order_relaxed);
            const uint64_t winrtReadbackMicros = captureWinrtReadbackMicrosTotal.load(std::memory_order_relaxed);
            const uint64_t winrtMapMicros = captureWinrtMapMicrosTotal.load(std::memory_order_relaxed);
            const uint64_t winrtPixelCopyMicros = captureWinrtPixelCopyMicrosTotal.load(std::memory_order_relaxed);

            if (!winrtStatsInitialized)
            {
                lastWinrtPolls = winrtPolls;
                lastWinrtDrained = winrtDrained;
                lastWinrtReturned = winrtReturned;
                lastWinrtEmpty = winrtEmpty;
                lastWinrtReadbackMicros = winrtReadbackMicros;
                lastWinrtMapMicros = winrtMapMicros;
                lastWinrtPixelCopyMicros = winrtPixelCopyMicros;
                lastWinrtStatsTime = now;
                winrtStatsInitialized = true;
            }
            else if (now - lastWinrtStatsTime >= 1.0)
            {
                const float dt = static_cast<float>(std::max(0.001, now - lastWinrtStatsTime));
                const uint64_t returnedDelta = winrtReturned - lastWinrtReturned;
                winrtPollRate = static_cast<float>(winrtPolls - lastWinrtPolls) / dt;
                winrtDrainedRate = static_cast<float>(winrtDrained - lastWinrtDrained) / dt;
                winrtReturnedRate = static_cast<float>(returnedDelta) / dt;
                winrtEmptyRate = static_cast<float>(winrtEmpty - lastWinrtEmpty) / dt;
                if (returnedDelta > 0)
                {
                    winrtReadbackAvgMs = static_cast<float>(winrtReadbackMicros - lastWinrtReadbackMicros) /
                        (1000.0f * static_cast<float>(returnedDelta));
                    winrtMapAvgMs = static_cast<float>(winrtMapMicros - lastWinrtMapMicros) /
                        (1000.0f * static_cast<float>(returnedDelta));
                    winrtPixelCopyAvgMs = static_cast<float>(winrtPixelCopyMicros - lastWinrtPixelCopyMicros) /
                        (1000.0f * static_cast<float>(returnedDelta));
                }

                lastWinrtPolls = winrtPolls;
                lastWinrtDrained = winrtDrained;
                lastWinrtReturned = winrtReturned;
                lastWinrtEmpty = winrtEmpty;
                lastWinrtReadbackMicros = winrtReadbackMicros;
                lastWinrtMapMicros = winrtMapMicros;
                lastWinrtPixelCopyMicros = winrtPixelCopyMicros;
                lastWinrtStatsTime = now;
            }

            ImGui::Separator();
            ImGui::Text("WinRT 帧率: %.1f/s | 拉出: %.1f/s", winrtReturnedRate, winrtDrainedRate);
            ImGui::Text("WinRT 空轮询: %.1f/s | 轮询: %.1f/s", winrtEmptyRate, winrtPollRate);
            ImGui::Text("WinRT 回读平均: %.3f ms | 映射: %.3f ms", winrtReadbackAvgMs, winrtMapAvgMs);
            ImGui::Text("WinRT 内存拷贝平均: %.3f ms", winrtPixelCopyAvgMs);
        }
        else
        {
            winrtStatsInitialized = false;
        }

#ifdef USE_CUDA
        if (config.backend == "TRT")
        {
            drawMovedPerformanceSummary();
            ImGui::Separator();
            ImGui::Text("CUDA直接捕获: %s", config.capture_use_cuda ? "已启用" : "已禁用");

            static uint64_t lastGpuAttempts = 0;
            static uint64_t lastGpuCaptured = 0;
            static uint64_t lastGpuTimeouts = 0;
            static uint64_t lastGpuAccumulated = 0;
            static uint64_t lastGpuMissed = 0;
            static uint64_t lastGpuPresent = 0;
            static uint64_t lastGpuMouseOnly = 0;
            static uint64_t lastGpuMetadataOnly = 0;
            static uint64_t lastGpuCoalesced = 0;
            static uint64_t lastCpuFallbackAttempts = 0;
            static uint64_t lastCpuFallbackFrames = 0;
            static double lastGpuStatsTime = 0.0;
            static float gpuAttemptRate = 0.0f;
            static float gpuCapturedRate = 0.0f;
            static float gpuTimeoutRate = 0.0f;
            static float gpuAccumulatedRate = 0.0f;
            static float gpuMissedRate = 0.0f;
            static float gpuPresentRate = 0.0f;
            static float gpuMouseOnlyRate = 0.0f;
            static float gpuMetadataOnlyRate = 0.0f;
            static float gpuCoalescedRate = 0.0f;
            static float cpuFallbackAttemptRate = 0.0f;
            static float cpuFallbackFrameRate = 0.0f;

            const uint64_t gpuAttempts = captureGpuAttemptsTotal.load(std::memory_order_relaxed);
            const uint64_t gpuCaptured = captureGpuCapturedTotal.load(std::memory_order_relaxed);
            const uint64_t gpuTimeouts = captureGpuTimeoutTotal.load(std::memory_order_relaxed);
            const uint64_t gpuAccumulated = captureGpuAccumulatedFramesTotal.load(std::memory_order_relaxed);
            const uint64_t gpuMissed = captureGpuMissedFramesTotal.load(std::memory_order_relaxed);
            const uint64_t gpuPresent = captureGpuPresentFramesTotal.load(std::memory_order_relaxed);
            const uint64_t gpuMouseOnly = captureGpuMouseOnlyEventsTotal.load(std::memory_order_relaxed);
            const uint64_t gpuMetadataOnly = captureGpuMetadataOnlyEventsTotal.load(std::memory_order_relaxed);
            const uint64_t gpuCoalesced = captureGpuCoalescedEventsTotal.load(std::memory_order_relaxed);
            const uint64_t cpuFallbackAttempts = captureCpuFallbackAttemptsTotal.load(std::memory_order_relaxed);
            const uint64_t cpuFallbackFrames = captureCpuFallbackFramesTotal.load(std::memory_order_relaxed);

            if (lastGpuStatsTime <= 0.0)
            {
                lastGpuAttempts = gpuAttempts;
                lastGpuCaptured = gpuCaptured;
                lastGpuTimeouts = gpuTimeouts;
                lastGpuAccumulated = gpuAccumulated;
                lastGpuMissed = gpuMissed;
                lastGpuPresent = gpuPresent;
                lastGpuMouseOnly = gpuMouseOnly;
                lastGpuMetadataOnly = gpuMetadataOnly;
                lastGpuCoalesced = gpuCoalesced;
                lastCpuFallbackAttempts = cpuFallbackAttempts;
                lastCpuFallbackFrames = cpuFallbackFrames;
                lastGpuStatsTime = now;
            }
            else if (now - lastGpuStatsTime >= 1.0)
            {
                const float dt = static_cast<float>(std::max(0.001, now - lastGpuStatsTime));
                gpuAttemptRate = static_cast<float>(gpuAttempts - lastGpuAttempts) / dt;
                gpuCapturedRate = static_cast<float>(gpuCaptured - lastGpuCaptured) / dt;
                gpuTimeoutRate = static_cast<float>(gpuTimeouts - lastGpuTimeouts) / dt;
                gpuAccumulatedRate = static_cast<float>(gpuAccumulated - lastGpuAccumulated) / dt;
                gpuMissedRate = static_cast<float>(gpuMissed - lastGpuMissed) / dt;
                gpuPresentRate = static_cast<float>(gpuPresent - lastGpuPresent) / dt;
                gpuMouseOnlyRate = static_cast<float>(gpuMouseOnly - lastGpuMouseOnly) / dt;
                gpuMetadataOnlyRate = static_cast<float>(gpuMetadataOnly - lastGpuMetadataOnly) / dt;
                gpuCoalescedRate = static_cast<float>(gpuCoalesced - lastGpuCoalesced) / dt;
                cpuFallbackAttemptRate = static_cast<float>(cpuFallbackAttempts - lastCpuFallbackAttempts) / dt;
                cpuFallbackFrameRate = static_cast<float>(cpuFallbackFrames - lastCpuFallbackFrames) / dt;

                lastGpuAttempts = gpuAttempts;
                lastGpuCaptured = gpuCaptured;
                lastGpuTimeouts = gpuTimeouts;
                lastGpuAccumulated = gpuAccumulated;
                lastGpuMissed = gpuMissed;
                lastGpuPresent = gpuPresent;
                lastGpuMouseOnly = gpuMouseOnly;
                lastGpuMetadataOnly = gpuMetadataOnly;
                lastGpuCoalesced = gpuCoalesced;
                lastCpuFallbackAttempts = cpuFallbackAttempts;
                lastCpuFallbackFrames = cpuFallbackFrames;
                lastGpuStatsTime = now;
            }

            ImGui::Text("DDA 已提交帧: %.1f/s (成功获取到的屏幕帧)", gpuCapturedRate);
            ImGui::Text("DDA 尝试: %.1f/s (尝试获取屏幕帧的次数)", gpuAttemptRate);
            ImGui::Text("DDA 呈现帧: %.1f/s (屏幕实际发生变化的帧)", gpuPresentRate);
            ImGui::Text("DDA 仅鼠标: %.1f/s (只有鼠标变化，没有画面变化)", gpuMouseOnlyRate);
            ImGui::Text("DDA 仅元数据: %.1f/s (收到变化通知，但没有新的图像)", gpuMetadataOnlyRate);
            ImGui::Text("DDA 合并: %.1f/s (多个更新被合并处理)", gpuCoalescedRate);
            ImGui::Text("DDA GPU超时: %.1f/s (等待新画面超时次数)", gpuTimeoutRate);
            ImGui::Text("DDA 累积: %.1f/s (等待事件累计次数)", gpuAccumulatedRate);
            ImGui::Text("DDA GPU丢失/合并: %.1f/s (未及时处理的帧或合并帧)", gpuMissedRate);
            ImGui::Text("DDA CPU回退帧: %.1f/s (GPU采集失败后使用CPU处理)", cpuFallbackFrameRate);
            ImGui::Text("DDA CPU回退尝试: %.1f/s (尝试切换CPU处理的次数)", cpuFallbackAttemptRate);
        }
#endif

    ImGui::PopID();
}

void draw_stats_summary()
{
    draw_stats_content(true, false);
}

void draw_stats()
{
    draw_stats_content(false, true);
}

void draw_inference_capacity_line()
{
    float currentInferenceMs = 0.0f;
#ifdef USE_CUDA
    if (trt_detector)
        currentInferenceMs = static_cast<float>(trt_detector->lastInferenceTimeMs.load(std::memory_order_relaxed));
#else
    if (dml_detector)
        currentInferenceMs = static_cast<float>(dml_detector->lastInferenceTimeMs.load(std::memory_order_relaxed));
#endif

    static float capacityInferenceTimes[120] = {};
    static int capacityIndex = 0;
    capacityInferenceTimes[capacityIndex] = currentInferenceMs;
    capacityIndex = (capacityIndex + 1) % IM_ARRAYSIZE(capacityInferenceTimes);

    float sum = 0.0f;
    int count = 0;
    for (float sample : capacityInferenceTimes)
    {
        if (sample > 0.0f)
        {
            sum += sample;
            ++count;
        }
    }

    const float avgInferenceMs = count > 0 ? sum / static_cast<float>(count) : 0.0f;
    const float fps = avgInferenceMs > 0.0f ? 1000.0f / avgInferenceMs : 0.0f;
    ImGui::Text("可承载的推理帧率上限: %.1f FPS", fps);
}
