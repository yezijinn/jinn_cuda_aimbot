// 减少 Windows 头包含的额外项，避免拉入不必要的定义
#define WIN32_LEAN_AND_MEAN
// 避免 winsock.h 与 winsock2 冲突的宏定义
#define _WINSOCKAPI_
// 包含 Winsock2 头（通常在 UI 文件中并不直接使用网络，但保持一致）
#include <winsock2.h>
// 包含 Windows API 的基础头（用于窗口相关操作等）
#include <Windows.h>

// 包含常用 STL 头以便使用字符串流、容器等
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <array>
#include <vector>

// ImGui 主头：提供所有绘制 UI 的函数和类型
#include "imgui/imgui.h"

// 项目公共头，包含全局 config 等定义
#include "mybot.h"
// 辅助工具函数，例如字符串处理、时间等
#include "include/other_tools.h"
// overlay 模块的声明头，包含窗口与渲染相关接口
#include "overlay.h"
// 当配置变更时标记以便保存的接口
#include "overlay/config_dirty.h"
#include "overlay/onnx_inspector.h"
// 绘制设置相关的辅助函数和声明
#include "draw_settings.h"
// UI 布局尺寸常量定义
#include "overlay/ui_sections.h"
#ifdef USE_CUDA
// 当启用 CUDA 时，导出面板与监控头文件
#include "overlay/export_progress_panel.h"
#include "trt_monitor.h"
#endif

// 保存上一次的后端名称（用于检测更改）
// 修复（跨 TU 静态初始化 UB）：原实现在 TU 静态初始化期读取另一翻译单元的
// 全局对象 config.*（含 std::string 等非平凡成员），属于 C++ 标准未定义行为——
// 静态初始化顺序不确定，可能读到零值；且优化器可据此误判。改为只声明、不初始化，
// 实际初值由下方 draw_ai() 首帧守卫块（ai_state_initialized）惰性同步。
std::string prev_backend;
// 当前活动热键槽（用于 UI 选择）
static int activeHotkeySlot = 0;
// 上次的置信度阈值，用于检测用户更改
float prev_confidence_threshold = 0.0f;
// 上次的 NMS 阈值，用于检测用户更改
float prev_nms_threshold = 0.0f;
// 上次的最大检测数
int prev_max_detections = 0;
// 上次的自动瞄准开关状态
bool prev_auto_aim = false;
// 上次追踪器启用状态
bool prev_tracker_enabled = false;
// 上次的目标选择模式
std::string prev_targeting_mode;

// 动态范围相关的历史值
bool prev_dynamic_range_enabled = false;
int prev_dynamic_range_shrink_scope = 0;
std::string prev_dynamic_range_target_classes;

// 全局类别启用状态的历史数组（用于检测哪些类别被启用）
bool prev_class_enabled[Config::FIXED_TARGET_CLASS_COUNT]{};
// 表示是否已经初始化了上面数组
static bool class_prev_initialized = false;

// 表示 AI 页面状态是否已初始化
static bool ai_state_initialized = false;

// 绘制 AI 设置面板的主函数（被外部 UI 调用）
void draw_ai()
{
#ifdef USE_CUDA
    // 如果启用了 CUDA，就把后端设置为 TRT（TensorRT）
    config.backend = "TRT";
#else
    // 否则使用 DML（DirectML）作为后端
    config.backend = "DML";
#endif

    // 如果本界面首次初始化则保存当前配置到上次状态变量
    if (!ai_state_initialized)
    {
        prev_backend = config.backend;
        prev_confidence_threshold = config.confidence_threshold;
        prev_nms_threshold = config.nms_threshold;
        prev_max_detections = config.max_detections;
        prev_auto_aim = config.auto_aim;
        prev_tracker_enabled = config.tracker_enabled;
        prev_targeting_mode = config.targeting_mode;
        prev_dynamic_range_enabled = config.dynamic_range_enabled;
        prev_dynamic_range_shrink_scope = config.dynamic_range_shrink_scope;
        prev_dynamic_range_target_classes = config.dynamic_range_target_classes;
        ai_state_initialized = true; // 标记已初始化
    }

#ifdef USE_CUDA
    // 如果当前正在导出 TRT 引擎，显示导出进度面板
    if (gIsTrtExporting)
    {
        OverlayExportUI::DrawTensorRtExportPanel(
            "ai_tensor_rt_export",
            "TensorRT 引擎导出",
            "正在编译优化的 AI 推理引擎",
            config.ai_model.c_str(),
            "取消导出");
    }
#endif


    // —— 变更检测 ——
    // 检查置信度、NMS 或最大检测数是否有变化，若有则标记配置脏
    if (prev_confidence_threshold != config.confidence_threshold ||
        prev_nms_threshold != config.nms_threshold ||
        prev_max_detections != config.max_detections)
    {
        prev_nms_threshold = config.nms_threshold;
        prev_confidence_threshold = config.confidence_threshold;
        prev_max_detections = config.max_detections;
        OverlayConfig_MarkDirty(); // 标记配置已更改
    }

    // 如果后端（模型引擎）改变，触发检测器模型重载
    if (prev_backend != config.backend)
    {
        prev_backend = config.backend;
        detector_model_changed.store(true); // 通知其他线程模型已改变
        OverlayConfig_MarkDirty();
    }

    // 检查自动瞄准、追踪器或目标选择模式的变化
    if (prev_auto_aim != config.auto_aim ||
        prev_tracker_enabled != config.tracker_enabled ||
        prev_targeting_mode != config.targeting_mode)
    {
        prev_auto_aim = config.auto_aim;
        prev_tracker_enabled = config.tracker_enabled;
        prev_targeting_mode = config.targeting_mode;
        OverlayConfig_MarkDirty();
    }

    // 检查动态范围设置是否变化
    if (prev_dynamic_range_enabled != config.dynamic_range_enabled ||
        prev_dynamic_range_shrink_scope != config.dynamic_range_shrink_scope ||
        prev_dynamic_range_target_classes != config.dynamic_range_target_classes)
    {
        prev_dynamic_range_enabled = config.dynamic_range_enabled;
        prev_dynamic_range_shrink_scope = config.dynamic_range_shrink_scope;
        prev_dynamic_range_target_classes = config.dynamic_range_target_classes;
        OverlayConfig_MarkDirty();
    }

    // 全局类别启用状态变更检测（用于避免用户禁用所有类别）
    if (!class_prev_initialized)
    {
        for (int i = 0; i < Config::FIXED_TARGET_CLASS_COUNT; ++i)
        {
            prev_class_enabled[i] = config.isClassEnabled(i); // 记录当前每个类别是否启用
        }
        class_prev_initialized = true; // 标记数组已初始化
    }
    bool classChanged = false; // 标记是否有类别启用状态变化
    for (int i = 0; i < Config::FIXED_TARGET_CLASS_COUNT; ++i)
    {
        if (prev_class_enabled[i] != config.isClassEnabled(i))
        {
            classChanged = true; // 发现变化则设置标志
            prev_class_enabled[i] = config.isClassEnabled(i); // 更新历史状态
        }
    }

    if (classChanged)
        OverlayConfig_MarkDirty(); // 若类别有变动，标记配置已更改
}

// 绘制全局 AI 设置（类别选择表格）的函数
void draw_global_ai_settings()
{
    ImGui::PushID("global_ai_classes"); // 为下面的控件分组推入 ID
        int globalEnabledCount = 0; // 统计当前启用的类别数量
        for (int i = 0; i < Config::FIXED_TARGET_CLASS_COUNT; ++i)
            globalEnabledCount += config.isClassEnabled(i) ? 1 : 0;
        // 提示信息，说明未勾选的类别不会参与后续功能
        ImGui::TextDisabled("不勾选的类别 不会进入锁定、瞄准、扳机、预览。");
        const StartupOnnxReport& report = startupOnnxReport();
        ImGui::TextUnformatted(report.success ? report.class_summary.c_str() : "智能推断，当前模型类别数量：未知");
        ImGui::TextUnformatted(report.success ? report.class_names.c_str() : "");
        // 开始一个 5 列的表格，用于显示类别复选框
        if (ImGui::BeginTable("global_target_classes", 5, ImGuiTableFlags_SizingStretchSame))
        {
            for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
            {
                ImGui::TableNextColumn(); // 移到下一个单元格
                ImGui::PushID(cls); // 使用类别索引作为临时 ID
                bool enabled = config.class_enabled[cls]; // 读取当前配置中的类别启用状态
                if (ImGui::Checkbox(("类别 " + std::to_string(cls)).c_str(), &enabled))
                {
                    // 如果用户尝试关闭最后一个启用的类别，禁止这样做
                    if (!enabled && globalEnabledCount <= 1)
                    {
                        enabled = true; // 恢复为启用，避免全部关闭
                    }
                    else
                    {
                        // 更新启用计数和配置，然后重置热键子集并标记为已更改
                        globalEnabledCount += enabled ? 1 : -1;
                        config.class_enabled[cls] = enabled;
                        config.resetHotkeyClassSubsets();
                        OverlayConfig_MarkDirty();
                    }
                }
                ShowSettingTooltip(("类别 " + std::to_string(cls)).c_str()); // 显示该类别的工具提示
                ImGui::PopID(); // 弹出前面 PushID
            }
            ImGui::EndTable(); // 结束表格
        }
    ImGui::PopID(); // 恢复 ID 栈
}

