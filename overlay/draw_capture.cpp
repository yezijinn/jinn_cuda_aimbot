#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <commdlg.h>

#include <string.h>
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <vector>

#include <imgui/imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "config.h"
#include "mybot.h"
#include "capture.h"
#include "other_tools.h"
#include "virtual_camera.h"
#include "draw_settings.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "overlay/onnx_inspector.h"

bool disable_winrt_futures = checkwin1903();
int monitors = get_active_monitors();

static std::vector<std::string> virtual_cameras;
static std::vector<CaptureWindowInfo> capture_windows;
static char virtual_camera_filter_buf[128] = "";
static char capture_window_filter_buf[128] = "";
static char udp_ip_buf[64] = "";
static int udp_port_buf = 1234;
static bool capture_windows_loaded = false;
static bool udp_settings_init = false;
static bool virtual_camera_settings_pending = false;
static std::future<OnnxInspectionResult> onnx_inspection_task;
static std::string onnx_inspection_text;
static std::string onnx_inspection_path;
static bool onnx_inspection_running = false;
static std::string onnx_inspection_task_path;

enum class OnnxInspectionMode
{
    Brief,
    Full
};

static OnnxInspectionMode onnx_inspection_running_mode = OnnxInspectionMode::Full;
static OnnxInspectionMode onnx_inspection_queued_mode = OnnxInspectionMode::Full;
static bool onnx_inspection_mode_queued = false;

static constexpr const char* kOnnxInspectionPrompt =
    "提示：你还未选择.onnx文件，请先指定一个.onnx文件。";

static std::string chooseOnnxFile()
{
    wchar_t fileName[MAX_PATH] = L"";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = static_cast<DWORD>(std::size(fileName));
    dialog.lpstrFilter = L"ONNX models (*.onnx)\0*.onnx\0All files (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog))
        return {};

    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, fileName, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 1)
        return {};

    std::string utf8Path(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, fileName, -1, utf8Path.data(), utf8Length, nullptr, nullptr);
    utf8Path.resize(static_cast<std::size_t>(utf8Length - 1));
    return utf8Path;
}

static void startOnnxInspection(OnnxInspectionMode mode)
{
    onnx_inspection_running_mode = mode;
    onnx_inspection_task_path = onnx_inspection_path;
    onnx_inspection_text = "正在读取模型，请稍候...";
    onnx_inspection_running = true;
    const std::string modelPath = onnx_inspection_task_path;
    try
    {
        onnx_inspection_task = std::async(std::launch::async, [modelPath] {
            return inspectOnnxModel(modelPath);
        });
    }
    catch (const std::exception& error)
    {
        onnx_inspection_text = std::string("读取模型信息时发生异常: ") + error.what();
        onnx_inspection_running = false;
    }
    catch (...)
    {
        onnx_inspection_text = "读取模型信息时发生未知异常。";
        onnx_inspection_running = false;
    }
}

static void requestWinrtCaptureRestart()
{
    capture_method_changed.store(true);
    capture_window_changed.store(true);
}

// 函数: requestWinrtCaptureRestart
// 作用: 当捕获方式或目标窗口需要重启捕获流程时调用此函数。
// 说明: 该函数通过设置原子标志通知捕获线程或管理线程重新初始化捕获
//      配置（例如切换到 WinRT 捕获或改变目标窗口后需要重启捕获）。


static void refreshCaptureWindowList()
{
    capture_windows = EnumerateCaptureWindows();
    capture_windows_loaded = true;
}

// 函数: refreshCaptureWindowList
// 作用: 枚举当前系统中所有可供捕获的窗口或显示设备，并缓存到 capture_windows。
// 说明: 该函数在用户请求刷新窗口列表或初始化时调用，枚举结果用于 UI 下拉
//      列表供用户选择捕获目标。执行后设置 capture_windows_loaded 标志为真。


static bool captureWindowMatchesTitle(const CaptureWindowInfo& window, const std::string& title)
{
    const std::string needle = OtherTools::TrimAscii(title);
    if (needle.empty())
        return false;

    return window.title == needle ||
        window.title.find(needle) != std::string::npos ||
        OtherTools::ContainsCaseInsensitive(window.title, needle);
}

// 函数: captureWindowMatchesTitle
// 作用: 判断给定的捕获窗口信息的标题是否与用户输入的目标标题匹配。
// 说明: 匹配规则包括完全相等、包含子串或不区分大小写的包含匹配。
//      用于在窗口列表中查找用户指定的捕获目标窗口。


static bool currentWindowTitleIsInList()
{
    for (const auto& window : capture_windows)
        if (captureWindowMatchesTitle(window, config.capture_window_title))
            return true;
    return false;
}

// 函数: currentWindowTitleIsInList
// 作用: 检查当前配置的 capture_window_title 是否存在于已枚举的窗口列表中。
// 说明: 如果用户在配置中直接输入了一个窗口标题，调用本函数可以判断该标题
//      是否出现在系统的窗口列表里，以便提供反馈或自动选择。


static void applyWinrtWindowTarget(const std::string& title)
{
    if (config.capture_window_title != title)
    {
        config.capture_window_title = title;
        OverlayConfig_MarkDirty();
    }

    requestWinrtCaptureRestart();
}

// 函数: applyWinrtWindowTarget
// 作用: 将用户选择或输入的窗口标题应用到配置并触发捕获重启。
// 说明: 首先比较新标题与现有配置，若不同则写入 config.capture_window_title 并标记配置为已更改，
//      最后调用 requestWinrtCaptureRestart 通知捕获子系统重启以应用新目标。


// 函数: ensureVirtualCamerasLoaded
// 作用: 如果虚拟摄像头列表尚未加载，则从系统查询可用的虚拟摄像头并缓存到 virtual_cameras。
// 说明: 该函数避免重复查询，仅当缓存为空时才调用 VirtualCameraCapture::GetAvailableVirtualCameras。
// 函数: ensureVirtualCamerasLoaded
// 作用: 确保虚拟摄像头列表已加载到虚拟摄像头缓存中。
// 说明: 当程序支持虚拟摄像头输出（virtual camera）时，会在此列出系统中安装的虚拟摄像头
//      以供用户选择。如果缓存为空则执行枚举并填充 virtual_cameras。
void ensureVirtualCamerasLoaded()
{
    if (virtual_cameras.empty())
    {
        virtual_cameras = VirtualCameraCapture::GetAvailableVirtualCameras();
    }
}

void draw_model_path_settings()
{
    const auto models = getAvailableModels();
    if (models.empty())
    {
        ImGui::TextDisabled("未找到模型文件");
        return;
    }
    int selected = 0;
    for (int i = 0; i < static_cast<int>(models.size()); ++i)
        if (models[i] == config.ai_model) selected = i;
    std::vector<const char*> items;
    items.reserve(models.size());
    for (const auto& model : models) items.push_back(model.c_str());
    ImGui::SetNextItemWidth(UiLayout::kComboLongWidth);
    if (ImGui::Combo("模型文件", &selected, items.data(), static_cast<int>(items.size())))
    {
        config.ai_model = models[selected];
        detector_model_changed.store(true);
        OverlayConfig_MarkDirty();
    }
    ImGui::Text("模型路径: %s", config.ai_model.c_str());
    // 本项目仅支持英伟达 TRT 单后端，构建恒定义 USE_CUDA，推理后端恒为
    // CUDA TensorRT，此处硬编码与实现一致，无需读 config.backend。
    ImGui::TextUnformatted("推理后端: CUDA TensorRT");
    const StartupOnnxReport& report = startupOnnxReport();
    ImGui::TextUnformatted(report.success ? report.summary.c_str() : "智能推断，当前模型分辨率：未知，模型类别数量：未知，模型版本：未知");
}

void draw_capture_and_model_settings()
{
    draw_model_path_settings();
    draw_capture_general_settings();
    // Keep source-specific controls beside the capture method selector.
    draw_capture_source_settings();
    ImGui::Spacing();
    draw_stats_summary();
    const StartupOnnxReport& report = startupOnnxReport();
    if (report.success)
    {
        std::string reportText = report.text;
        ImGui::InputTextMultiline("##startup_onnx_report", &reportText,
            ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8.0f), ImGuiInputTextFlags_ReadOnly);
    }
}

void draw_performance_settings()
{
    draw_stats();

    ImGui::SeparatorText("ONNX 模型信息");

    if (ImGui::Button("选择.onnx文件"))
    {
        const std::string selectedPath = chooseOnnxFile();
        if (!selectedPath.empty())
        {
            onnx_inspection_path = selectedPath;
            onnx_inspection_text.clear();
            onnx_inspection_mode_queued = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("简洁信息"))
    {
        if (onnx_inspection_path.empty())
            onnx_inspection_text = kOnnxInspectionPrompt;
        else if (!onnx_inspection_running)
            startOnnxInspection(OnnxInspectionMode::Brief);
        else
        {
            onnx_inspection_queued_mode = OnnxInspectionMode::Brief;
            onnx_inspection_mode_queued = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("完整信息"))
    {
        if (onnx_inspection_path.empty())
            onnx_inspection_text = kOnnxInspectionPrompt;
        else if (!onnx_inspection_running)
            startOnnxInspection(OnnxInspectionMode::Full);
        else
        {
            onnx_inspection_queued_mode = OnnxInspectionMode::Full;
            onnx_inspection_mode_queued = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("复制内容"))
    {
        if (onnx_inspection_path.empty())
            onnx_inspection_text = kOnnxInspectionPrompt;
        else if (!onnx_inspection_running && !onnx_inspection_text.empty())
            ImGui::SetClipboardText(onnx_inspection_text.c_str());
    }
    if (onnx_inspection_running)
        ImGui::TextDisabled("解析中...");

    ImGui::TextUnformatted(onnx_inspection_path.empty()
        ? kOnnxInspectionPrompt
        : onnx_inspection_path.c_str());

    if (onnx_inspection_running &&
        onnx_inspection_task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
    {
        const bool taskMatchesSelectedPath = onnx_inspection_task_path == onnx_inspection_path;
        OnnxInspectionResult result;
        bool taskCompleted = false;
        try
        {
            result = onnx_inspection_task.get();
            taskCompleted = true;
        }
        catch (const std::exception& error)
        {
            onnx_inspection_text = std::string("读取模型信息时发生异常: ") + error.what();
        }
        catch (...)
        {
            onnx_inspection_text = "读取模型信息时发生未知异常。";
        }
        onnx_inspection_running = false;

        if (taskMatchesSelectedPath)
        {
            const OnnxInspectionMode mode = onnx_inspection_mode_queued
                ? onnx_inspection_queued_mode
                : onnx_inspection_running_mode;
            if (taskCompleted)
            {
                onnx_inspection_text = mode == OnnxInspectionMode::Brief
                    ? result.brief_text
                    : result.full_text;
            }
            onnx_inspection_mode_queued = false;
        }
        else if (onnx_inspection_mode_queued && !onnx_inspection_path.empty())
        {
            const OnnxInspectionMode mode = onnx_inspection_queued_mode;
            onnx_inspection_mode_queued = false;
            startOnnxInspection(mode);
        }
    }

    ImGui::InputTextMultiline(
        "##onnx_model_details", &onnx_inspection_text,
        ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8.0f), ImGuiInputTextFlags_ReadOnly);
}

void draw_capture_general_settings()
{
    ImGui::PushID("capture_section_general");
        ImGui::BeginGroup();
        const ImGuiStyle& style = ImGui::GetStyle();
        {
            static constexpr int kDetectionResolutions[] = {
                160, 192, 224, 256, 288, 320, 352, 384,
                416, 448, 480, 512, 544, 576, 608, 640
            };
            static constexpr const char* kDetectionResolutionLabels[] = {
                "160", "192", "224", "256", "288", "320", "352", "384",
                "416", "448", "480", "512", "544", "576", "608", "640"
            };
            int selectedDetectionResolution = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kDetectionResolutions); ++i)
            {
                if (kDetectionResolutions[i] == config.detection_resolution)
                {
                    selectedDetectionResolution = i;
                    break;
                }
            }
            ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth);
            const bool detectionResolutionChanged = ImGui::Combo(
                "##detection_resolution", &selectedDetectionResolution,
                kDetectionResolutionLabels, IM_ARRAYSIZE(kDetectionResolutionLabels));
            if (detectionResolutionChanged)
                config.detection_resolution = kDetectionResolutions[selectedDetectionResolution];
            ImGui::SameLine();
            ImGui::TextDisabled("模型分辨率 [160, 640]");
            ShowSettingTooltip("模型分辨率");
            if (detectionResolutionChanged)
            {
                detection_resolution_changed.store(true);
                detector_model_changed.store(true);

                globalMouseThread->updateConfig(
                    config.detection_resolution,
                    config.fovX,
                    config.fovY,
                    config.minSpeedMultiplier,
                    config.maxSpeedMultiplier,
                    config.predictionInterval,
                    config.auto_shoot,
                    config.bScope_multiplier);
                OverlayConfig_MarkDirty();
            }
        }

        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        bool captureFpsChanged = ImGui::InputInt("##capture_fps", &config.capture_fps, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); if (ImGui::Button("-##capture_fps")) { --config.capture_fps; captureFpsChanged = true; }
        ImGui::SameLine(); if (ImGui::Button("+##capture_fps")) { ++config.capture_fps; captureFpsChanged = true; }
        ImGui::SameLine(); ImGui::TextDisabled("采集帧率 [1, 240]");
        ShowSettingTooltip("采集帧率");
        if (captureFpsChanged)
        {
            capture_fps_changed.store(true);
            OverlayConfig_MarkDirty();
        }
        ValidateIntParam(&config.capture_fps, 1, 240, 60);

        if (config.capture_fps >= 241)
        {
            ImGui::TextDisabled("警告：高帧率可能降低性能。");
        }

        {
            if (ImGui::Checkbox("圆形视野", &config.circle_fov_enabled))
            {
                OverlayConfig_MarkDirty();
            }
        }
        ImGui::EndGroup();

        if (config.circle_fov_enabled)
        {
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool circleFovChanged = ImGui::InputInt("##circle_fov", &config.circle_fov_radius_percent, 0, 0, ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine(); if (ImGui::Button("-##circle_fov")) { --config.circle_fov_radius_percent; circleFovChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##circle_fov")) { ++config.circle_fov_radius_percent; circleFovChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("圆形视野大小 [1, 100]");
            if (circleFovChanged)
            {
                OverlayConfig_MarkDirty();
            }
            ValidateIntParam(&config.circle_fov_radius_percent, 1, 100, 100);
        }

        ImGui::BeginGroup();

#ifdef USE_CUDA
        if (config.backend == "TRT")
        {
            const bool cudaCaptureAvailable = (config.capture_method == "duplication_api");
            if (!cudaCaptureAvailable)
            {
                ImGui::BeginDisabled();
            }

            {
#ifdef USE_CUDA
                bool captureUseCuda = config.capture_use_cuda;
                if (ImGui::Checkbox("##cuda_capture", &captureUseCuda))
                {
                    config.capture_use_cuda = captureUseCuda;
                    OverlayConfig_MarkDirty();
                }
#else
                ImGui::Checkbox("##value", false);
#endif
                ImGui::SameLine();
                ImGui::TextUnformatted("CUDA 加速捕获");
            }

            if (!cudaCaptureAvailable)
            {
                ImGui::EndDisabled();
                ImGui::TextDisabled("仅在DXGI模式下可用。");
            }
        }
#endif

        std::vector<std::string> captureMethodOptions = { "duplication_api", "winrt", "virtual_camera", "udp_capture" };
        std::vector<std::string> captureMethodDisplayNames = { "DXGI", "WinRT", "采集卡", "UDP推流" };
        std::vector<const char*> captureMethodItems;

        for (const auto& name : captureMethodDisplayNames)
        {
            captureMethodItems.push_back(name.c_str());
        }

        int currentcaptureMethodIndex = 0;
        for (size_t i = 0; i < captureMethodOptions.size(); ++i)
        {
            if (captureMethodOptions[i] == config.capture_method)
            {
                currentcaptureMethodIndex = static_cast<int>(i);
                break;
            }
        }

        {
            ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth);
            if (ImGui::Combo("##capture_method", &currentcaptureMethodIndex, captureMethodItems.data(), static_cast<int>(captureMethodItems.size())))
            {
                config.capture_method = captureMethodOptions[currentcaptureMethodIndex];
                OverlayConfig_MarkDirty();
                if (config.capture_method == "virtual_camera")
                {
                    virtual_camera_settings_pending = true;
                }
                else
                {
                    capture_method_changed.store(true);
                    virtual_camera_settings_pending = false;
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("捕获方式");
        }
        ImGui::EndGroup();
    ImGui::PopID();

    draw_capture_preview();
}

void draw_capture_source_settings()
{
    if (config.capture_method == "winrt")
    {
        ImGui::PushID("capture_section_winrt");
        ImGui::SeparatorText("WinRT捕获");
            {
                std::vector<std::string> targetOptions = { "monitor", "window" };
                std::vector<std::string> targetDisplayNames = { "显示器", "窗口" };
                int currentTargetIndex = (config.capture_target == "window") ? 1 : 0;
                {
                    ImGui::TextUnformatted("捕获目标(WinRT)");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth);
                    if (ImGui::Combo("##winrt_capture_target", &currentTargetIndex,
                        [](void* data, int idx) -> const char* {
                            const auto* v = static_cast<const std::vector<std::string>*>(data);
                            if (idx < 0 || idx >= (int)v->size()) return nullptr;
                            return v->at(idx).c_str();
                        }, (void*)&targetDisplayNames, (int)targetDisplayNames.size()))
                    {
                        config.capture_target = targetOptions[currentTargetIndex];
                        OverlayConfig_MarkDirty();
                        requestWinrtCaptureRestart();
                    }
                }
            }

            if (config.capture_target == "window")
            {
                if (!capture_windows_loaded)
                    refreshCaptureWindowList();

                {
                    ImGui::TextUnformatted("窗口过滤");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(UiLayout::kTextMediumWidth);
                    ImGui::InputText("##winrt_window_filter", capture_window_filter_buf, IM_ARRAYSIZE(capture_window_filter_buf));
                }

                const std::string filterLower = OtherTools::ToLowerAscii(capture_window_filter_buf);
                std::vector<int> filteredWindowIndices;
                filteredWindowIndices.reserve(capture_windows.size());
                for (int i = 0; i < static_cast<int>(capture_windows.size()); ++i)
                {
                    const std::string displayLower = OtherTools::ToLowerAscii(capture_windows[i].displayName);
                    if (filterLower.empty() || displayLower.find(filterLower) != std::string::npos)
                        filteredWindowIndices.push_back(i);
                }

                if (!filteredWindowIndices.empty())
                {
                    const std::string preview = config.capture_window_title.empty()
                        ? std::string("选择窗口")
                        : config.capture_window_title;

                    ImGui::TextUnformatted("窗口");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(UiLayout::kComboLongWidth);
                    if (ImGui::BeginCombo("##winrt_window_pick", preview.c_str()))
                    {
                        for (int index : filteredWindowIndices)
                        {
                            const CaptureWindowInfo& window = capture_windows[index];
                            const bool selected = captureWindowMatchesTitle(window, config.capture_window_title);

                            ImGui::PushID(window.hwnd);
                            if (ImGui::Selectable(window.displayName.c_str(), selected))
                            {
                                applyWinrtWindowTarget(window.title);
                            }
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                }
                else
                {
                    ImGui::TextDisabled("没有匹配的窗口");
                }

                {
                    if (ImGui::Button("刷新", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
                    {
                        refreshCaptureWindowList();
                        if (currentWindowTitleIsInList())
                            requestWinrtCaptureRestart();
                    }
                }
            }

            if (disable_winrt_futures)
            {
                ImGui::BeginDisabled();
            }

            {
                if (ImGui::Checkbox("捕获窗口边框", &config.capture_borders))
                {
                    capture_borders_changed.store(true);
                    OverlayConfig_MarkDirty();
                }
            }

            {
                if (ImGui::Checkbox("捕获鼠标光标", &config.capture_cursor))
                {
                    capture_cursor_changed.store(true);
                    OverlayConfig_MarkDirty();
                }
            }

            if (disable_winrt_futures)
            {
                ImGui::EndDisabled();
            }

        ImGui::PopID();
    }

    if (config.capture_method == "duplication_api" || (config.capture_method == "winrt" && config.capture_target != "window"))
    {
        ImGui::PushID("capture_section_monitor");
        ImGui::SeparatorText("显示器捕获");
            std::vector<std::string> monitorNames;
            int monitorCount = monitors;
            if (monitorCount <= 0)
            {
                monitorNames.push_back("显示器 1");
                monitorCount = 1;
            }
            else
            {
                for (int i = 0; i < monitorCount; ++i)
                {
                    monitorNames.push_back("显示器 " + std::to_string(i + 1));
                }
            }

            std::vector<const char*> monitorItems;
            for (const auto& name : monitorNames)
            {
                monitorItems.push_back(name.c_str());
            }

            int selectedMonitor = std::clamp(config.monitor_idx, 0, monitorCount - 1);
                {
                    ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth);
                    if (ImGui::Combo("##value", &selectedMonitor, monitorItems.data(), static_cast<int>(monitorItems.size())))
                {
                    config.monitor_idx = selectedMonitor;
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
            }

        ImGui::PopID();
    }

    if (config.capture_method == "virtual_camera")
    {
        ImGui::PushID("capture_section_virtual_camera");
        ImGui::SeparatorText("采集卡");
            ensureVirtualCamerasLoaded();

            {
                ImGui::TextUnformatted("过滤");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(UiLayout::kTextMediumWidth);
                // 修复：与下方采集卡 Combo 原本共用 "##value" 导致 ID 冲突。
                ImGui::InputText("##vcam_filter", virtual_camera_filter_buf, IM_ARRAYSIZE(virtual_camera_filter_buf));
            }

            std::string filter_lower = OtherTools::ToLowerAscii(virtual_camera_filter_buf);

            std::vector<int> filtered_indices;
            for (int i = 0; i < static_cast<int>(virtual_cameras.size()); ++i)
            {
                std::string name_lower = OtherTools::ToLowerAscii(virtual_cameras[i]);
                if (filter_lower.empty() || name_lower.find(filter_lower) != std::string::npos)
                {
                    filtered_indices.push_back(i);
                }
            }

            if (!filtered_indices.empty())
            {
                int currentIndex = 0;
                for (int fi = 0; fi < static_cast<int>(filtered_indices.size()); ++fi)
                {
                    if (virtual_cameras[filtered_indices[fi]] == config.virtual_camera_name)
                    {
                        currentIndex = fi;
                        break;
                    }
                }

                std::vector<const char*> items;
                items.reserve(filtered_indices.size());
                for (int idx : filtered_indices)
                {
                    items.push_back(virtual_cameras[idx].c_str());
                }

                {
                    ImGui::TextUnformatted("采集卡");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(UiLayout::kComboLongWidth);
                    if (ImGui::Combo("##vcam_device", &currentIndex, items.data(), static_cast<int>(items.size())))
                    {
                        config.virtual_camera_name = virtual_cameras[filtered_indices[currentIndex]];
                        OverlayConfig_MarkDirty();
                        virtual_camera_settings_pending = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted("选择采集设备");
                }
            }
            else
            {
                ImGui::TextDisabled("没有匹配的采集卡");
            }

            {
                if (ImGui::Button("刷新", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
                {
                    VirtualCameraCapture::ClearCachedCameraList();
                    virtual_cameras = VirtualCameraCapture::GetAvailableVirtualCameras(true);
                    virtual_camera_filter_buf[0] = '\0';
                }
            }

            ImGui::BeginGroup();
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool virtualCameraWidthChanged = ImGui::InputInt("##virtual_camera_width", &config.virtual_camera_width, 0, 0, ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine(); if (ImGui::Button("-##virtual_camera_width")) { --config.virtual_camera_width; virtualCameraWidthChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##virtual_camera_width")) { ++config.virtual_camera_width; virtualCameraWidthChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("采集卡宽度 [1, 7680]");
            if (virtualCameraWidthChanged)
            {
                OverlayConfig_MarkDirty();
                virtual_camera_settings_pending = true;
            }
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool virtualCameraHeightChanged = ImGui::InputInt("##virtual_camera_height", &config.virtual_camera_heigth, 0, 0, ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine(); if (ImGui::Button("-##virtual_camera_height")) { --config.virtual_camera_heigth; virtualCameraHeightChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##virtual_camera_height")) { ++config.virtual_camera_heigth; virtualCameraHeightChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("采集卡高度 [1, 4320]");
            if (virtualCameraHeightChanged)
            {
                OverlayConfig_MarkDirty();
                virtual_camera_settings_pending = true;
            }
            ValidateIntParam(&config.virtual_camera_width, 1, 7680, 1920);
            ValidateIntParam(&config.virtual_camera_heigth, 1, 4320, 1080);
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool virtualCameraFpsChanged = ImGui::InputInt("##virtual_camera_fps", &config.virtual_camera_fps, 0, 0, ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine(); if (ImGui::Button("-##virtual_camera_fps")) { --config.virtual_camera_fps; virtualCameraFpsChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##virtual_camera_fps")) { ++config.virtual_camera_fps; virtualCameraFpsChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("采集卡帧率 [1, 240]");
            if (virtualCameraFpsChanged)
            {
                config.virtual_camera_fps = std::clamp(config.virtual_camera_fps, 1, 240);
                OverlayConfig_MarkDirty();
                virtual_camera_settings_pending = true;
            }
            if (ImGui::Button("应用采集卡设置", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
            {
                virtual_camera_apply_requested.store(true);
                capture_method_changed.store(true);
                virtual_camera_settings_pending = false;
            }
            if (virtual_camera_settings_pending)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("有未应用的更改");
            }
            ImGui::EndGroup();

        ImGui::PopID();
    }

    if (config.capture_method == "udp_capture")
    {
        ImGui::PushID("capture_section_udp");
        ImGui::SeparatorText("UDP捕获");
            if (!udp_settings_init)
            {
                ImGui::BeginGroup();
                {
                    memset(udp_ip_buf, 0, sizeof(udp_ip_buf));
                    std::string ip = config.udp_ip;
                    if (ip.size() >= sizeof(udp_ip_buf))
                        ip = ip.substr(0, sizeof(udp_ip_buf) - 1);
                    memcpy(udp_ip_buf, ip.c_str(), ip.size());
                    udp_port_buf = config.udp_port;
                    udp_settings_init = true;
                }
                ImGui::EndGroup();
            }
            const float udpPortButtonWidth = std::max(
                ImGui::CalcTextSize("-").x,
                ImGui::CalcTextSize("+").x) + ImGui::GetStyle().FramePadding.x * 2.0f;
            {
                ImGui::TextUnformatted("UDP IP地址（0.0.0.0 = 任意来源）");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(UiLayout::kTextShortWidth);
                ImGui::InputText("##value", udp_ip_buf, IM_ARRAYSIZE(udp_ip_buf));
            }
            {
                ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
                ImGui::InputInt("##udp_port", &udp_port_buf, 0, 0, ImGuiInputTextFlags_CharsDecimal);
                ImGui::SameLine();
                if (ImGui::Button("-##udp_port", ImVec2(udpPortButtonWidth, 0.0f)))
                    --udp_port_buf;
                ImGui::SameLine();
                if (ImGui::Button("+##udp_port", ImVec2(udpPortButtonWidth, 0.0f)))
                    ++udp_port_buf;
                ImGui::SameLine();
                ImGui::TextDisabled("UDP端口 [1, 65535]");
            }
            {
                if (ImGui::Button("应用UDP设置", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
                {
                    udp_port_buf = std::clamp(udp_port_buf, 1, 65535);
                    config.udp_ip = udp_ip_buf;
                    config.udp_port = udp_port_buf;
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
            }

        ImGui::PopID();
    }
}
