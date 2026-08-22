#define WIN32_LEAN_AND_MEAN // 减少 Windows 头包含的额外项，提高编译速度
#define _WINSOCKAPI_ // 防止包含 winsock.h 与 winsock2 冲突
#include <winsock2.h> // 包含 Winsock2 的网络头（仅为保持一致，UI 文件很少直接使用）
#include <Windows.h> // 包含基本 Windows API（窗口控制、消息循环等）
#include "imgui/imgui.h" // ImGui 主头，提供所有 UI 绘制函数
#include "mybot.h" // 项目主头，包含全局配置对象 config 等
#include "overlay.h" // overlay 模块声明头，用于访问覆盖层功能
#include "overlay/config_dirty.h" // 标记配置已修改的接口，便于后续保存
#include "overlay/ui_sections.h" // UI 布局尺寸等常量（例如按钮宽度）

// 函数: draw_overlay
// 作用: 绘制覆盖层界面设置面板，包括隐藏选项和保存按钮。
// 说明: 此函数被 UI 主循环调用来显示与覆盖层外观相关的控件。
void draw_overlay()
{
    ImGui::PushID("overlay_section_visual"); // 使用唯一 ID 分组本节控件，避免重复
    ImGui::SeparatorText("界面设置"); // 绘制分割线与小节标题
    if (ImGui::Checkbox("从捕获中隐藏面板", &config.overlay_exclude_from_capture)) // 用户勾选框，改变配置值
    { // 当用户改变复选框状态时执行以下代码
        Overlay_ApplyCaptureExclusion(); // 应用新的隐藏/显示设置到窗口
        OverlayConfig_MarkDirty(); // 标记配置已更改，程序稍后会保存到磁盘
    }
    ShowSettingTooltip("从捕获中隐藏面板"); // 在复选框旁显示帮助提示（悬停时出现）
    ImGui::PopID(); // 恢复 ID 栈

    ImGui::Spacing(); // 插入垂直间距，美化 UI 布局
    if (ImGui::Button("保存配置到文件", ImVec2(UiLayout::kActionButtonWidth, 0.0f))) // 绘制保存按钮
    { // 用户点击保存按钮时执行
        // 修复：原实现直接调用无参 config.saveConfig()，存在三个问题：
        //   1) 与延迟落盘机制使用的 saveConfig("config.ini") 目标文件名可能不一致（保存位置分裂）；
        //   2) 完全绕过 OverlayConfig_MarkDirty/TrySave 的去抖机制；
        //   3) 保存后不清 cfgDirty，0.35s 后 TrySave 会再写一次盘（用户点一次实际写两次）。
        // 改为统一走 MarkDirty + SaveNow：保持「点击必定立即写盘」的原语义
        //（SaveNow 在 !cfgDirty 时会提前返回，故需先置脏），同时清除脏标记消除双写。
        OverlayConfig_MarkDirty();
        OverlayConfig_SaveNow();
    }
    ShowSettingTooltip("保存配置到文件"); // 为保存按钮显示帮助提示
}

