# ImGui 全宽复选框与等宽列布局

## 问题

本项目的自定义 `ImGui::Checkbox()` 在有可见标签时，会将整项宽度设置为当前可用内容宽度：

```cpp
const float row_w = has_visible_label
    ? ImMax(min_w, GetContentRegionAvail().x)
    : ImMax(min_w, CalcItemWidth());
```

因此，在同一内容区域连续绘制带标签的复选框时，每个复选框都是一整行的可点击区域。`ImGui::SameLine()` 只能移动下一个控件的绘制光标；它不会改变前一个复选框已经计算出的 `GetContentRegionAvail().x`，也不会将可用宽度按列分配。

如果继续用 `SameLine()` 排列类别 0 到类别 14，类别 0 的控件会先占满整行，后续控件会被放到窗口右侧，导致它们不可见或无法点击。

相关自定义控件实现位于 `imgui/imgui_widgets.cpp` 的 `ImGui::Checkbox()`。

## 不适用的方案：`SameLine()`

`SameLine()` 适合排列宽度由内容或调用方明确控制的紧凑控件，例如两个小按钮。它不适合当前的全宽复选框，原因如下：

- 全宽复选框会读取当前可用区域宽度并占满它。
- `SameLine()` 不提供列轨道，不会为五个项目预留各占 20% 的区域。
- 即使标签长度相同，窗口缩放后也无法保证每项宽度一致。

不要通过硬编码 X 偏移或固定像素宽度补救。这会在 UI 缩放、窗口宽度改变或字体变化时重新产生溢出问题。

## 正确方案：五列等宽表格

对固定的 15 个目标类别，使用 5 列、等宽拉伸的 ImGui 表格：

```cpp
if (ImGui::BeginTable(
        "global_target_classes",
        5,
        ImGuiTableFlags_SizingStretchSame))
{
    for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
    {
        ImGui::TableNextColumn();
        ImGui::PushID(cls);

        bool enabled = config.class_enabled[cls];
        ImGui::Checkbox(("类别 " + std::to_string(cls)).c_str(), &enabled);

        ImGui::PopID();
    }
    ImGui::EndTable();
}
```

`ImGuiTableFlags_SizingStretchSame` 会将表格的可用宽度平均分给五列。每次 `TableNextColumn()` 进入下一个单元格；第五列之后会自动开始下一行。因此 `15 / 5 = 3`，类别顺序固定为：

```text
类别 0   类别 1   类别 2   类别 3   类别 4
类别 5   类别 6   类别 7   类别 8   类别 9
类别 10  类别 11  类别 12  类别 13  类别 14
```

自定义 `Checkbox()` 在表格单元格内调用 `GetContentRegionAvail().x` 时，获得的是该单元格的宽度而非整个区域的宽度，所以仍可保留整块可点击区域，同时不会越过相邻列。

当前实际实现见 `overlay/draw_ai.cpp` 的 `draw_global_ai_settings()`：

- `BeginTable("global_target_classes", 5, ImGuiTableFlags_SizingStretchSame)` 创建五等分列。
- `Config::FIXED_TARGET_CLASS_COUNT` 固定为 15。
- 每个类别在一个独立表格单元格中调用 `ImGui::Checkbox()`。

## 与设置行控件的区别

`OverlayUI::CheckboxRow()` 用于单个设置项的“左侧标签、右侧控件”布局。它通过 `BeginSettingRow()` 读取整个内容区域的可用宽度，并将控件放到右侧的统一控制区；这正是它适合单行设置、不适合类别网格的原因。

| 布局需求 | 应使用的模式 |
| --- | --- |
| 单个设置项，标签在左、控件在右 | `OverlayUI::CheckboxRow()` |
| 多个同类开关，需要固定列数和等宽分布 | `ImGui::BeginTable()` + `ImGuiTableFlags_SizingStretchSame` |
| 紧凑的、宽度由控件本身决定的项目 | `ImGui::SameLine()` |

`CheckboxRow()` 与 `BeginSettingRow()` 的实现位于 `overlay/ui_sections.h`。
