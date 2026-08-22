# 原始 ImGui 新手入门教学（已迁移）

> 此单文件教程已于 2026-07-31 拆分为更基础、更详细的十天学习计划。请从 [../../imgui-learning/README.md](../../imgui-learning/README.md) 开始；本文件仅保留为原始内容归档，方便追溯。

# ImGui 新手入门教学

本文以本项目的 Overlay 设置面板为例，从零开始学习 Dear ImGui。目标不是只会调用几个控件，而是能遵循项目现有习惯，独立写出一个安全、易维护的设置页面。

## 1. 先理解 ImGui 是什么

Dear ImGui 是“立即模式 GUI”（Immediate Mode GUI）库。

传统 GUI 常把按钮、输入框等控件创建成长期存在的对象；ImGui 不这样做。程序每一帧都按当前数据重新调用绘制函数：

```cpp
void draw_example()
{
    ImGui::TextUnformatted("你好，ImGui");
    ImGui::Button("执行操作");
}
```

这一段会在每一帧执行。ImGui 根据本帧的鼠标、键盘输入，返回“这个控件是否被修改/点击”的结果。

因此要记住三件事：

1. ImGui 控件不是业务数据的主人，业务数据应存在 `config`、运行时对象或你自己定义的状态中。
2. 绘制函数必须很快；不要在每帧绘制路径里反复执行硬件枚举、网络请求或长时间磁盘访问。
3. 控件的返回值通常表示“本帧发生了修改”，适合在该分支中保存配置或通知后台线程。

本项目的 Overlay 渲染与 Win32/DX11 后端在 `overlay/overlay.cpp` 中；具体的设置控件按功能分散在 `overlay/draw_capture.cpp`、`overlay/draw_mouse.cpp`、`overlay/draw_ai.cpp` 等文件中。

## 2. 本项目的 UI 结构

先从目录职责开始。新增页面前应先找到相近页面，而不是重新发明布局规则。

| 位置 | 职责 |
| --- | --- |
| `overlay/overlay.cpp` | 创建 Overlay 窗口、处理消息、驱动 ImGui 每帧渲染、导航页面。 |
| `overlay/draw_settings.h` | 各设置绘制函数的声明。 |
| `overlay/draw_capture.cpp` | 画面、模型、捕获源相关控件。 |
| `overlay/draw_mouse.cpp` | 鼠标、热键、移动参数相关控件。 |
| `overlay/draw_ai.cpp` | AI、类别、推理参数相关控件。 |
| `overlay/ui_sections.h` | 常用宽度常量、通用提示词等 UI 约定。 |
| `overlay/config_dirty.h` | 配置被 UI 修改后的延迟保存标记。 |
| `config/config.h` | `Config` 字段定义；持久配置的真正存放处。 |

例如，画面页入口 `draw_capture_and_model_settings()` 位于 `overlay/draw_capture.cpp`，它依次绘制模型、通用捕获设置、捕获源专属设置和统计摘要。将一组控件拆成小函数，比把所有 UI 堆在一个巨大函数中更容易维护。

## 3. 一帧发生了什么

概念上的帧顺序如下：

```cpp
ImGui_ImplWin32_NewFrame();
ImGui_ImplDX11_NewFrame();
ImGui::NewFrame();

// 调用本项目各个 draw_* 函数，在这里声明本帧界面。
draw_capture_and_model_settings();

ImGui::Render();
ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
```

项目已经封装好了这段流程。正常新增设置页时，不要手动再调用 `NewFrame()`、`Render()` 或后端的 `NewFrame()`；只需要在已有的页面绘制函数中调用 ImGui 控件。

`Begin` 与 `End`、`BeginChild` 与 `EndChild`、`BeginGroup` 与 `EndGroup` 必须配对。即使中间使用了条件分支，也要确保已经成功开始的容器能正确结束。

## 4. 第一个项目风格的控件

最常见的模式是：控件直接编辑配置字段；只有控件返回 `true` 时才执行副作用。

```cpp
if (ImGui::Checkbox("启用示例功能", &config.example_enabled))
{
    OverlayConfig_MarkDirty();
}
```

含义：

- `Checkbox` 接收一个标签和 `bool*`。
- 用户在本帧切换复选框时，它返回 `true`，并已经写入 `config.example_enabled`。
- `OverlayConfig_MarkDirty()` 告诉项目配置系统稍后保存，不需要每一帧写 `config.ini`。

不要这样写：

```cpp
config.example_enabled = ImGui::Checkbox("启用示例功能", &config.example_enabled);
```

因为 `Checkbox` 的返回值是“是否发生变化”，不是复选框当前值。

## 5. 常用文本控件

```cpp
ImGui::Text("当前 FPS: %d", captureFps.load());
ImGui::TextUnformatted("固定文本，不解析格式化符号");
ImGui::TextDisabled("当前不可用");
ImGui::SeparatorText("采集卡");
ImGui::Spacing();
```

使用建议：

- `Text` 适合有 `%d`、`%s`、`%.2f` 等格式化数据的文本。
- `TextUnformatted` 适合普通固定文本；它不会将文本中的 `%` 当成格式化符。
- `TextDisabled` 用于说明、禁用原因和低优先级状态。项目中“没有匹配的采集卡”等提示采用这种风格。
- `SeparatorText` 用作一组设置的标题，如 `overlay/draw_capture.cpp` 的“采集卡”“UDP捕获”。

## 6. 布局：从默认纵向排列开始

ImGui 默认每个控件占一行。下面是最稳定的起点：

```cpp
ImGui::Checkbox("启用", &config.example_enabled);
ImGui::InputInt("数量", &config.example_count);
ImGui::Button("应用");
```

需要把控件放到同一行时使用 `SameLine()`：

```cpp
ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
ImGui::InputInt("##example_count", &config.example_count);
ImGui::SameLine();
ImGui::TextDisabled("数量 [1, 100]");
```

`SetNextItemWidth` 只影响紧随其后的一个控件。项目将常用宽度收口在 `overlay/ui_sections.h` 的 `UiLayout` 命名空间中，例如：

```cpp
UiLayout::kNumericWidth
UiLayout::kTextShortWidth
UiLayout::kTextMediumWidth
UiLayout::kComboMediumWidth
UiLayout::kComboLongWidth
UiLayout::kActionButtonWidth
```

新增页面应优先复用这些宽度，避免每个页面出现不同尺寸的输入框。

## 7. 标签、隐藏标签与唯一 ID

ImGui 用控件标签同时生成显示文本和内部 ID：

```cpp
ImGui::Checkbox("启用", &enabled);
```

标签“启用”既显示给用户，也参与内部 ID。两个同层级控件若使用同一个标签，会出现状态串扰、点击错位或断言问题。

当视觉标签要单独摆放时，使用 `##` 隐藏显示部分，但保留 ID：

```cpp
ImGui::TextUnformatted("采集帧率");
ImGui::SameLine();
ImGui::InputInt("##capture_fps", &config.capture_fps);
```

`##capture_fps` 不显示，但其 ID 是唯一的。项目的数值输入框与捕获方式下拉框大量采用该写法。

若一段区域中会复用相同的隐藏标签，例如多个 `"##value"`，必须加 ID 作用域：

```cpp
ImGui::PushID("capture_section_example");
ImGui::InputInt("##value", &config.example_count);
ImGui::PopID();
```

项目在 `overlay/draw_capture.cpp` 用 `PushID("capture_section_virtual_camera")` 包裹采集卡区域。循环中还可以使用整数、指针或字符串作为 `PushID` 参数：

```cpp
for (int index = 0; index < count; ++index)
{
    ImGui::PushID(index);
    ImGui::Checkbox("启用", &items[index].enabled);
    ImGui::PopID();
}
```

规则：每个 `PushID` 必须有一个对应的 `PopID`，包括提前 `return` 前的路径。

## 8. 复选框、按钮与禁用状态

### 8.1 Checkbox

```cpp
if (ImGui::Checkbox("圆形视野", &config.circle_fov_enabled))
{
    OverlayConfig_MarkDirty();
}
```

如果一个开关控制后续配置是否可见，直接使用条件分支：

```cpp
if (config.circle_fov_enabled)
{
    ImGui::InputInt("圆形视野大小", &config.circle_fov_radius_percent);
}
```

这就是条件面板。`draw_capture.cpp` 中的圆形视野、WinRT 窗口目标、采集卡和 UDP 面板都是同一思想：根据当前配置绘制对应的专属控件。

### 8.2 Button

```cpp
if (ImGui::Button("应用设置", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
{
    ApplyExampleSettings();
}
```

按钮只在被点击的那一帧返回 `true`。适合刷新列表、应用一组待确认设置、保存、执行测试等操作。

本项目的采集卡设置是重要范例：选择设备、修改宽高或 FPS 只标记待应用；点击“应用采集卡设置”才通知捕获线程创建或重建采集卡。这避免在每帧或每次输入时执行昂贵的硬件操作。

### 8.3 BeginDisabled / EndDisabled

当功能不适用于当前状态时，禁用而不是偷偷忽略用户操作：

```cpp
const bool available = config.capture_method == "duplication_api";
if (!available)
    ImGui::BeginDisabled();

ImGui::Checkbox("CUDA 加速捕获", &config.capture_use_cuda);

if (!available)
{
    ImGui::EndDisabled();
    ImGui::TextDisabled("仅在DXGI模式下可用。");
}
```

项目的 CUDA 捕获选项采用这一模式。`BeginDisabled` 和 `EndDisabled` 也必须严格配对。

## 9. 数字输入与范围校验

最简单的整数输入：

```cpp
if (ImGui::InputInt("最大数量", &config.example_count))
{
    OverlayConfig_MarkDirty();
}
```

项目常用“输入框 + 减号 + 加号 + 说明”的紧凑模式：

```cpp
ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
bool changed = ImGui::InputInt("##example_count", &config.example_count);
ImGui::SameLine();
if (ImGui::Button("-##example_count"))
{
    --config.example_count;
    changed = true;
}
ImGui::SameLine();
if (ImGui::Button("+##example_count"))
{
    ++config.example_count;
    changed = true;
}
ImGui::SameLine();
ImGui::TextDisabled("数量 [1, 100]");

ValidateIntParam(&config.example_count, 1, 100, 10);
if (changed)
    OverlayConfig_MarkDirty();
```

要点：

1. `InputInt` 不会自动限制输入范围，用户可粘贴任何整数。
2. 每一种改变路径都要让 `changed` 为真，不能只处理输入框而忘记加减按钮。
3. 调用项目已有的 `ValidateIntParam` 或等价范围限制逻辑，使运行时值始终安全。
4. 如果修改需要重建捕获器、重载模型等，不要直接做耗时工作；先写配置，再用项目已有原子标志通知对应线程。

`InputFloat`、`SliderInt`、`SliderFloat` 的原则相同。滑块适合范围小且可视化直观的值；精确参数通常更适合输入框。

## 10. 下拉框 Combo

固定选项可以直接传入 C 字符串数组：

```cpp
static const char* modes[] = { "快速", "稳定", "自定义" };
int selected = 0;
if (ImGui::Combo("模式", &selected, modes, IM_ARRAYSIZE(modes)))
{
    OverlayConfig_MarkDirty();
}
```

项目常将“内部值”和“显示名”分开，避免把中文展示文案写入配置：

```cpp
std::vector<std::string> values = { "duplication_api", "winrt", "virtual_camera" };
std::vector<std::string> labels = { "DXGI", "WinRT", "采集卡" };

int selected = 0;
if (ImGui::Combo("捕获方式", &selected, items.data(), static_cast<int>(items.size())))
{
    config.capture_method = values[selected];
    OverlayConfig_MarkDirty();
}
```

实际代码可参考 `overlay/draw_capture.cpp` 的捕获方式选择。写配置时保存稳定的内部值；显示时使用可翻译、可修改的标签。

对于动态列表，例如窗口列表、模型列表、采集卡列表，先构造 `std::vector<const char*>`。要注意 `c_str()` 指向的 `std::string` 在本帧调用期间必须保持有效，不能从临时字符串取指针。

当选项需要复杂预览、搜索或每项额外说明时，用 `BeginCombo` / `Selectable`：

```cpp
if (ImGui::BeginCombo("设备", preview.c_str()))
{
    for (const auto& device : devices)
    {
        const bool selected = config.device_name == device;
        if (ImGui::Selectable(device.c_str(), selected))
        {
            config.device_name = device;
            OverlayConfig_MarkDirty();
        }
        if (selected)
            ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}
```

WinRT 窗口选择的实现可参考 `overlay/draw_capture.cpp`。

## 11. 文本输入与缓冲区

`InputText` 使用可写字符缓冲区，而不是 `std::string`：

```cpp
static char filter_buf[128] = "";
ImGui::InputText("过滤", filter_buf, IM_ARRAYSIZE(filter_buf));
```

本项目的 `virtual_camera_filter_buf`、`capture_window_filter_buf`、`udp_ip_buf` 都是这种本地 UI 缓冲区。它们适合正在编辑但尚未应用的文本。

推荐分层：

- `static char ..._buf[]`：输入过程中的 UI 状态。
- `config.xxx`：用户已应用、可持久化的配置。
- “应用”按钮：把缓冲区写入配置并通知后台系统。

例如 UDP 面板先把 `config.udp_ip` 和 `config.udp_port` 初始化到缓冲区，用户编辑后点击“应用UDP设置”才写回配置并请求捕获重启。这样不会在每次按键时重建网络捕获器。

## 12. 本地 UI 状态、配置状态和运行状态

这是写复杂页面最重要的边界。

### 本地 UI 状态

仅用于界面显示与编辑过程，不必保存：

```cpp
static bool settings_pending = false;
static char filter_buf[128] = "";
static std::vector<std::string> cached_devices;
```

采集卡页面的 `virtual_camera_settings_pending` 用于提示“有未应用的更改”。它不是用户配置本身。

### 配置状态

用户希望下次启动仍保留的数据写到 `config`：

```cpp
config.virtual_camera_name = selected_device;
config.virtual_camera_fps = new_fps;
OverlayConfig_MarkDirty();
```

### 运行状态与后台线程通知

有些配置变更需要后台线程重启资源。UI 不应直接执行耗时操作，而应发出信号：

```cpp
capture_method_changed.store(true);
detector_model_changed.store(true);
```

捕获方式、模型或设备重建等标志由后台代码消费。可参考 `capture/capture.h` 中的原子标志声明和 `capture/capture.cpp` 中的处理逻辑。

不要因为“改了一个输入框”就立即枚举摄像头、打开网络端口或在 UI 线程等待后台任务。应让 UI 保持轻量、由后台线程完成耗时工作。

## 13. 设备列表：缓存、刷新和应用

动态设备列表是本项目最值得学习的复杂场景。

错误做法：

```cpp
// 错误：每帧都可能执行系统设备枚举。
devices = FindDevices();
```

正确的思路：

1. 用缓存保存上次结果。
2. 初次显示页面时读取缓存结果。
3. 只有用户点击“刷新”时才强制重新枚举。
4. 用户从下拉框选择设备后，仅更新待应用配置。
5. 用户点击“应用”后，才通知后台线程尝试打开设备一次。

采集卡页面体现了这套流程：

```cpp
if (ImGui::Button("刷新"))
{
    VirtualCameraCapture::ClearCachedCameraList();
    virtual_cameras = VirtualCameraCapture::GetAvailableVirtualCameras(true);
}

if (ImGui::Button("应用采集卡设置"))
{
    virtual_camera_apply_requested.store(true);
    capture_method_changed.store(true);
}
```

这条规则尤其重要：不存在设备时，程序不得周期性地在 UI 路径中扫描硬件。用户主动刷新与应用，才是可预测且不会卡顿的交互方式。

## 14. 提示词 Tooltip

好的设置页面不仅有控件，还要让用户理解参数用途。项目把多数设置提示集中在 `overlay/ui_sections.h` 的 `TooltipForSetting()`。

在页面中可以沿用现有辅助函数：

```cpp
ImGui::TextDisabled("采集帧率 [1, 240]");
ShowSettingTooltip("采集帧率");
```

新增通用配置项时：

1. 先使用清晰、稳定的中文标签。
2. 在 `TooltipForSetting()` 增加与标签匹配的简短解释。
3. 说明范围、性能影响和典型用途，不要只重复标签文字。

提示词应解释“什么时候用”“调大/调小会怎样”，而不是解释“这是一个输入框”。

## 15. 表格、分组和可折叠区域

控件较多时，按内容选择容器。

### BeginGroup / EndGroup

用于把一小块相关控件视作一个布局单元：

```cpp
ImGui::BeginGroup();
ImGui::Checkbox("启用", &config.example_enabled);
ImGui::InputInt("数量", &config.example_count);
ImGui::EndGroup();
```

### CollapsingHeader

用于可选的高级设置：

```cpp
if (ImGui::CollapsingHeader("高级设置"))
{
    ImGui::Checkbox("输出详细日志", &config.verbose);
}
```

不要把新手必须完成的基础设置藏进折叠栏；适合折叠的是调试、专家参数和低频操作。

### Table

多列排列一组结构相同的选项时使用表格，而不是堆叠大量 `SameLine()`：

```cpp
if (ImGui::BeginTable("category_grid", 3))
{
    for (int i = 0; i < count; ++i)
    {
        ImGui::TableNextColumn();
        ImGui::PushID(i);
        ImGui::Checkbox(category_names[i], &config.categories[i]);
        ImGui::PopID();
    }
    ImGui::EndTable();
}
```

可参考 `docs/guides/imgui-checkbox-layout-zh.md` 以及 `overlay/draw_ai.cpp` 中类别开关的布局。

## 16. 写一个完整的新页面：练习案例

假设要新增“示例功能”页面，包含开关、等级、模式、设备刷新和应用按钮。

### 第一步：增加配置字段

先在 `config/config.h` 定义真正需要持久化的字段，例如：

```cpp
bool example_enabled = false;
int example_level = 1;
std::string example_mode = "balanced";
std::string example_device;
```

同时按项目配置加载/保存模式更新 `config/config.cpp`。不要把临时过滤文本、下拉框索引等 UI 状态写进配置。

### 第二步：声明绘制函数

在 `overlay/draw_settings.h` 增加：

```cpp
void draw_example_settings();
```

### 第三步：实现页面

```cpp
void draw_example_settings()
{
    static bool pending = false;
    static std::vector<std::string> devices;

    ImGui::SeparatorText("示例功能");

    if (ImGui::Checkbox("启用示例功能", &config.example_enabled))
    {
        OverlayConfig_MarkDirty();
        pending = true;
    }

    ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
    if (ImGui::InputInt("等级", &config.example_level))
    {
        ValidateIntParam(&config.example_level, 1, 10, 1);
        OverlayConfig_MarkDirty();
        pending = true;
    }

    static const char* mode_labels[] = { "平衡", "快速", "稳定" };
    static const char* mode_values[] = { "balanced", "fast", "stable" };
    int mode_index = 0;
    for (int i = 0; i < IM_ARRAYSIZE(mode_values); ++i)
        if (config.example_mode == mode_values[i]) mode_index = i;

    ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth);
    if (ImGui::Combo("模式", &mode_index, mode_labels, IM_ARRAYSIZE(mode_labels)))
    {
        config.example_mode = mode_values[mode_index];
        OverlayConfig_MarkDirty();
        pending = true;
    }

    if (ImGui::Button("刷新设备", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
    {
        devices = EnumerateExampleDevices(); // 应只由用户操作触发。
    }

    if (ImGui::Button("应用示例设置", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
    {
        example_settings_changed.store(true); // 需要时定义自己的原子标志。
        pending = false;
    }

    if (pending)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("有未应用的更改");
    }
}
```

此示例中的 `EnumerateExampleDevices()`、`example_settings_changed` 只是示意名称。新增功能前要先确认项目已有的业务接口和线程模型，不要复制不存在的 API。

### 第四步：接入导航

在 `overlay/overlay.cpp` 的页面导航和内容绘制位置，参考现有页面调用 `draw_example_settings()`。页面名称、导航 ID、绘制函数应保持一一对应。

### 第五步：验证

至少手动检查：

1. 打开页面不会崩溃，标签和控件不重叠。
2. 修改每个字段后配置确实变更并能保存/重载。
3. 重复标签不会发生 ID 冲突。
4. 点击应用只执行一次对应操作。
5. 不存在设备或网络异常时，页面仍然可响应。
6. 切换到其他页面后再回来，本地状态是否符合预期。

## 17. 常见错误清单

### 每帧做耗时工作

错误：在 `draw_*()` 中扫描摄像头、枚举窗口、读大文件、连接网络。

修正：使用缓存；提供明确的“刷新”按钮；耗时任务放到后台线程或由一次性请求触发。

### 相同控件 ID

错误：同一作用域两个 `InputInt("##value", ...)`。

修正：使用可见的不同标签，或以 `PushID/PopID` 创建作用域。

### 忘记标记配置已修改

错误：配置字段变了，但重启后恢复旧值。

修正：所有用户确认的配置修改路径都调用 `OverlayConfig_MarkDirty()`。

### 每次输入都重启后端

错误：输入 IP、设备名或分辨率的每个字符都触发重建。

修正：使用本地缓冲区和“应用”按钮；后台操作只由明确的应用请求触发。

### 没有校验数值

错误：用户输入负数或过大分辨率，后台代码收到非法值。

修正：在 UI 侧用 `ValidateIntParam` 或等价逻辑限制，并在配置加载和业务边界处保持必要校验。

### 容器不配对

错误：`BeginGroup()` 后遗漏 `EndGroup()`，或 `BeginCombo()` 后遗漏 `EndCombo()`。

修正：紧邻编写配对调用；复杂分支先整理控制流，再增加容器。

## 18. 新增页面前的检查表

1. 我是否找到了一个功能与布局最相近的 `draw_*.cpp` 页面？
2. 每个控件是否有唯一 ID？
3. 哪些值是临时 UI 状态，哪些值应该进入 `config`？
4. 配置改变后是否调用了 `OverlayConfig_MarkDirty()`？
5. 该改变是否需要通知后台线程？如果需要，是否使用了正确的原子标志或既有接口？
6. 是否避免在每帧绘制函数内做耗时工作？
7. 是否为复杂或有风险的参数提供了范围说明、禁用提示或 Tooltip？
8. `Begin/End`、`PushID/PopID`、`BeginDisabled/EndDisabled` 是否全部配对？
9. 是否在 Release 构建后实际打开 Overlay，完成一次鼠标点击和输入验证？

## 19. 推荐阅读顺序

1. `overlay/draw_capture.cpp`：适合学习基础输入、下拉框、条件面板、设备刷新与应用。
2. `overlay/ui_sections.h`：理解统一宽度与项目提示词。
3. `overlay/draw_mouse.cpp`：学习更复杂的配置分区、热键页面和大量数值控件。
4. `overlay/draw_ai.cpp`：学习类别开关、表格与 AI 参数组织。
5. `overlay/overlay.cpp`：最后再理解窗口、导航和 DX11/Win32 渲染生命周期。
6. `docs/guides/imgui-checkbox-layout-zh.md`：查看本项目对复选框网格布局的补充说明。

掌握本文后，新增页面的正确顺序应当是：先定义数据和副作用边界，再编写小型 `draw_*` 函数，复用项目布局与提示词规则，最后在真实 Overlay 中验证交互与性能。
