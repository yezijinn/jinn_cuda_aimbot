#pragma once

#ifdef USE_CUDA

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"
#include "tensorrt/trt_monitor.h"

namespace OverlayExportUI
{
struct PhaseSnapshot
{
    std::string name;
    int current = 0;
    int max = 0;
};

struct ProgressSnapshot
{
    std::vector<PhaseSnapshot> phases;
    float aggregate = 0.0f;
    int currentSteps = 0;
    int maxSteps = 0;
    double secondsSinceUpdate = 0.0;
    bool hasPhases = false;
    bool stale = false;
    bool cancelRequested = false;
};

inline std::string FitTextToWidth(const std::string& text, float maxWidth)
{
    if (maxWidth <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
    {
        return text;
    }

    static constexpr const char* suffix = "...";
    const float suffixWidth = ImGui::CalcTextSize(suffix).x;
    if (suffixWidth >= maxWidth)
    {
        return suffix;
    }

    std::string out = text;
    while (!out.empty())
    {
        out.pop_back();
        std::string candidate = out + suffix;
        if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth)
        {
            return candidate;
        }
    }
    return suffix;
}

inline ProgressSnapshot CaptureProgress()
{
    ProgressSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        snapshot.phases.reserve(gProgressPhases.size());
        for (const auto& [name, phase] : gProgressPhases)
        {
            PhaseSnapshot item;
            item.name = name;
            item.current = std::max(0, phase.current);
            item.max = std::max(0, phase.max);
            snapshot.currentSteps += item.current;
            snapshot.maxSteps += item.max;
            snapshot.phases.push_back(std::move(item));
        }
    }

    snapshot.hasPhases = !snapshot.phases.empty();
    snapshot.aggregate = snapshot.maxSteps > 0
        ? std::clamp(snapshot.currentSteps / static_cast<float>(snapshot.maxSteps), 0.0f, 1.0f)
        : 0.0f;

    const long long lastUpdate = gTrtExportLastUpdateMs.load();
    if (lastUpdate > 0)
    {
        snapshot.secondsSinceUpdate = (TrtNowMs() - lastUpdate) / 1000.0;
    }
    snapshot.stale = snapshot.secondsSinceUpdate > 45.0;
    snapshot.cancelRequested = gTrtExportCancelRequested.load();
    return snapshot;
}

inline void DrawPhaseRows(const ProgressSnapshot& snapshot, int maxRows)
{
    const int count = static_cast<int>(std::min<size_t>(snapshot.phases.size(), static_cast<size_t>(maxRows)));
    for (int i = 0; i < count; ++i)
    {
        const PhaseSnapshot& phase = snapshot.phases[i];
        const float fraction = phase.max > 0
            ? std::clamp(phase.current / static_cast<float>(phase.max), 0.0f, 1.0f)
            : 0.0f;

        char value[32] = {};
        if (phase.max > 0)
        {
            std::snprintf(value, sizeof(value), "%d/%d", phase.current, phase.max);
        }
        else
        {
            std::snprintf(value, sizeof(value), "%d", phase.current);
        }

        ImGui::TextUnformatted(FitTextToWidth(phase.name, ImGui::GetContentRegionAvail().x).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", value);
        ImGui::ProgressBar(phase.max > 0 ? fraction : -1.0f, ImVec2(-FLT_MIN, 0.0f));
    }

    if (static_cast<int>(snapshot.phases.size()) > maxRows)
    {
        const int hidden = static_cast<int>(snapshot.phases.size()) - maxRows;
        char text[64] = {};
        std::snprintf(text, sizeof(text), "%d more export phases running", hidden);
        ImGui::TextDisabled("%s", text);
    }
}

inline void DrawTensorRtExportPanel(const char* id, const char* title,
    const char* subtitle, const char* modelName, const char* cancelLabel)
{
    ProgressSnapshot snapshot = CaptureProgress();

    ImGui::PushID(id);
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##export_progress", ImVec2(width, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextUnformatted(title ? title : "TensorRT export");

    std::string detail;
    if (modelName && *modelName)
    {
        detail = modelName;
    }
    else if (subtitle && *subtitle)
    {
        detail = subtitle;
    }
    else
    {
        detail = "Building optimized engine";
    }
    ImGui::TextDisabled("%s", FitTextToWidth(detail, ImGui::GetContentRegionAvail().x).c_str());

    const char* stateText = snapshot.cancelRequested ? "Canceling" : (snapshot.stale ? "Working" : "Running");
    ImGui::TextDisabled("%s", stateText);
    char totalLabel[48] = {};
    if (snapshot.hasPhases && snapshot.maxSteps > 0)
    {
        std::snprintf(totalLabel, sizeof(totalLabel), "%d%%", static_cast<int>(std::round(snapshot.aggregate * 100.0f)));
    }
    else
    {
        std::snprintf(totalLabel, sizeof(totalLabel), "Preparing");
    }
    ImGui::Text("Overall progress: %s", totalLabel);
    ImGui::ProgressBar(snapshot.hasPhases ? snapshot.aggregate : -1.0f, ImVec2(-FLT_MIN, 0.0f));

    if (snapshot.stale)
    {
        const char* warning = "TensorRT may spend a long time selecting tactics without reporting new steps.";
        ImGui::TextWrapped("%s", warning);
    }

    if (snapshot.hasPhases)
    {
        DrawPhaseRows(snapshot, 5);
    }
    else
    {
        ImGui::TextDisabled("%s", subtitle && *subtitle ? subtitle : "Waiting for TensorRT to report build phases");
    }

    if (snapshot.cancelRequested)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(cancelLabel ? cancelLabel : "Cancel export"))
    {
        gTrtExportCancelRequested = true;
    }
    if (snapshot.cancelRequested)
    {
        ImGui::EndDisabled();
    }
    char updated[64] = {};
    std::snprintf(updated, sizeof(updated), "Last update %.1f s ago", snapshot.secondsSinceUpdate);
    ImGui::TextDisabled("%s", updated);
    ImGui::EndChild();
    ImGui::PopID();
}
}

#endif
