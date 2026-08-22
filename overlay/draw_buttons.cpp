// 控制包含量，减少 Windows 头带来的额外定义
#define WIN32_LEAN_AND_MEAN
// 避免 winsock.h 与 winsock2.h 的冲突
#define _WINSOCKAPI_
// 包含 Winsock2（UI 文件通常不直接使用网络，这里仅为一致性）
#include <winsock2.h>
// 包含 Windows API（窗口与消息相关）
#include <Windows.h>

// 使用 std::string 和 std::vector 容器
#include <string>
#include <vector>

// ImGui 主头，提供绘制 GUI 的接口
#include "imgui/imgui.h"
// 项目全局头，包含 config 等全局对象
#include "mybot.h"
// overlay 模块头，声明 Overlay 相关接口
#include "overlay.h"
// 当 UI 修改配置时，标记为脏以便后续保存
#include "overlay/config_dirty.h"
// UI 布局尺寸、常量定义
#include "overlay/ui_sections.h"

// 匿名命名空间内包含了一些内部辅助函数，仅在本文件可见
namespace
{
// 在按键名称数组中查找给定名称的索引，未找到返回 0（默认按键）
int findKeyIndexByName(const std::string& keyName)
{
    // 遍历全局按键名数组，比较字符串
    for (size_t k = 0; k < key_names.size(); ++k)
    {
        if (key_names[k] == keyName)
            return static_cast<int>(k); // 找到则返回索引
    }
    return 0; // 未找到则返回 0
}

// 绘制一组按键绑定的行（可多个绑定），并返回是否发生修改
bool drawButtonBindingRows(const char* rowLabel, std::vector<std::string>& bindings, bool keepAtLeastOne)
{
    // 如果按键名称列表尚未准备好，显示提示并返回
    if (key_names_cstrs.empty())
    {
        ImGui::TextDisabled("按键列表不可用。");
        return false; // 无法绘制
    }

    bool changed = false; // 标记是否被用户修改过
    // 如果没有任何绑定且需要至少一个绑定，则添加一个占位绑定
    if (bindings.empty() && keepAtLeastOne)
    {
        bindings.push_back("None"); // 占位，表示无操作
        changed = true; // 视为已变更以便后续保存
    }

    // 使用 rowLabel 作为 ImGui 的 ID 片段，避免与其他控件冲突
    ImGui::PushID(rowLabel);

    // 遍历每个绑定并绘制：标签、下拉框、添加/删除按钮
    for (size_t i = 0; i < bindings.size();)
    {
        // 引用当前绑定的字符串，便于直接修改
        std::string& currentKeyName = bindings[i];
        // 找到该绑定在按键名数组中的索引（用于在 Combo 中显示当前选择）
        int currentIndex = findKeyIndexByName(currentKeyName);
        // 为多绑定的情况生成带序号的标签，例如 "动作 2"
        const std::string indexedLabel = (bindings.size() > 1)
            ? std::string(rowLabel) + " " + std::to_string(i + 1)
            : std::string(rowLabel);

        // 使用索引作为 PushID 的一部分，确保同一行内元素的 ID 唯一
        ImGui::PushID(static_cast<int>(i));

        // 显示文本标签
        ImGui::TextUnformatted(indexedLabel.c_str());
        ImGui::SameLine(); // 在同一行继续绘制下一个控件
        const float actionBtnW = ImGui::GetFrameHeight(); // 按钮宽度参考
        ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth); // 下拉宽度设置

        // 绘制下拉选择（Combo），显示可绑定的按键名
        if (ImGui::Combo("##value", &currentIndex, key_display_names_cstrs.data(), static_cast<int>(key_display_names_cstrs.size())))
        {
            // 用户选择了新的按键，将选择的按键名写回绑定数组
            currentKeyName = key_names[currentIndex];
            changed = true; // 标记变更
        }

        // 在下拉后绘制添加绑定的按钮
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("+", ImVec2(actionBtnW, 0.0f)))
        {
            // 在当前绑定后插入一个新的占位绑定
            bindings.insert(bindings.begin() + static_cast<std::vector<std::string>::difference_type>(i + 1), "None");
            changed = true; // 标记变更
        }

        // 在添加按钮后绘制删除按钮，并处理删除逻辑
        ImGui::SameLine(0.0f, 3.0f);
        bool removedCurrent = false; // 本次循环是否删除了当前项
        if (ImGui::Button("-", ImVec2(actionBtnW, 0.0f)))
        {
            if (bindings.size() <= 1 && keepAtLeastOne)
            {
                // 如果必须至少保留一个绑定，则将其重置为 "None"，而不是删除
                bindings[0] = "None";
            }
            else
            {
                // 否则直接删除当前绑定
                bindings.erase(bindings.begin() + static_cast<std::vector<std::string>::difference_type>(i));
                removedCurrent = true; // 标记已删除以便跳过 i++
            }
            changed = true; // 标记变更
        }

        ImGui::PopID(); // 弹出 PushID(static_cast<int>(i))

        if (removedCurrent)
            continue; // 如果删除了当前项，继续循环而不递增 i

        ++i; // 仅在未删除当前项时递增索引
    }

    ImGui::PopID(); // 弹出最外层 PushID(rowLabel)

    return changed; // 返回是否有修改
}

// 包装函数：绘制绑定行并在发生更改时标记配置脏
void drawBindingRowsAndMarkDirty(const char* rowLabel, std::vector<std::string>& bindings, bool keepAtLeastOne = true)
{
    if (drawButtonBindingRows(rowLabel, bindings, keepAtLeastOne))
        OverlayConfig_MarkDirty(); // 标记需要保存
}
} // 匿名命名空间结束

// 绘制主程序热键面板的入口函数
void draw_buttons()
{
    ImGui::PushID("buttons_section_hotkeys"); // 使用唯一 ID 分组本节控件
    ImGui::SeparatorText("主程序的全局热键"); // 显示本节标题
    // 以下三行分别绘制常用热键的绑定行：退出、暂停、自定义面板切换
    drawBindingRowsAndMarkDirty("直接退出程序", config.button_exit);
    drawBindingRowsAndMarkDirty("暂停自瞄触发", config.button_pause);
    drawBindingRowsAndMarkDirty("弹出收起面板", config.button_open_overlay);
    ImGui::PopID(); // 恢复 ID 栈
}
