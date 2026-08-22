#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "d3d11.h"
#include "imgui/imgui.h"

#include <cmath>
#include <iostream>
#include <mutex>
#include <vector>

#include "overlay.h"
#include "overlay/config_dirty.h"
#include "draw_settings.h"
#include "overlay/ui_sections.h"
#include "mybot.h"
#include "runtime/thread_loops.h"

bool prev_tracker_overlay_table_enabled = config.tracker_overlay_table_enabled;

void draw_tracker()
{
    bool changed = false;
    std::vector<TrackDebugInfo> tracks;
    int lockedTrackId = -1;
    {
        std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
        tracks = g_trackerDebugTracks;
        lockedTrackId = g_trackerLockedId;
    }

    ImGui::PushID("tracker_section_status");
    ImGui::SeparatorText("状态");
    ImGui::BeginGroup();
    const float trackerCheckboxWidth = ImGui::GetFrameHeight() * 2.0f +
        ImGui::GetStyle().ItemInnerSpacing.x * 2.0f + ImGui::CalcTextSize("启用追踪器").x +
        ImGui::CalcTextSize("显示目标表格").x + ImGui::GetStyle().ItemSpacing.x;
    const bool fitTrackerCheckboxes = ImGui::GetContentRegionAvail().x >= trackerCheckboxWidth;
    changed |= ImGui::Checkbox("启用追踪器", &config.tracker_enabled);
    ShowSettingTooltip("启用追踪器");
    if (fitTrackerCheckboxes)
        ImGui::SameLine();
    changed |= ImGui::Checkbox("显示目标表格", &config.tracker_overlay_table_enabled);
    ShowSettingTooltip("显示目标表格");
    ImGui::EndGroup();
        ImGui::Text("模式: 简单锁定");
        ImGui::Text("运行时: %s", config.tracker_enabled ? "追踪器" : "最近目标");
        ImGui::Text("锁定目标ID: %d", lockedTrackId);
        ImGui::Text("活跃目标数: %d", static_cast<int>(tracks.size()));
    ImGui::PopID();

    if (config.tracker_overlay_table_enabled)
    {
        ImGui::PushID("tracker_section_tracks");
        ImGui::SeparatorText("追踪列表");
        if (tracks.empty())
        {
            ImGui::TextDisabled("无活跃追踪");
        }
        else if (ImGui::BeginTable("tracker_tracks_table", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("类别");
            ImGui::TableSetupColumn("已锁定");
            ImGui::TableSetupColumn("已观察");
            ImGui::TableSetupColumn("已丢失");
            ImGui::TableSetupColumn("枢轴");
            ImGui::TableSetupColumn("速度");
            ImGui::TableHeadersRow();

            for (const auto& track : tracks)
            {
                const double speed = std::hypot(track.velocityX, track.velocityY);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", track.trackId);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", track.classId);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", track.isLocked ? "Yes" : "No");
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", track.observedThisFrame ? "Yes" : "No");
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%d", track.missedFrames);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%.0f, %.0f", track.pivotX, track.pivotY);
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%.0f", speed);
            }

            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    if (changed ||
        prev_tracker_overlay_table_enabled != config.tracker_overlay_table_enabled)
    {
        prev_tracker_overlay_table_enabled = config.tracker_overlay_table_enabled;
        OverlayConfig_MarkDirty();
    }
}
