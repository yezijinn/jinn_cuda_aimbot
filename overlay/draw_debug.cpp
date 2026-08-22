#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstddef>

#include "imgui/imgui.h"
#include "scr/data_collector.h"
#include "mybot.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "include/other_tools.h"
#include "capture.h"
#include "overlay/ui_sections.h"
#include "runtime/thread_loops.h"
#include "mouse/AimbotTarget.h"

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p)       \
    do {                      \
        if ((p) != nullptr) { \
            (p)->Release();   \
            (p) = nullptr;    \
        }                     \
    } while (0)
#endif

int prev_screenshot_delay = 0;
bool prev_verbose = false;
static bool debug_state_initialized = false;

static ID3D11Texture2D* g_debugTex = nullptr;
static ID3D11ShaderResourceView* g_debugSRV = nullptr;
static int texW = 0, texH = 0;

static ID3D11Texture2D* g_maskTex = nullptr;
static ID3D11ShaderResourceView* g_maskSRV = nullptr;
static int maskTexW = 0, maskTexH = 0;

static float debug_scale = 1.0f;
static int debug_preview_hotkey_slot = 0;
static char g_collectOutputDirBuffer[512] = {};
static std::string g_collectOutputDirMirror;
static char g_collectClassFilterBuffer[256] = {};
static std::string g_collectClassFilterMirror;

static std::string getFormattedCompileTime()
{
    char month[4] = {};
    int day = 0;
    int year = 0;
    if (sscanf_s(__DATE__, "%3s %d %d", month, static_cast<unsigned>(_countof(month)), &day, &year) != 3)
        return std::string(__DATE__) + " " + __TIME__;

    static const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int monthNumber = 0;
    for (int i = 0; i < 12; ++i)
    {
        if (std::strcmp(month, months[i]) == 0)
        {
            monthNumber = i + 1;
            break;
        }
    }

    char formatted[64] = {};
    if (monthNumber == 0)
        return std::string(__DATE__) + " " + __TIME__;

    std::snprintf(formatted, sizeof(formatted), "%d年%d月%d日%s", year, monthNumber, day, __TIME__);
    return formatted;
}

static void syncDebugTextBuffer(char* buffer, size_t buffer_size, std::string& mirror, const std::string& value)
{
    if (mirror == value)
        return;

    std::snprintf(buffer, buffer_size, "%s", value.c_str());
    buffer[buffer_size - 1] = '\0';
    mirror = value;
}

static bool applyDebugTextBuffer(std::string& target, std::string& mirror, const char* buffer)
{
    const std::string value = buffer ? std::string(buffer) : std::string();
    if (target == value && mirror == value)
        return false;

    target = value;
    mirror = value;
    return true;
}

static int findDebugKeyIndexByName(const std::string& keyName)
{
    for (size_t k = 0; k < key_names.size(); ++k)
    {
        if (key_names[k] == keyName)
            return static_cast<int>(k);
    }
    return 0;
}

static bool drawScreenshotButtonRows()
{
    if (key_names_cstrs.empty())
    {
        ImGui::TextDisabled("无可用的按键列表。");
        return false;
    }

    bool changed = false;
    if (config.screenshot_button.empty())
    {
        config.screenshot_button.push_back("None");
        changed = true;
    }

    for (size_t i = 0; i < config.screenshot_button.size();)
    {
        std::string& currentKeyName = config.screenshot_button[i];
        int currentIndex = findDebugKeyIndexByName(currentKeyName);
        const std::string rowLabel = (config.screenshot_button.size() > 1)
            ? "截图 " + std::to_string(i + 1)
            : "截图";

        ImGui::PushID(static_cast<int>(i));

        ImGui::TextUnformatted(rowLabel.c_str());
        ImGui::SameLine();
        const float actionBtnW = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth);

        if (ImGui::Combo("##value", &currentIndex, key_display_names_cstrs.data(), static_cast<int>(key_display_names_cstrs.size())))
        {
            currentKeyName = key_names[currentIndex];
            changed = true;
        }

        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("+", ImVec2(actionBtnW, 0.0f)))
        {
            config.screenshot_button.insert(config.screenshot_button.begin() + static_cast<std::vector<std::string>::difference_type>(i + 1), "None");
            changed = true;
        }

        ImGui::SameLine(0.0f, 3.0f);
        bool removedCurrent = false;
        if (ImGui::Button("-", ImVec2(actionBtnW, 0.0f)))
        {
            if (config.screenshot_button.size() <= 1)
            {
                config.screenshot_button[0] = "None";
            }
            else
            {
                config.screenshot_button.erase(config.screenshot_button.begin() + static_cast<std::vector<std::string>::difference_type>(i));
                removedCurrent = true;
            }
            changed = true;
        }

        ShowSettingTooltip(rowLabel.c_str());
        ImGui::PopID();

        if (removedCurrent)
            continue;

        ++i;
    }

    return changed;
}

static void uploadDebugFrame(const cv::Mat& bgr)
{
    if (bgr.empty()) return;

    if (!g_debugTex || bgr.cols != texW || bgr.rows != texH)
    {
        SAFE_RELEASE(g_debugTex);
        SAFE_RELEASE(g_debugSRV);

        texW = bgr.cols;  texH = bgr.rows;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = texW;
        td.Height = texH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        // 修复：必须检查 HRESULT。创建失败时 g_debugTex 保持 nullptr，
        // 若继续执行下方 Map(nullptr, ...) 将触发 D3D11 未定义行为/崩溃。
        if (FAILED(g_pd3dDevice->CreateTexture2D(&td, nullptr, &g_debugTex)))
        {
            g_debugTex = nullptr;
            texW = texH = 0;
            return;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(g_pd3dDevice->CreateShaderResourceView(g_debugTex, &sd, &g_debugSRV)))
        {
            SAFE_RELEASE(g_debugTex);
            g_debugSRV = nullptr;
            texW = texH = 0;
            return;
        }
    }

    if (!g_debugTex || !g_debugSRV)
        return;

    // 修复：采集源可能是 4 通道(BGRA)或 1 通道(灰度)，直接按 BGR2RGBA 转换会抛
    // cv::Exception 并逃逸到 UI 线程（无 try/catch）导致进程终止。按通道数分派。
    static cv::Mat rgba;
    switch (bgr.channels())
    {
    case 3: cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA); break;
    case 4: cv::cvtColor(bgr, rgba, cv::COLOR_BGRA2RGBA); break;
    case 1: cv::cvtColor(bgr, rgba, cv::COLOR_GRAY2RGBA); break;
    default: return;
    }

    if (rgba.cols != texW || rgba.rows != texH || rgba.type() != CV_8UC4)
        return;

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_pd3dDeviceContext->Map(g_debugTex, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        for (int y = 0; y < texH; ++y)
            memcpy((uint8_t*)ms.pData + ms.RowPitch * y,
                rgba.ptr(y), static_cast<size_t>(texW) * 4);
        g_pd3dDeviceContext->Unmap(g_debugTex, 0);
    }
}

static void uploadMaskFrame(const cv::Mat& rgba)
{
    if (rgba.empty()) return;

    if (!g_maskTex || rgba.cols != maskTexW || rgba.rows != maskTexH)
    {
        SAFE_RELEASE(g_maskTex);
        SAFE_RELEASE(g_maskSRV);

        maskTexW = rgba.cols;
        maskTexH = rgba.rows;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = maskTexW;
        td.Height = maskTexH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(g_pd3dDevice->CreateTexture2D(&td, nullptr, &g_maskTex)))
        {
            g_maskTex = nullptr;
            maskTexW = maskTexH = 0;
            return;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(g_pd3dDevice->CreateShaderResourceView(g_maskTex, &sd, &g_maskSRV)))
        {
            SAFE_RELEASE(g_maskTex);
            g_maskSRV = nullptr;
            maskTexW = maskTexH = 0;
            return;
        }
    }

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_pd3dDeviceContext->Map(g_maskTex, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        for (int y = 0; y < maskTexH; ++y)
            memcpy((uint8_t*)ms.pData + ms.RowPitch * y,
                rgba.ptr(y), maskTexW * 4);
        g_pd3dDeviceContext->Unmap(g_maskTex, 0);
    }
}

static bool drawDataCollectionSection()
{
    syncDebugTextBuffer(g_collectOutputDirBuffer, sizeof(g_collectOutputDirBuffer), g_collectOutputDirMirror, config.collect_output_dir);
    syncDebugTextBuffer(g_collectClassFilterBuffer, sizeof(g_collectClassFilterBuffer), g_collectClassFilterMirror, config.auto_label_record_classes);

    bool changed = false;

    ImGui::PushID("debug_section_data_collection");

    ImGui::SeparatorText("截图保存");

    // 第一行：独占
    changed |= ImGui::Checkbox("游戏截图总开关", &config.collect_data_while_playing);

    // 第二行：两个并排
    changed |= ImGui::Checkbox("仅自瞄运行时截图", &config.collect_only_when_aimbot_running);

    ImGui::SameLine();

    changed |= ImGui::Checkbox("仅出现目标时截图", &config.collect_only_when_targets_present);

    //ImGui::PopID();

    int saveEveryNFrames = config.collect_save_every_n_frames;
    const float collectionNumericButtonWidth = std::max(
        ImGui::CalcTextSize("-").x,
        ImGui::CalcTextSize("+").x) + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
    bool saveEveryNFramesChanged = ImGui::InputInt("##save_every_n_frames", &saveEveryNFrames, 0, 0, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(); if (ImGui::Button("-##save_every_n_frames")) { --saveEveryNFrames; saveEveryNFramesChanged = true; }
    ImGui::SameLine(); if (ImGui::Button("+##save_every_n_frames")) { ++saveEveryNFrames; saveEveryNFramesChanged = true; }
    ImGui::SameLine(); ImGui::TextDisabled("每 N 帧保存 [1, 600]");
    ValidateIntParam(&saveEveryNFrames, 1, 600, 300);
    if (saveEveryNFramesChanged || config.collect_save_every_n_frames != saveEveryNFrames)
    {
        config.collect_save_every_n_frames = saveEveryNFrames;
        changed = true;
    }

    int jpegQuality = config.collect_jpeg_quality;
    ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
    bool jpegQualityChanged = ImGui::InputInt("##jpeg_quality", &jpegQuality, 0, 0, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(); if (ImGui::Button("-##jpeg_quality")) { --jpegQuality; jpegQualityChanged = true; }
    ImGui::SameLine(); if (ImGui::Button("+##jpeg_quality")) { ++jpegQuality; jpegQualityChanged = true; }
    ImGui::SameLine(); ImGui::TextDisabled("图片质量 [50, 100]");
    ValidateIntParam(&jpegQuality, 50, 100, 100);
    if (jpegQualityChanged || config.collect_jpeg_quality != jpegQuality)
    {
        config.collect_jpeg_quality = jpegQuality;
        changed = true;
    }

    ImGui::SetNextItemWidth(UiLayout::kTextMediumWidth);
    if (ImGui::InputText("保存目录 (可留空 默认使用程序screenshots目录)", g_collectOutputDirBuffer, sizeof(g_collectOutputDirBuffer)))
        changed |= applyDebugTextBuffer(config.collect_output_dir, g_collectOutputDirMirror, g_collectOutputDirBuffer);

    ImGui::PushID("自动标注");
    ImGui::SeparatorText("自动标注");
    changed |= ImGui::Checkbox("自动生成标注.txt", &config.auto_label_data);

        ImGui::BeginDisabled(!config.auto_label_data);

        float minConf = config.auto_label_min_conf;
        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        bool minConfChanged = ImGui::InputFloat("##auto_label_min_conf", &minConf, 0.0f, 0.0f, "%.4f", ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); if (ImGui::Button("-##auto_label_min_conf")) { minConf -= 0.01f; minConfChanged = true; }
        ImGui::SameLine(); if (ImGui::Button("+##auto_label_min_conf")) { minConf += 0.01f; minConfChanged = true; }
        ImGui::SameLine(); ImGui::TextDisabled("自动标注最低置信度 [0.01, 0.99]");
        ShowSettingTooltip("自动标注最低置信度");
        ValidateFloatParam(&minConf, 0.01f, 0.99f, 0.30f);
        if (minConfChanged || config.auto_label_min_conf != minConf)
        {
            config.auto_label_min_conf = minConf;
            changed = true;
        }

        int maxBoxes = config.auto_label_max_boxes;
        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        bool maxBoxesChanged = ImGui::InputInt("##auto_label_max_boxes", &maxBoxes, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); if (ImGui::Button("-##auto_label_max_boxes")) { --maxBoxes; maxBoxesChanged = true; }
        ImGui::SameLine(); if (ImGui::Button("+##auto_label_max_boxes")) { ++maxBoxes; maxBoxesChanged = true; }
        ImGui::SameLine(); ImGui::TextDisabled("自动标注最大框数 [1, 200]");
        ShowSettingTooltip("自动标注最大框数");
        ValidateIntParam(&maxBoxes, 1, 200, 20);
        if (maxBoxesChanged || config.auto_label_max_boxes != maxBoxes)
        {
            config.auto_label_max_boxes = maxBoxes;
            changed = true;
        }

        ImGui::SetNextItemWidth(UiLayout::kTextMediumWidth);
        if (ImGui::InputText("想要标注的类别 (可留空 默认标注所有类别)", g_collectClassFilterBuffer, sizeof(g_collectClassFilterBuffer)))
            changed |= applyDebugTextBuffer(config.auto_label_record_classes, g_collectClassFilterMirror, g_collectClassFilterBuffer);

        ImGui::TextDisabled("自己填类别 使用英文逗号分隔 如 0,1,2  禁止用中文逗号");
        ImGui::EndDisabled();

    ImGui::PopID();

    const cvm::DataCollectionUiState ui = cvm::GetDataCollectionUiState("", config.ai_model.c_str(), config);
    ImGui::Separator();
    ImGui::Text("帧数记录: %llu", static_cast<unsigned long long>(ui.observed_frame_count));
    ImGui::SameLine();

    ImGui::Text("保存动作: %llu", static_cast<unsigned long long>(ui.attempted_sample_count));
    ImGui::SameLine();

    ImGui::Text("已存图片: %llu", static_cast<unsigned long long>(ui.saved_image_count));
    ImGui::SameLine();

    ImGui::Text("标注文档: %llu", static_cast<unsigned long long>(ui.saved_label_count));
    ImGui::TextWrapped("保存目录: %s", ui.resolved_output_dir.c_str());
    if (!ui.status.empty())
        ImGui::TextWrapped("状态: %s", ui.status.c_str());
    else
        ImGui::TextDisabled("状态: 空闲");

    ImGui::PushID("copy_resolved_path");
    if (ImGui::Button("复制全路径", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
        ImGui::SetClipboardText(ui.resolved_output_dir.c_str());
    ImGui::PopID();

    ImGui::PushID("reset_collect_counters");
    if (ImGui::Button("重置计数值", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
        cvm::ResetDataCollectionRuntime();
    ImGui::PopID();

    ImGui::PopID();
    return changed;
}

void draw_debug_frame()
{
    cv::Mat frameCopy;
    {
        std::lock_guard<std::mutex> lk(frameMutex);
        if (!latestFrame.empty())
            latestFrame.copyTo(frameCopy);
    }

    uploadDebugFrame(frameCopy);

    if (!g_debugSRV) return;

    {
        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        ImGui::InputFloat("##debug_scale", &debug_scale, 0.0f, 0.0f, "%.4f", ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); if (ImGui::Button("-##debug_scale")) debug_scale -= 0.1f;
        ImGui::SameLine(); if (ImGui::Button("+##debug_scale")) debug_scale += 0.1f;
        ImGui::SameLine(); ImGui::TextDisabled("调试缩放 [0.10, 3.00]");
        ValidateFloatParam(&debug_scale, 0.10f, 3.00f, 1.00f);
    }

    {
        const char* previewHotkeyLabels[] = { "热键1", "热键2", "热键3" };
        ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth);
        ImGui::Combo("你想预览哪个热键的目标？", &debug_preview_hotkey_slot, previewHotkeyLabels,
            static_cast<int>(Config::MAX_MOUSE_HOTKEYS));
    }

    debug_preview_hotkey_slot = std::clamp(
        debug_preview_hotkey_slot,
        0,
        static_cast<int>(Config::MAX_MOUSE_HOTKEYS) - 1);
    const auto& previewProfile = config.mouse_hotkeys[static_cast<std::size_t>(debug_preview_hotkey_slot)];
    const bool previewTriggerEnabled =
        previewProfile.localBool("trigger_enabled", config.trigger_targeting.enabled) &&
        previewProfile.localBool("trigger_enabled_for_hotkey", true);

    ImVec2 image_size(texW * debug_scale, texH * debug_scale);
    ImGui::Image((ImTextureID)(intptr_t)g_debugSRV, image_size);

    ImVec2 image_pos = ImGui::GetItemRectMin();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    {
        // 类别颜色调色板（10 色，暗色背景高对比度）
        static const ImU32 kClassColors[] = {
            IM_COL32(255,  80,  80, 255),  // 0 红
            IM_COL32( 80, 220,  80, 255),  // 1 绿
            IM_COL32( 80, 160, 255, 255),  // 2 蓝
            IM_COL32(255, 220,  60, 255),  // 3 黄
            IM_COL32(220,  80, 220, 255),  // 4 紫
            IM_COL32( 60, 230, 230, 255),  // 5 青
            IM_COL32(255, 160,  60, 255),  // 6 橙
            IM_COL32(200, 200, 200, 255),  // 7 灰白
            IM_COL32(160, 220, 100, 255),  // 8 黄绿
            IM_COL32(255, 120, 160, 255),  // 9 粉红
        };
        constexpr int kClassColorCount = sizeof(kClassColors) / sizeof(kClassColors[0]);

        std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
        for (size_t i = 0; i < detectionBuffer.boxes.size(); ++i)
        {
            const cv::Rect& box = detectionBuffer.boxes[i];

            ImVec2 p1(image_pos.x + box.x * debug_scale,
                image_pos.y + box.y * debug_scale);
            ImVec2 p2(p1.x + box.width * debug_scale,
                p1.y + box.height * debug_scale);

            // 按类别选择颜色
            int classId = 0;
            if (i < detectionBuffer.classes.size())
                classId = detectionBuffer.classes[i];
        if (classId < 0 || classId >= Config::FIXED_TARGET_CLASS_COUNT ||
                !config.isClassEnabled(classId) ||
                !previewProfile.localBool(targetClassConfigKeys().enabled[classId], false))
            {
                continue;
            }
            ImU32 color = kClassColors[classId % kClassColorCount];

            draw_list->AddRect(p1, p2, color, 0.0f, 2.0f);

            // 标签: 类别 + 置信度
            char label[64];
            float conf = 0.0f;
            if (i < detectionBuffer.confidences.size())
                conf = detectionBuffer.confidences[i];

            snprintf(label, sizeof(label), "%d: %.2f", classId, conf);
            draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.5f,
                ImVec2(p1.x, p1.y - 22), color, label);

            const TargetClassConfigKeys& classKeys = targetClassConfigKeys();
            const float aimOffsetX = std::clamp(
                previewProfile.localFloat(classKeys.aimOffsetX[classId], 0.5f),
                0.0f,
                1.0f);
            const float aimOffsetY = std::clamp(
                previewProfile.localFloat(classKeys.aimOffsetY[classId], 0.5f),
                0.0f,
                1.0f);
            float aimPivotX = box.x + box.width * aimOffsetX;
            float aimPivotY = box.y + box.height * aimOffsetY;
            ImVec2 aimCenter(image_pos.x + aimPivotX * debug_scale,
                             image_pos.y + aimPivotY * debug_scale);
            draw_list->AddCircleFilled(aimCenter, 5.0f * debug_scale, IM_COL32(0, 255, 0, 255));

            if (previewTriggerEnabled)
            {
                const float zoneOffsetX = std::clamp(
                    previewProfile.localFloat(classKeys.triggerZoneOffsetX[classId], 0.1f), 0.0f, 1.0f);
                const float zoneOffsetY = std::clamp(
                    previewProfile.localFloat(classKeys.triggerZoneOffsetY[classId], 0.1f), 0.0f, 1.0f);
                const float zoneSizeX = std::clamp(
                    previewProfile.localFloat(classKeys.triggerZoneSizeX[classId], 0.8f), 0.01f, 1.0f);
                const float zoneSizeY = std::clamp(
                    previewProfile.localFloat(classKeys.triggerZoneSizeY[classId], 0.8f), 0.01f, 1.0f);
                const float tzX = box.x + box.width * zoneOffsetX;
                const float tzY = box.y + box.height * zoneOffsetY;
                const float tzW = std::min(box.width * zoneSizeX, box.x + box.width - tzX);
                const float tzH = std::min(box.height * zoneSizeY, box.y + box.height - tzY);

                ImVec2 sp1(image_pos.x + tzX * debug_scale,
                           image_pos.y + tzY * debug_scale);
                ImVec2 sp2(sp1.x + tzW * debug_scale,
                           sp1.y + tzH * debug_scale);

                draw_list->AddRectFilled(sp1, sp2, IM_COL32(255, 220, 30, 45));
                draw_list->AddRect(sp1, sp2, IM_COL32(255, 220, 30, 255), 0.0f, 0, 3.0f);
            }
        }
    }

    // —— 绘制瞄准触发区域（动态范围圆，瞄准槽位专用） ——
    {
        float imageCenterX = image_pos.x + texW * debug_scale * 0.5f;
        float imageCenterY = image_pos.y + texH * debug_scale * 0.5f;

        // 检测画布外接圆（半透明，作为视野参考）
        float originalRadius = (texW * debug_scale) * 0.5f;
        draw_list->AddCircle(
            ImVec2(imageCenterX, imageCenterY),
            originalRadius,
            IM_COL32(0, 220, 80, 80),
            64, 1.0f);

        // 动态范围圆（瞄准触发区域）：根据 dynamic_range_enabled 实时调整
        // 修复：config.fovX 为 int 且 loadConfig 不做范围校验，手改 config.ini 为 0
        // 会导致除零 -> aimRadius = inf/NaN -> AddCircle 收到 NaN 半径、
        // static_cast<int>(inf) 属未定义行为。此处对分母取下限 1。
        const double baseFovX = static_cast<double>(config.fovX) > 0.0
            ? static_cast<double>(config.fovX)
            : 1.0;
        double effectiveFov = g_dynamicEffectiveFov.load();
        if (!config.dynamic_range_enabled || !(effectiveFov > 0.0))
            effectiveFov = baseFovX;
        float aimRadius = originalRadius * static_cast<float>(effectiveFov / baseFovX);
        if (!std::isfinite(aimRadius) || aimRadius < 0.0f)
            aimRadius = originalRadius;

        ImU32 aimColor = config.dynamic_range_enabled
            ? IM_COL32(255, 160, 40, 220)   // 启用动态范围：橙色
            : IM_COL32(255, 220, 30, 220); // 未启用：黄色
        draw_list->AddCircle(
            ImVec2(imageCenterX, imageCenterY),
            aimRadius,
            aimColor,
            64, 2.0f);

        char aimLbl[48];
        const float aimScale = (debug_scale > 0.0f) ? debug_scale : 1.0f;
        const float aimDiameterF = std::clamp(aimRadius * 2.0f / aimScale, 0.0f, 1.0e6f);
        int aimDiameter = static_cast<int>(aimDiameterF);
        snprintf(aimLbl, sizeof(aimLbl),
            config.dynamic_range_enabled ? "瞄准范围 %dpx" : "瞄准范围 %dpx",
            aimDiameter);
        draw_list->AddText(
            ImVec2(imageCenterX + aimRadius * 0.55f, imageCenterY - aimRadius - 4.0f),
            aimColor, aimLbl);

        // 十字准心
        float crossLen = 10.0f * debug_scale;
        draw_list->AddLine(
            ImVec2(imageCenterX - crossLen, imageCenterY),
            ImVec2(imageCenterX + crossLen, imageCenterY),
            IM_COL32(255, 255, 255, 120), 1.5f);
        draw_list->AddLine(
            ImVec2(imageCenterX, imageCenterY - crossLen),
            ImVec2(imageCenterX, imageCenterY + crossLen),
            IM_COL32(255, 255, 255, 120), 1.5f);
    }

    if (config.draw_futurePositions && globalMouseThread)
    {
        auto futurePts = globalMouseThread->getFuturePositions();
        if (!futurePts.empty())
        {
            // 修复：detection_resolution 来自配置文件，为 0 时除零产生 inf，
            // 后续 static_cast<int>(inf) 属未定义行为。取下限 1。
            const float detRes = (config.detection_resolution > 0)
                ? static_cast<float>(config.detection_resolution)
                : 1.0f;
            float scale_x = static_cast<float>(texW) / detRes;
            float scale_y = static_cast<float>(texH) / detRes;

            ImVec2 clip_min = image_pos;
            ImVec2 clip_max = ImVec2(image_pos.x + texW * debug_scale,
                image_pos.y + texH * debug_scale);
            draw_list->PushClipRect(clip_min, clip_max, true);

            int totalPts = static_cast<int>(futurePts.size());
            for (size_t i = 0; i < futurePts.size(); ++i)
            {
                int px = static_cast<int>(futurePts[i].first * scale_x);
                int py = static_cast<int>(futurePts[i].second * scale_y);
                ImVec2 pt(image_pos.x + px * debug_scale,
                    image_pos.y + py * debug_scale);

                int b = static_cast<int>(255 - (i * 255.0 / totalPts));
                int r = static_cast<int>(i * 255.0 / totalPts);
                int g = 50;

                ImU32 fillColor = IM_COL32(r, g, b, 255);
                ImU32 outlineColor = IM_COL32(255, 255, 255, 255);

                draw_list->AddCircleFilled(pt, 4.0f * debug_scale, fillColor);
                draw_list->AddCircle(pt, 4.0f * debug_scale, outlineColor, 0, 1.0f);
            }

            draw_list->PopClipRect();
        }
    }
}

void draw_capture_preview()
{
    ImGui::PushID("capture_section_preview");
    {
            if (ImGui::Checkbox("显示预览窗口 仅测试时开启 正常使用要关闭", &config.show_window))
            {
                OverlayConfig_MarkDirty();
            }
            ShowSettingTooltip("显示预览窗口 仅测试时开启 正常使用要关闭");
    }

        if (config.show_window)
        {
            draw_debug_frame();
        }

    ImGui::PopID();
}

void draw_debug()
{
    bool changed = false;

    // 首帧惰性同步：config 在 mybot.cpp 中加载，跨翻译单元的静态初始化顺序未定义，
    // 不能在全局初始化阶段读取。首帧绘制时配置已就绪，此时同步不会误判为"用户已修改"。
    if (!debug_state_initialized)
    {
        prev_screenshot_delay = config.screenshot_delay;
        prev_verbose = config.verbose;
        debug_state_initialized = true;
    }

    ImGui::PushID("debug_section_screenshot_buttons");
    ImGui::SeparatorText("截图按钮");
        if (drawScreenshotButtonRows())
            changed = true;

        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        bool screenshotDelayChanged = ImGui::InputInt("##screenshot_delay", &config.screenshot_delay, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); if (ImGui::Button("-##screenshot_delay")) { config.screenshot_delay -= 50; screenshotDelayChanged = true; }
        ImGui::SameLine(); if (ImGui::Button("+##screenshot_delay")) { config.screenshot_delay += 50; screenshotDelayChanged = true; }
        ImGui::SameLine(); ImGui::TextDisabled("截图延迟 [0, inf)");
        ShowSettingTooltip("截图延迟");
        if (screenshotDelayChanged)
            changed = true;
        if (ImGui::Checkbox("详细控制台输出", &config.verbose))
            changed = true;
        ImGui::EndGroup();

        if (config.screenshot_delay < 0)
            config.screenshot_delay = 0;

        ImGui::PushID("button_cv2_build_info");
        if (ImGui::Button("打印构建信息", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
        {
            std::cout << cv::getBuildInformation() << std::endl;
        }

        ImGui::PopID();
    ImGui::PopID();

    changed |= drawDataCollectionSection();

    // 修复：__DATE__/__TIME__ 为编译期常量，原实现每帧执行 sscanf_s + snprintf +
    // std::string 构造。改为函数内 static 只计算一次。
    static const std::string kCompileTimeText = getFormattedCompileTime();
    ImGui::PushFont(g_debugBoldFont, ImGui::GetStyle().FontSizeBase * 2.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "编译时间 %s", kCompileTimeText.c_str());
    ImGui::PopFont();

    if (prev_screenshot_delay != config.screenshot_delay ||
        prev_verbose != config.verbose)
    {
        prev_screenshot_delay = config.screenshot_delay;
        prev_verbose = config.verbose;
        changed = true;
    }

    if (changed)
    {
        OverlayConfig_MarkDirty();
    }
}
