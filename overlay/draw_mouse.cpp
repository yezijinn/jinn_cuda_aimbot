#define WIN32_LEAN_AND_MEAN // 减少 Windows 头的定义以加快编译
#define _WINSOCKAPI_ // 防止 winsock 冲突
#include <winsock2.h> // Winsock 网络库（保持与其他文件一致）
#include <Windows.h> // Windows API（窗口、消息等）

#include <algorithm> // STL 算法库（sort、find 等）
#include <array> // std::array（类别键名缓存等）
#include <cstdio> // C 标准 I/O（printf 等）

#include "imgui/imgui.h" // ImGui 主头，所有 UI 绘制函数

#include "mybot.h" // 项目全局头（包含 config 等）
#include "overlay/config_dirty.h" // 标记配置已更改的接口
#include "capture/capture.h" // 捕获模块声明
#include "overlay/ui_sections.h" // UI 布局常量
#include "kmbox_net/picture.h" // KMBOX 网络通信相关

extern std::atomic<bool> detector_model_changed; // 检测器模型已改变的原子标志（跨线程通知）
extern std::atomic<bool> detection_resolution_changed; // 检测分辨率改变的原子标志

namespace // 匿名命名空间：仅本文件可见的辅助代码
{
// ——— 保存上一帧的鼠标设置值，用于检测用户是否修改了配置 ———
// 注意：这些历史值一律以「类型默认值」静态初始化，绝不能写成 `= config.xxx`。
// 原因：全局对象 `config` 定义在 mybot.cpp（另一个翻译单元），且含 std::string /
// std::unordered_map 成员属非平凡构造，跨 TU 的动态初始化顺序未定义。在本 TU 静态初始化期
// 读取 `config.xxx` 属未定义行为，实际拿到的恒为零值；而真正的配置值要到 main() 里
// loadConfig() 之后才存在。旧写法导致首次绘制面板时全部历史值失配 → 无条件触发
// updateConfig() + MarkDirty()，用户什么都没改就重写一次 config.ini。
// 修复方式与 draw_ai.cpp 的 ai_state_initialized 保持一致：改为首帧惰性同步。
int prev_fovX = 0; // 上次的 FOV X 范围
int prev_fovY = 0; // 上次的 FOV Y 范围
float prev_minSpeedMultiplier = 0.0f; // 上次最小速度倍率
float prev_maxSpeedMultiplier = 0.0f; // 上次最大速度倍率
float prev_predictionInterval = 0.0f; // 上次预测间隔
bool  prev_kalman_enabled = false; // 上次 Kalman 启用状态
float prev_kalman_process_noise_position = 0.0f; // 上次 Kalman 位置过程噪声
float prev_kalman_process_noise_velocity = 0.0f; // 上次 Kalman 速度过程噪声
float prev_kalman_measurement_noise = 0.0f; // 上次 Kalman 测量噪声
float prev_kalman_velocity_damping = 0.0f; // 上次 Kalman 速度阻尼
float prev_kalman_max_velocity = 0.0f; // 上次 Kalman 最大速度
int   prev_kalman_warmup_frames = 0; // 上次 Kalman 预热帧数
bool  prev_kalman_compensate_detection_delay = false; // 上次 Kalman 检测延迟补偿
float prev_kalman_additional_prediction_ms = 0.0f; // 上次额外预测毫秒数
float prev_kalman_reset_timeout_sec = 0.0f; // 上次 Kalman 重置超时秒数
float prev_snapRadius = 0.0f; // 上次吸附半径
float prev_nearRadius = 0.0f; // 上次近距离半径
float prev_speedCurveExponent = 0.0f; // 上次速度曲线指数
float prev_snapBoostFactor = 0.0f; // 上次吸附加速倍数

// ——— 曲线与扰动相关的历史值 ———
bool  prev_curve_enabled = false; // 上次曲线启用状态
float prev_curve_intensity = 0.0f; // 上次曲线强度
float prev_perturbation_strength = 0.0f; // 上次扰动强度
float prev_curve_max_speed = 0.0f; // 上次曲线最大速度
float prev_curve_distance = 0.0f; // 上次曲线距离

// ——— 射击相关的历史值 ———
bool prev_auto_shoot = false; // 上次自动射击启用状态
float prev_bScope_multiplier = 0.0f; // 上次倍镜倍率乘数

bool mouse_state_initialized = false; // 首帧惰性同步标志

// 函数: syncPrevMouseSettings
// 作用: 把当前 config 的鼠标相关字段快照到全部 prev_* 历史值。
// 说明: 既用于首帧初始化，也用于变更检测命中后的状态推进，避免两处写法漂移。
void syncPrevMouseSettings()
{
    prev_fovX = config.fovX;
    prev_fovY = config.fovY;
    prev_minSpeedMultiplier = config.minSpeedMultiplier;
    prev_maxSpeedMultiplier = config.maxSpeedMultiplier;
    prev_predictionInterval = config.predictionInterval;
    prev_kalman_enabled = config.kalman_enabled;
    prev_kalman_process_noise_position = config.kalman_process_noise_position;
    prev_kalman_process_noise_velocity = config.kalman_process_noise_velocity;
    prev_kalman_measurement_noise = config.kalman_measurement_noise;
    prev_kalman_velocity_damping = config.kalman_velocity_damping;
    prev_kalman_max_velocity = config.kalman_max_velocity;
    prev_kalman_warmup_frames = config.kalman_warmup_frames;
    prev_kalman_compensate_detection_delay = config.kalman_compensate_detection_delay;
    prev_kalman_additional_prediction_ms = config.kalman_additional_prediction_ms;
    prev_kalman_reset_timeout_sec = config.kalman_reset_timeout_sec;
    prev_snapRadius = config.snapRadius;
    prev_nearRadius = config.nearRadius;
    prev_speedCurveExponent = config.speedCurveExponent;
    prev_snapBoostFactor = config.snapBoostFactor;
    prev_curve_enabled = config.curve_enabled;
    prev_curve_intensity = config.curve_intensity;
    prev_perturbation_strength = config.perturbation_strength;
    prev_curve_max_speed = config.curve_max_speed;
    prev_curve_distance = config.curve_distance;
    prev_auto_shoot = config.auto_shoot;
    prev_bScope_multiplier = config.bScope_multiplier;
}

// 函数: notifyMouseThreadConfig
// 作用: 统一的鼠标线程参数推送入口，内含空指针防护。
// 说明: globalMouseThread 在 mybot.cpp 中初值为 nullptr，且退出路径上会先于
//      Overlay 线程销毁；全项目其余调用点均有判空，此处补齐保持一致，避免退出竞态解引用。
void notifyMouseThreadConfig()
{
    if (!globalMouseThread)
        return;
    globalMouseThread->updateConfig(
        config.detection_resolution,
        config.fovX,
        config.fovY,
        config.minSpeedMultiplier,
        config.maxSpeedMultiplier,
        config.predictionInterval,
        config.auto_shoot,
        config.bScope_multiplier);
}

// 枚举：定义鼠标设置UI的不同页面/选项卡
enum class MouseSettingsPage
{
    All, // 显示所有设置
    Movement, // 仅显示移动相关设置
    Prediction, // 仅显示预测相关设置
    Assist, // 仅显示辅助相关设置
    Profiles, // 仅显示游戏配置相关
    Input // 仅显示输入设备相关
};

// 函数：判断当前页面是否应该绘制给定的内容类别
// 返回真表示应该绘制，假表示跳过（例如用户在非该页面时）
bool shouldDrawMousePage(MouseSettingsPage current, MouseSettingsPage wanted)
{
    // 如果是 All（显示全部）或者 current 恰好是 wanted，则返回真
    return current == MouseSettingsPage::All || current == wanted;
}

// ——— 类别相关配置键名缓存 ———
// 原实现在 draw_hotkey_profile 的类别面板里每帧执行 "class_order_" + std::to_string(lhs) 等
// 字符串拼接：排序比较 15×15≈225 次 + 每个启用类别 8+ 个瞄准点/扳机键，合计 300+ 次堆分配/帧。
// 类别索引固定（0..FIXED_TARGET_CLASS_COUNT-1）且键名常量，预构建一次后全部复用 std::string。
struct HotkeyClassKeyCache
{
    std::array<std::string, Config::FIXED_TARGET_CLASS_COUNT> order;   // class_order_N
    std::array<std::string, Config::FIXED_TARGET_CLASS_COUNT> enabled; // class_enabled_N
    std::array<std::string, Config::FIXED_TARGET_CLASS_COUNT> aimX;    // class_N_aim_offset_x
    std::array<std::string, Config::FIXED_TARGET_CLASS_COUNT> aimY;    // class_N_aim_offset_y
    std::array<std::string, Config::FIXED_TARGET_CLASS_COUNT> zoneX;   // class_N_trigger_zone_offset_x
    std::array<std::string, Config::FIXED_TARGET_CLASS_COUNT> zoneY;   // class_N_trigger_zone_offset_y
    std::array<std::string, Config::FIXED_TARGET_CLASS_COUNT> zoneW;   // class_N_trigger_zone_size_x
    std::array<std::string, Config::FIXED_TARGET_CLASS_COUNT> zoneH;   // class_N_trigger_zone_size_y
    bool built = false;

    void build()
    {
        for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
        {
            const std::string n = std::to_string(cls);
            order[cls]   = "class_order_" + n;
            enabled[cls] = "class_enabled_" + n;
            aimX[cls]    = "class_" + n + "_aim_offset_x";
            aimY[cls]    = "class_" + n + "_aim_offset_y";
            zoneX[cls]   = "class_" + n + "_trigger_zone_offset_x";
            zoneY[cls]   = "class_" + n + "_trigger_zone_offset_y";
            zoneW[cls]   = "class_" + n + "_trigger_zone_size_x";
            zoneH[cls]   = "class_" + n + "_trigger_zone_size_y";
        }
        built = true;
    }
};
static HotkeyClassKeyCache g_hotkeyClassKeys;

} // 匿名命名空间结束

// 函数: draw_mouse_page
// 作用: 核心函数，根据页面类型绘制鼠标设置的不同部分。
// 参数: page - MouseSettingsPage 枚举值，定义要显示的内容类别
// 说明: 该函数包含所有鼠标参数的 UI 绘制逻辑，包括 FOV、灵敏度、预测、曲线等。
//      能根据 page 参数选择性地显示特定类别的设置，支持灵活的 UI 布局。
static void draw_mouse_page(MouseSettingsPage page)
{
    // 首帧惰性同步：把 loadConfig() 之后的真实配置写入 prev_*，
    // 避免"用户未做任何改动却触发 updateConfig + 写盘"的误判。
    if (!mouse_state_initialized)
    {
        syncPrevMouseSettings();
        mouse_state_initialized = true;
    }

    if (shouldDrawMousePage(page, MouseSettingsPage::Profiles))
    {
        ImGui::PushID("mouse_section_global_fov");
        ImGui::SeparatorText("全局 FOV 范围");
        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        bool fovXChanged = ImGui::InputInt("##fov_x", &config.fovX, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        // 步进按钮就地钳制：ValidateIntParam 的语义是"越界即恢复默认值"而非 clamp，
        // 若在下限 1 处再点一次 "-" 会被重置成 121，直接摧毁用户已调好的值。
        if (ImGui::Button("-##fov_x"))
        {
            config.fovX = (std::max)(1, config.fovX - 1);
            fovXChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("+##fov_x"))
        {
            config.fovX = (std::min)(360, config.fovX + 1);
            fovXChanged = true;
        }
        ImGui::SameLine(); ImGui::TextDisabled("FOV范围X [1, 360]");
        ShowSettingTooltip("FOV范围X");
        if (fovXChanged)
            OverlayConfig_MarkDirty();
        ValidateIntParam(&config.fovX, 1, 360, 121);
        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        bool fovYChanged = ImGui::InputInt("##fov_y", &config.fovY, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (ImGui::Button("-##fov_y"))
        {
            config.fovY = (std::max)(1, config.fovY - 1);
            fovYChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("+##fov_y"))
        {
            config.fovY = (std::min)(360, config.fovY + 1);
            fovYChanged = true;
        }
        ImGui::SameLine(); ImGui::TextDisabled("FOV范围Y [1, 360]");
        ShowSettingTooltip("FOV范围Y");
        if (fovYChanged)
            OverlayConfig_MarkDirty();
        ValidateIntParam(&config.fovY, 1, 360, 90);
        ImGui::PopID();
    }

    if (shouldDrawMousePage(page, MouseSettingsPage::Profiles))
    {
        ImGui::PushID("mouse_section_factory_reset");
        if (ImGui::Button("所有参数都恢复默认", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
            ImGui::OpenPopup("确认全局恢复出厂");
        if (ImGui::BeginPopupModal("确认全局恢复出厂", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("将备份当前配置并恢复全部默认参数，是否继续？");
            if (ImGui::Button("确认恢复", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
            {
                if (config.resetToFactoryDefaults())
                {
                    detector_model_changed.store(true);
                    detection_resolution_changed.store(true);
                    OverlayConfig_MarkDirty();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    if (shouldDrawMousePage(page, MouseSettingsPage::Profiles))
    {
        ImGui::PushID("mouse_section_game_profile");
        ImGui::SeparatorText("游戏配置");

        // 块: 收集并排序所有游戏配置名称
        std::vector<std::string> profile_names; // 存储所有配置名
        for (const auto& kv : config.game_profiles) // 遍历配置字典
            profile_names.push_back(kv.first); // 提取配置名
        std::sort(profile_names.begin(), profile_names.end()); // 字母序排序

        // 块: 查找当前活跃配置在列表中的索引
        static int selected_index = 0; // 当前选择的索引（保留状态）
        for (size_t i = 0; i < profile_names.size(); ++i)
        {
            if (profile_names[i] == config.active_game) // 匹配当前活跃配置
            {
                selected_index = static_cast<int>(i); // 记录其索引
                break;
            }
        }

        // 块: 转换字符串向量为 C 风格字符串指针供 ImGui::Combo 使用
        std::vector<const char*> profile_items; // ImGui Combo 需要的指针数组
        for (const auto& name : profile_names)
            profile_items.push_back(name.c_str()); // 获取每个字符串的 C 指针

        ImGui::SetNextItemWidth(UiLayout::kComboLongWidth);
        if (ImGui::Combo("正使用的配置文件，首次使用请添加新的配置名称", &selected_index, profile_items.data(), static_cast<int>(profile_items.size())))
        { // 用户选择了新配置
            config.active_game = profile_names[selected_index]; // 更新活跃配置
            OverlayConfig_MarkDirty(); // 标记需要保存
            notifyMouseThreadConfig(); // 通知鼠标线程使用新参数（内含空指针防护）
        }

        const auto& gp = config.currentProfile(); // 获取当前活跃配置

        // 块: 显示当前配置的摘要信息（只读）
        ImGui::Text("当前配置名: %s", gp.name.c_str()); // 配置名称
        ImGui::Text("移动灵敏度: %.4f", gp.sens); // 全局移动灵敏度
        ImGui::Text("水平灵敏度: %.4f", gp.yaw); // 水平转向灵敏度
        ImGui::Text("垂直灵敏度: %.4f", gp.pitch); // 垂直转向灵敏度
        ImGui::Text("视野的缩放: %s", gp.fovScaled ? "true" : "false"); // 视野缩放开关

        // 块: 检查是否为 UNIFIED 配置（全局配置无法编辑）
        if (gp.name != "UNIFIED") // 如果不是全局 UNIFIED 配置
        {
            Config::GameProfile& modifiable = config.game_profiles[gp.name]; // 获取可编辑的配置引用
            bool changed = false; // 标记配置是否被修改

            // 块: 将 double 参数转为 float 供 ImGui 输入框编辑
            float sens_f = static_cast<float>(modifiable.sens); // 灵敏度转 float
            float yaw_f = static_cast<float>(modifiable.yaw); // 水平灵敏度转 float
            float pitch_f = static_cast<float>(modifiable.pitch); // 垂直灵敏度转 float
            float baseFOV_f = static_cast<float>(modifiable.baseFOV); // 基础视角转 float

            // 辅助: 对局部副本做范围校验，并把"发生了自动修正"也计入 changed。
            // 原实现只修正局部副本、不回写 config：当 config.ini 存有越界值（例如 sens=0）时，
            // 每帧都会打印一行"数值不合理…已恢复默认值"却永远修不好，同时 UI 显示 1.0 而
            // Config::degToCounts() 实际读到 0 直接 return {0,0} —— 鼠标完全不动却毫无提示。
            auto validateFloatSticky = [&](float* value, float minVal, float maxVal, float defVal)
            {
                const float before = *value;
                ValidateFloatParam(value, minVal, maxVal, defVal);
                if (*value != before)
                    changed = true; // 触发下方回写，让修正真正落到 config
            };

            // 块: 灵敏度调整控件
            // 步进按钮就地钳制，避免在下限处再点一次 "-" 被 ValidateFloatParam 重置为默认值，
            // 抹掉用户耗时校准的灵敏度（不可撤销且立即落盘）。
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            changed |= ImGui::InputFloat("##sensitivity", &sens_f, 0.0f, 0.0f, "%.4f", ImGuiInputTextFlags_CharsDecimal); // 文本输入
            ImGui::SameLine(); if (ImGui::Button("-##sensitivity")) { sens_f = (std::max)(0.01f, sens_f - 0.0001f); changed = true; } // 减少按钮
            ImGui::SameLine(); if (ImGui::Button("+##sensitivity")) { sens_f = (std::min)(100.0f, sens_f + 0.0001f); changed = true; } // 增加按钮
            ImGui::SameLine(); ImGui::TextDisabled("移动灵敏度 [0.01, 100]"); // 范围提示
            ShowSettingTooltip("灵敏度"); // 帮助提示
            validateFloatSticky(&sens_f, 0.01f, 100.0f, 1.0f); // 范围验证（修正即回写）

            // 块: 水平灵敏度调整
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            changed |= ImGui::InputFloat("##yaw", &yaw_f, 0.0f, 0.0f, "%.4f", ImGuiInputTextFlags_CharsDecimal); // 文本输入
            ImGui::SameLine(); if (ImGui::Button("-##yaw")) { yaw_f = (std::max)(0.0001f, yaw_f - 0.0001f); changed = true; } // 减少
            ImGui::SameLine(); if (ImGui::Button("+##yaw")) { yaw_f = (std::min)(10.0f, yaw_f + 0.0001f); changed = true; } // 增加
            ImGui::SameLine(); ImGui::TextDisabled("水平灵敏度 [0.0001, 10]"); // 范围
            ShowSettingTooltip("水平灵敏度");
            validateFloatSticky(&yaw_f, 0.0001f, 10.0f, 0.022f); // 验证（修正即回写）

            // 块: 垂直灵敏度调整
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            changed |= ImGui::InputFloat("##pitch", &pitch_f, 0.0f, 0.0f, "%.4f", ImGuiInputTextFlags_CharsDecimal); // 文本输入
            ImGui::SameLine(); if (ImGui::Button("-##pitch")) { pitch_f = (std::max)(0.0001f, pitch_f - 0.0001f); changed = true; } // 减少
            ImGui::SameLine(); if (ImGui::Button("+##pitch")) { pitch_f = (std::min)(10.0f, pitch_f + 0.0001f); changed = true; } // 增加
            ImGui::SameLine(); ImGui::TextDisabled("垂直灵敏度 [0.0001, 10]"); // 范围
            ShowSettingTooltip("垂直灵敏度");
            validateFloatSticky(&pitch_f, 0.0001f, 10.0f, 0.022f); // 验证（修正即回写）

            // 块: 视野缩放开关及其条件参数
            changed |= ImGui::Checkbox("视野缩放值", &modifiable.fovScaled); // 启用/禁用视野缩放
            if (modifiable.fovScaled) // 仅当启用时显示基础视角参数
            {
                ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
                changed |= ImGui::InputFloat("##base_fov", &baseFOV_f, 0.0f, 0.0f, "%.4f", ImGuiInputTextFlags_CharsDecimal); // 文本输入
                ImGui::SameLine(); if (ImGui::Button("-##base_fov")) { baseFOV_f = (std::max)(1.0f, baseFOV_f - 0.1f); changed = true; } // 减少
                ImGui::SameLine(); if (ImGui::Button("+##base_fov")) { baseFOV_f = (std::min)(200.0f, baseFOV_f + 0.1f); changed = true; } // 增加
                ImGui::SameLine(); ImGui::TextDisabled("基础视野 [1, 200]"); // 范围
                ShowSettingTooltip("基础视野");
                validateFloatSticky(&baseFOV_f, 1.0f, 200.0f, 90.0f); // 验证（修正即回写）
            }

            // 块: 检测到更改则将 float 转回 double 并保存
            if (changed) // 如果用户修改了任何参数
            {
                modifiable.sens = static_cast<double>(sens_f); // 转回 double
                modifiable.yaw = static_cast<double>(yaw_f);
                modifiable.pitch = static_cast<double>(pitch_f);
                modifiable.baseFOV = static_cast<double>(baseFOV_f);
                OverlayConfig_MarkDirty(); // 标记配置已更改（幂等，原重复调用已去除）
            }
        }

        ImGui::PopID();
    }

    if (shouldDrawMousePage(page, MouseSettingsPage::Profiles))
    {
        ImGui::PushID("mouse_section_manage_profiles");
        ImGui::SeparatorText("配置管理");
        static char new_profile_name[64] = "";
        bool addProfile = false;
        {
            ImGui::TextUnformatted("新配置名称");
            ImGui::SameLine();
            const float buttonW = UiLayout::kActionButtonWidth;
            ImGui::SetNextItemWidth(UiLayout::kTextMediumWidth);
            ImGui::InputText("##value", new_profile_name, sizeof(new_profile_name));
            ImGui::SameLine();
            addProfile = ImGui::Button("添加", ImVec2(buttonW, 0.0f));
            ShowSettingTooltip("新配置名称");
        }
        if (addProfile)
        {
            std::string name = std::string(new_profile_name);
            if (!name.empty() && config.game_profiles.count(name) == 0)
            {
                Config::GameProfile gp;
                gp.name = name;
                gp.sens = 1.0;
                gp.yaw = 0.022;
                gp.pitch = 0.022;
                gp.fovScaled = false;
                gp.baseFOV = 90.0;
                config.game_profiles[name] = gp;
                config.active_game = name;
                OverlayConfig_MarkDirty();
                new_profile_name[0] = '\0'; // clear
            }
        }

        const auto& gp = config.currentProfile();
        if (gp.name != "UNIFIED")
        {
            ImGui::PushID("delete_current_profile");
            if (ImGui::Button("删除当前配置", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
            {
                // currentProfile() 返回的是 game_profiles 内部元素的引用，gp.name 这块 std::string
                // 内存就位于将被销毁的节点里。直接把它当作 erase(key) 的实参属于"用容器自身元素
                // 删除自己"的 use-after-free 模式；先拷贝一份 key 脱离容器再删。
                const std::string profileNameToErase = gp.name;
                config.game_profiles.erase(profileNameToErase);
                if (config.game_profiles.count("UNIFIED") != 0)
                    config.active_game = "UNIFIED";
                else if (!config.game_profiles.empty())
                    config.active_game = config.game_profiles.begin()->first;
                else
                    config.active_game = "UNIFIED";

                OverlayConfig_MarkDirty();
            }
            ImGui::PopID();
        }

        ImGui::PopID();
    }

    if (shouldDrawMousePage(page, MouseSettingsPage::Input))
    {
        ImGui::PushID("mouse_section_input_method");
        // 常量表改为 static，避免每帧构造 2 个 vector<std::string>（8 次堆分配）。
        static const char* const kInputMethodLabels[] = { "WIN32(标准)", "KmboxNet", "KmboxA", "Makcu" };
        static const char* const kInputMethodValues[] = { "WIN32", "KMBOX_NET", "KMBOX_A", "MAKCU" };
        static constexpr int kInputMethodCount = static_cast<int>(IM_ARRAYSIZE(kInputMethodLabels));
        static_assert(IM_ARRAYSIZE(kInputMethodLabels) == IM_ARRAYSIZE(kInputMethodValues),
                      "输入法显示名与配置值必须一一对应");

        const char* const* method_items = kInputMethodLabels;

        int input_method_index = 0;
        for (int i = 0; i < kInputMethodCount; ++i)
        {
            if (config.input_method == kInputMethodValues[i])
            {
                input_method_index = i;
                break;
            }
        }

        ImGui::SetNextItemWidth(UiLayout::kComboLongWidth);
        if (ImGui::Combo("输入方式", &input_method_index, method_items, kInputMethodCount))
        {
            // ImGui::Combo 内部已对 current_item 做 [0, items_count) 守卫，此处再兜底一次。
            const int safeIndex = (input_method_index >= 0 && input_method_index < kInputMethodCount)
                                      ? input_method_index
                                      : 0;
            std::string new_input_method = kInputMethodValues[safeIndex];

            if (new_input_method != config.input_method)
            {
                config.input_method = new_input_method;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
        }

        if (config.input_method == "WIN32")
        {
            ImGui::Text("标准鼠标输入方式。建议用 Kmbox 或 Makcu 后端。");
            ImGui::TextDisabled("有风险，此方法可能被检测。");
        }
        else if (config.input_method == "KMBOX_NET")
        {
            static char ip[32] = "";
            static char port[8] = "";
            static char uuid[16] = "";
            static std::string last_ip;
            static std::string last_port;
            static std::string last_uuid;

            if (last_ip != config.kmbox_net_ip || last_port != config.kmbox_net_port || last_uuid != config.kmbox_net_uuid)
            {
                strncpy(ip, config.kmbox_net_ip.c_str(), sizeof(ip));
                strncpy(port, config.kmbox_net_port.c_str(), sizeof(port));
                strncpy(uuid, config.kmbox_net_uuid.c_str(), sizeof(uuid));
                ip[sizeof(ip) - 1] = '\0';
                port[sizeof(port) - 1] = '\0';
                uuid[sizeof(uuid) - 1] = '\0';
                last_ip = config.kmbox_net_ip;
                last_port = config.kmbox_net_port;
                last_uuid = config.kmbox_net_uuid;
            }

            ImGui::SetNextItemWidth(UiLayout::kTextShortWidth);
            ImGui::InputText("IP地址", ip, sizeof(ip));
            ImGui::SetNextItemWidth(UiLayout::kTextShortWidth);
            ImGui::InputText("端口", port, sizeof(port));
            ImGui::SetNextItemWidth(UiLayout::kTextShortWidth);
            ImGui::InputText("UUID", uuid, sizeof(uuid));

            ImGui::PushID("kmbox_net_save_reconnect");
            if (ImGui::Button("保存并重连", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
            {
                config.kmbox_net_ip = ip;
                config.kmbox_net_port = port;
                config.kmbox_net_uuid = uuid;
                last_ip = config.kmbox_net_ip;
                last_port = config.kmbox_net_port;
                last_uuid = config.kmbox_net_uuid;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
            ImGui::PopID();

            bool kmboxNetConnected = false;
            bool kmboxNetConnecting = false;
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxNetConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                    ? activeMouseInputOwner->kmboxNet()
                    : nullptr;
                kmboxNetConnected = device && device->isOpen();
                kmboxNetConnecting = activeMouseInputOwner &&
                    std::string(activeMouseInputOwner->name()) == "KMBOX_NET" &&
                    activeMouseInputOwner->isConnecting();
            }

            if (kmboxNetConnected)
            {
                ImGui::Text("kmboxNet已连接");
            }
            else if (kmboxNetConnecting)
            {
                ImGui::TextDisabled("kmboxNet连接中");
            }
            else
            {
                ImGui::TextDisabled("kmboxNet未连接");
            }

              ImGui::PushID("kmbox_net_reboot");
              if (!kmboxNetConnected)
                  ImGui::BeginDisabled();
              if (ImGui::Button("重启设备", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
              {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxNetConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                    ? activeMouseInputOwner->kmboxNet()
                    : nullptr;
                if (device && device->isOpen())
                      device->reboot();
              }
              if (!kmboxNetConnected)
                  ImGui::EndDisabled();
              ImGui::PopID();

               ImGui::PushID("kmbox_net_release_all");
               if (ImGui::Button("释放所有按键，取消遮罩", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
              {
                  std::lock_guard<std::mutex> lock(inputDevicesMutex);
                  KmboxNetConnection* device =
                      activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                      ? activeMouseInputOwner->kmboxNet()
                      : nullptr;
                  // 断线/掉 ACK 时仍允许用户手动释放：不要求 device->isOpen()。
                  if (device)
                       device->releaseAllButtons();
               }
               ImGui::PopID();

               ImGui::PushID("kmbox_net_image");
               if (!kmboxNetConnected)
                   ImGui::BeginDisabled();
               if (ImGui::Button("更换图像", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxNetConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                    ? activeMouseInputOwner->kmboxNet()
                    : nullptr;
                if (device && device->isOpen())
                {
                    device->lcdColor(0);
                     device->lcdPicture(gImage_128x160);
                 }
             }
             if (!kmboxNetConnected)
                 ImGui::EndDisabled();
             ImGui::PopID();
        }
        else if (config.input_method == "KMBOX_A")
        {
            static char pidvid[32] = "";
            static std::string last_pidvid;

            if (last_pidvid != config.kmbox_a_pidvid)
            {
                strncpy(pidvid, config.kmbox_a_pidvid.c_str(), sizeof(pidvid));
                pidvid[sizeof(pidvid) - 1] = '\0';
                last_pidvid = config.kmbox_a_pidvid;
            }

            ImGui::SetNextItemWidth(UiLayout::kTextShortWidth);
            ImGui::InputText("PIDVID", pidvid, sizeof(pidvid));
            ImGui::TextDisabled("格式: PPPPVVVV（单字段）");

            ImGui::PushID("kmbox_a_save_reconnect");
            if (ImGui::Button("保存并重连", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
            {
                config.kmbox_a_pidvid = pidvid;
                last_pidvid = config.kmbox_a_pidvid;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
            ImGui::PopID();

            // 连接状态必须在 inputDevicesMutex 下、经 activeMouseInputOwner 查询。
            // 直接读裸全局 kmboxASerial 会与鼠标线程的 createInputDevices() 竞争：
            // 后者先在锁内把该全局置空，再在锁外 reset() 析构 KmboxAConnection；
            // 无锁读者可能已取到旧的非空指针，随后对已析构对象调用 isOpen()（UAF）。
            bool kmboxAConnected = false;
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxAConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_A"
                    ? activeMouseInputOwner->kmboxA()
                    : nullptr;
                kmboxAConnected = device && device->isOpen();
            }

            if (kmboxAConnected)
            {
                ImGui::Text("kmboxA已连接");
            }
            else
            {
                ImGui::TextDisabled("kmboxA未连接");
            }
        }
        else if (config.input_method == "MAKCU")
        {
            // 串口/波特率均为固定枚举，改为进程级只构造一次的静态表，
            // 消除每帧 30 次 "COM"+to_string 堆分配与 5 个 vector 的构造/析构。
            static constexpr int kMakcuPortCount = 30;
            static const char* const* const kMakcuPortItems = []() -> const char* const* {
                static std::string storage[kMakcuPortCount];
                static const char* items[kMakcuPortCount];
                for (int i = 0; i < kMakcuPortCount; ++i)
                {
                    storage[i] = "COM" + std::to_string(i + 1);
                    items[i] = storage[i].c_str();
                }
                return items;
            }();

            int port_index = 0;
            for (int i = 0; i < kMakcuPortCount; ++i)
            {
                if (config.makcu_port == kMakcuPortItems[i])
                {
                    port_index = i;
                    break;
                }
            }

            ImGui::SetNextItemWidth(UiLayout::kComboShortWidth);
            if (ImGui::Combo("Makcu端口", &port_index, kMakcuPortItems, kMakcuPortCount))
            {
                if (port_index >= 0 && port_index < kMakcuPortCount)
                {
                    config.makcu_port = kMakcuPortItems[port_index];
                    OverlayConfig_MarkDirty();
                    input_method_changed.store(true);
                }
            }

            static constexpr int kMakcuBaudList[] = { 9600, 19200, 38400, 57600, 115200 };
            static constexpr int kMakcuBaudCount = static_cast<int>(IM_ARRAYSIZE(kMakcuBaudList));
            static const char* const kMakcuBaudItems[kMakcuBaudCount] = {
                "9600", "19200", "38400", "57600", "115200"
            };

            int baud_index = 0;
            for (int i = 0; i < kMakcuBaudCount; ++i)
            {
                if (kMakcuBaudList[i] == config.makcu_baudrate)
                {
                    baud_index = i;
                    break;
                }
            }

            ImGui::SetNextItemWidth(UiLayout::kComboShortWidth);
            if (ImGui::Combo("Makcu波特率", &baud_index, kMakcuBaudItems, kMakcuBaudCount))
            {
                if (baud_index >= 0 && baud_index < kMakcuBaudCount)
                {
                    config.makcu_baudrate = kMakcuBaudList[baud_index];
                    OverlayConfig_MarkDirty();
                    input_method_changed.store(true);
                }
            }

            // 同 KMBOX_A：经 inputDevicesMutex + activeMouseInputOwner 查询，
            // 避免与 createInputDevices() 的"锁内置空 / 锁外析构"序列构成 UAF。
            bool makcuConnected = false;
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                MakcuConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "MAKCU"
                    ? activeMouseInputOwner->makcu()
                    : nullptr;
                makcuConnected = device && device->isOpen();
            }

            if (makcuConnected)
            {
                ImGui::Text("Makcu已连接");
            }
            else
            {
                ImGui::TextDisabled("Makcu未连接");
            }
        }

        ImGui::PushID("mouse_section_input_test");
        ImGui::SeparatorText("后端动作测试");
            static bool test_running = false;
            static ULONGLONG next_test_time = 0;
            static std::string test_backend;
            static std::string test_result = "尚未测试";
            static ULONGLONG test_cooldown_until = 0;
            constexpr ULONGLONG testStartDelayMs = 500;
            constexpr ULONGLONG testCooldownMs = 3000;

            const auto now = GetTickCount64();
            auto withActiveInput = [](const auto& action) -> bool {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                if (!activeMouseInputOwner || !activeMouseInputOwner->isOpen())
                    return false;
                return action(*activeMouseInputOwner);
            };
            auto startMovementTest = [&]() {
                if (now < test_cooldown_until)
                {
                    const auto remainingSeconds = (test_cooldown_until - now + 999) / 1000;
                    test_result = "测试冷却中，还需 " + std::to_string(remainingSeconds) + " 秒";
                    return;
                }

                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                if (!activeMouseInputOwner || !activeMouseInputOwner->isOpen())
                {
                    test_result = "无法开始：当前后端未连接";
                    return;
                }
                test_running = true;
                next_test_time = now + testStartDelayMs;
                test_backend = activeMouseInputOwner->name();
                test_cooldown_until = now + testCooldownMs;
                test_result = "短距离移动测试将在 0.5 秒后开始，请将鼠标移到安全位置";
            };

            ImGui::TextDisabled("仅发送一次相对移动 (60, 60)。成功表示当前后端接受了移动命令，不代表设备有物理反馈。");
            ImGui::Text("当前后端: %s", config.input_method.c_str());
            ImGui::TextWrapped("测试结果: %s", test_result.c_str());

            const bool test_cooldown_active = now < test_cooldown_until;
            if (test_cooldown_active)
            {
                const auto remainingSeconds = (test_cooldown_until - now + 999) / 1000;
                ImGui::TextDisabled("冷却中：%llu 秒后可再启测试",
                    static_cast<unsigned long long>(remainingSeconds));
            }

            if (!test_running)
            {
                if (test_cooldown_active)
                    ImGui::BeginDisabled();

                if (ImGui::Button("短距离移动测试", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
                    startMovementTest();

                if (test_cooldown_active)
                    ImGui::EndDisabled();
            }
            else
            {
                if (test_backend != config.input_method)
                {
                    test_running = false;
                    test_result = "测试已停止：输入后端发生切换，请重新开始测试";
                }
                if (test_running && ImGui::Button("停止测试", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
                {
                    test_running = false;
                    test_result = "测试已手动停止";
                }

                if (test_running && now >= next_test_time)
                {
                    const bool actionOk = withActiveInput([](IMouseInput& input) {
                        return input.move(60, 60);
                    });
                    test_running = false;
                    test_result = actionOk
                        ? "短距离移动命令已发送"
                        : "移动发送失败：未连接或后端拒绝命令";
                }
            }

        ImGui::PopID();
        ImGui::PopID();
    }

    if (prev_fovX != config.fovX ||
        prev_fovY != config.fovY ||
        prev_minSpeedMultiplier != config.minSpeedMultiplier ||
        prev_maxSpeedMultiplier != config.maxSpeedMultiplier ||
        prev_predictionInterval != config.predictionInterval ||
        prev_kalman_enabled != config.kalman_enabled ||
        prev_kalman_process_noise_position != config.kalman_process_noise_position ||
        prev_kalman_process_noise_velocity != config.kalman_process_noise_velocity ||
        prev_kalman_measurement_noise != config.kalman_measurement_noise ||
        prev_kalman_velocity_damping != config.kalman_velocity_damping ||
        prev_kalman_max_velocity != config.kalman_max_velocity ||
        prev_kalman_warmup_frames != config.kalman_warmup_frames ||
        prev_kalman_compensate_detection_delay != config.kalman_compensate_detection_delay ||
        prev_kalman_additional_prediction_ms != config.kalman_additional_prediction_ms ||
        prev_kalman_reset_timeout_sec != config.kalman_reset_timeout_sec ||
        prev_snapRadius != config.snapRadius ||
        prev_nearRadius != config.nearRadius ||
        prev_speedCurveExponent != config.speedCurveExponent ||
        prev_snapBoostFactor != config.snapBoostFactor)
    {
        prev_fovX = config.fovX;
        prev_fovY = config.fovY;
        prev_minSpeedMultiplier = config.minSpeedMultiplier;
        prev_maxSpeedMultiplier = config.maxSpeedMultiplier;
        prev_predictionInterval = config.predictionInterval;
        prev_kalman_enabled = config.kalman_enabled;
        prev_kalman_process_noise_position = config.kalman_process_noise_position;
        prev_kalman_process_noise_velocity = config.kalman_process_noise_velocity;
        prev_kalman_measurement_noise = config.kalman_measurement_noise;
        prev_kalman_velocity_damping = config.kalman_velocity_damping;
        prev_kalman_max_velocity = config.kalman_max_velocity;
        prev_kalman_warmup_frames = config.kalman_warmup_frames;
        prev_kalman_compensate_detection_delay = config.kalman_compensate_detection_delay;
        prev_kalman_additional_prediction_ms = config.kalman_additional_prediction_ms;
        prev_kalman_reset_timeout_sec = config.kalman_reset_timeout_sec;
        prev_snapRadius = config.snapRadius;
        prev_nearRadius = config.nearRadius;
        prev_speedCurveExponent = config.speedCurveExponent;
        prev_snapBoostFactor = config.snapBoostFactor;

        notifyMouseThreadConfig(); // 统一推送入口（内含 globalMouseThread 空指针防护）

        OverlayConfig_MarkDirty();
    }

    if (prev_curve_enabled != config.curve_enabled ||
        prev_curve_intensity != config.curve_intensity ||
        prev_perturbation_strength != config.perturbation_strength ||
        prev_curve_max_speed != config.curve_max_speed ||
        prev_curve_distance != config.curve_distance)
    {
        prev_curve_enabled = config.curve_enabled;
        prev_curve_intensity = config.curve_intensity;
        prev_perturbation_strength = config.perturbation_strength;
        prev_curve_max_speed = config.curve_max_speed;
        prev_curve_distance = config.curve_distance;

        notifyMouseThreadConfig(); // 统一推送入口（内含 globalMouseThread 空指针防护）

        OverlayConfig_MarkDirty();
    }

    if (prev_auto_shoot != config.auto_shoot ||
        prev_bScope_multiplier != config.bScope_multiplier)
    {
        prev_auto_shoot = config.auto_shoot;
        prev_bScope_multiplier = config.bScope_multiplier;

        notifyMouseThreadConfig(); // 统一推送入口（内含 globalMouseThread 空指针防护）

        OverlayConfig_MarkDirty();
    }
}

// 函数: draw_mouse
// 作用: 绘制鼠标设置主面板，显示所有鼠标相关配置选项。
// 说明: 这是从菜单调用的主入口，负责展示完整的鼠标参数界面。
void draw_mouse()
{
    draw_mouse_page(MouseSettingsPage::All); // 显示所有内容（非过滤）
}

// 函数: draw_mouse_movement
// 作用: 绘制鼠标移动/反应速度相关的设置面板。
// 说明: 仅显示与鼠标运动相关的参数，如灵敏度、加速等。
void draw_mouse_movement()
{
    draw_mouse_page(MouseSettingsPage::Movement); // 仅显示移动相关内容
}

// 函数: draw_mouse_prediction
// 作用: 绘制目标预测与轨迹跟随的设置面板。
// 说明: 仅显示与预测逻辑相关的参数，如预测间隔、Kalman 滤波等。
void draw_mouse_prediction()
{
    draw_mouse_page(MouseSettingsPage::Prediction); // 仅显示预测相关内容
}

// 函数: draw_mouse_assist
// 作用: 绘制鼠标辅助工具的设置面板。
// 说明: 仅显示修饰性功能，如曲线、扰动、吸附等辅助参数。
void draw_mouse_assist()
{
    draw_mouse_page(MouseSettingsPage::Assist); // 仅显示辅助相关内容
}

// 函数: draw_mouse_profiles
// 作用: 绘制游戏配置管理面板。
// 说明: 允许用户创建、切换和编辑游戏特定的配置文件。
void draw_mouse_profiles()
{
    draw_mouse_page(MouseSettingsPage::Profiles); // 仅显示配置管理相关内容
}

// 函数: draw_mouse_input
// 作用: 绘制输入设备设置面板。
// 说明: 配置鼠标、键盘等输入设备的后端参数。
void draw_mouse_input()
{
    draw_mouse_page(MouseSettingsPage::Input); // 仅显示输入设备相关内容
}

// 函数: draw_hotkey_profile
// 作用: 绘制指定热键槽的本地配置编辑面板。
// 参数: slot - 热键槽索引（0、1、2），对应三个可配置的鼠标热键
// 说明: 允许用户为每个热键单独配置目标锁定、灵敏度、触发器等参数。
void draw_hotkey_profile(std::size_t slot)
{
    if (slot >= Config::MAX_MOUSE_HOTKEYS) // 如果槽索引超出范围
        return; // 立即返回，不绘制任何内容

    auto& profile = config.mouse_hotkeys[slot];
    ImGui::PushID(static_cast<int>(slot));
    bool profileEnabled = profile.enabled;
        if (ImGui::Button(profileEnabled ? "关闭热键" : "激活热键", ImVec2(UiLayout::kActionButtonWidth, 0.0f)))
    {
        profile.enabled = !profile.enabled;
        profileEnabled = profile.enabled;
        OverlayConfig_MarkDirty();
    }
    ImGui::SameLine();
    ImGui::Text(profileEnabled ? "热键 %zu：已激活，显示局部参数" : "热键 %zu：已关闭，隐藏局部参数", slot + 1);
    if (!profileEnabled)
    {
        ImGui::PopID();
        return;
    }


    int buttonIndex = 0;
    static const char* buttonLabels[] = { "左键", "右键", "中键", "侧键 1", "侧键 2" };
    static const char* buttonValues[] = { "LeftMouseButton", "RightMouseButton", "MiddleMouseButton", "X1MouseButton", "X2MouseButton" };
    if (!profile.buttons.empty())
    {
        for (int i = 0; i < 5; ++i)
            if (profile.buttons.front() == buttonValues[i]) buttonIndex = i;
    }
    ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth);
    if (ImGui::Combo("绑定鼠标键", &buttonIndex, buttonLabels, 5))
    {
        bool duplicate = false;
        for (std::size_t i = 0; i < Config::MAX_MOUSE_HOTKEYS; ++i)
        {
            if (i != slot && !config.mouse_hotkeys[i].buttons.empty() &&
                config.mouse_hotkeys[i].buttons.front() == buttonValues[buttonIndex])
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            profile.buttons = {buttonValues[buttonIndex]};
            OverlayConfig_MarkDirty();
        }
    }

    ImGui::TextDisabled("固定优先级：热键1>2>3  以下所有参数仅对当前热键生效。");

    // 说明: 控件 ID 改用 ImGui::PushID(key) 隔离 + 固定字面量标签，替代原先每个控件
    // 3 次 `std::string("##") + key` 拼接。ID 依然唯一（PushID 参与 ID hash 种子），
    // 但消除了热路径上的堆分配（本面板 20+ 控件 × 3 次/帧）。
    // 步进按钮同时就地钳制，避免"点一次越界再被 clamp"的中间态与提示抖动。
    // 原 numericButtonWidth 计算结果从未被任何控件使用（死代码 + 每帧 2 次 CalcTextSize），已删除。
    auto localIntInput = [&](const char* label, const char* key, int minimum, int maximum, int fallback)
    {
        int value = profile.localInt(key, fallback);
        ImGui::PushID(key);
        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        bool changed = ImGui::InputInt("##v", &value, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); if (ImGui::Button("-")) { value = (std::max)(minimum, value - 1); changed = true; }
        ImGui::SameLine(); if (ImGui::Button("+")) { value = (std::min)(maximum, value + 1); changed = true; }
        ImGui::SameLine(); ImGui::TextDisabled("%s [%d, %d]", label, minimum, maximum);
        ShowSettingTooltip(label);
        ImGui::PopID();
        if (changed)
        {
            profile.setLocalInt(key, std::clamp(value, minimum, maximum));
            OverlayConfig_MarkDirty();
        }
    };
    // 原形参 showDomain 在 lambda 体内从未被引用（死参数），唯一调用方传 true 也无任何效果，已删除。
    auto localFloatInput = [&](const char* label, const char* key, float minimum, float maximum, float fallback)
    {
        float value = profile.localFloat(key, fallback);
        ImGui::PushID(key);
        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        bool changed = ImGui::InputFloat("##v", &value, 0.0f, 0.0f);
        const float step = maximum - minimum <= 1.0f ? 0.01f : 0.1f;
        ImGui::SameLine(); if (ImGui::Button("-")) { value = (std::max)(minimum, value - step); changed = true; }
        ImGui::SameLine(); if (ImGui::Button("+")) { value = (std::min)(maximum, value + step); changed = true; }
        ImGui::SameLine(); ImGui::TextDisabled("%s [%.4g, %.4g]", label, minimum, maximum);
        ShowSettingTooltip(label);
        ImGui::PopID();
        if (changed)
        {
            profile.setLocalFloat(key, std::clamp(value, minimum, maximum));
            OverlayConfig_MarkDirty();
        }
    };
    localFloatInput("置信度阈值", "confidence_threshold", 0.1f, 0.9f, config.confidence_threshold);
    localFloatInput("NMS 阈值", "nms_threshold", 0.1f, 0.9f, config.nms_threshold);
    localIntInput("敌人检测数", "max_detections", 1, 20, config.max_detections);

    bool localAutoAim = profile.localBool("auto_aim", config.auto_aim);
    const float aimControlCheckboxWidth = ImGui::GetFrameHeight() * 2.0f +
        ImGui::GetStyle().ItemInnerSpacing.x * 2.0f + ImGui::CalcTextSize("自动瞄准开关").x +
        ImGui::CalcTextSize("保持目标锁定").x + ImGui::GetStyle().ItemSpacing.x;
    const bool fitAimControlCheckboxes = ImGui::GetContentRegionAvail().x >= aimControlCheckboxWidth;
    if (ImGui::Checkbox("自动瞄准的总开关", &localAutoAim))
    {
        profile.setLocalBool("auto_aim", localAutoAim);
        OverlayConfig_MarkDirty();
    }
    bool localTracker = profile.localBool("tracker_enabled", config.tracker_enabled);
    if (fitAimControlCheckboxes)
        ImGui::SameLine();
    if (ImGui::Checkbox("保持目标锁定不跳", &localTracker))
    {
        profile.setLocalBool("tracker_enabled", localTracker);
        OverlayConfig_MarkDirty();
    }
    int targetingMode = profile.localString("targeting_mode", config.targeting_mode) == "largest_box" ? 1 : 0;
    const char* targetingModes[] = { "中心最近", "方框最大" };
    ImGui::SetNextItemWidth(UiLayout::kComboMediumWidth);
    if (ImGui::Combo("瞄准目标的方式", &targetingMode, targetingModes, 2))
    {
        profile.setLocalString("targeting_mode", targetingMode == 1 ? "largest_box" : "closest_center");
        OverlayConfig_MarkDirty();
    }


    // ---- MouseController 参数面板 (依据 鼠标调参指南.md) ----
    // 直接写 config.mc_*, 经 syncMouseController() 推送给算法;
    // 不写 profile.localXxx, 避免制造幽灵控件。
    if (ImGui::CollapsingHeader("MouseController 参数", ImGuiTreeNodeFlags_None))
    {
        // 本地浮点输入助手: 写 config.mc_* + MarkDirty (带 -/+ 步进与范围钳制)
        // 控件 ID 改用 PushID(id) + 固定字面量，替代原实现每次调用构造
        // "##mc_" + id / "-##mc_" + id / "+##mc_" + id 三个 std::string（20 控件 × 3 次/帧堆分配）。
        // ID 唯一性由 PushID(id) 参与 hash 种子保证，行为不变。
        auto mcFloat = [&](const char* label, const char* id, float& ref, float minV, float maxV, float stepV) {
            float v = ref;
            ImGui::PushID(id);
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool changed = ImGui::InputFloat("##v", &v, 0.0f, 0.0f);
            ImGui::SameLine(); if (ImGui::Button("-")) { v -= stepV; changed = true; }
            ImGui::SameLine(); if (ImGui::Button("+")) { v += stepV; changed = true; }
            ImGui::SameLine(); ImGui::TextDisabled("%s [%.4g, %.4g]", label, minV, maxV);
            ShowSettingTooltip(label);
            ImGui::PopID();
            if (changed) { ref = std::clamp(v, minV, maxV); OverlayConfig_MarkDirty(); }
        };

        bool localMc = config.mc_enabled;
        if (ImGui::Checkbox("启用MouseController模块(总开关)", &localMc))
        {
            config.mc_enabled = localMc;
            OverlayConfig_MarkDirty();
        }

        if (ImGui::TreeNodeEx("X 轴调参", ImGuiTreeNodeFlags_DefaultOpen))
        {
            mcFloat("追踪强度 tracking", "x_track", config.mc_x_tracking, 2.0f, 6.0f, 0.5f);
            mcFloat("震荡抑制 damping",  "x_damp", config.mc_x_damping, 0.02f, 0.12f, 0.01f);
            mcFloat("最大速度 maxSpeed", "x_speed", config.mc_x_maxspeed, 800.0f, 3000.0f, 100.0f);
            mcFloat("积分增益 integral", "x_int", config.mc_x_integral, 0.0f, 2.0f, 0.05f);
            mcFloat("死区 deadzone",     "x_dz", config.mc_x_deadzone, 1.0f, 5.0f, 1.0f);
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Y 轴调参", ImGuiTreeNodeFlags_DefaultOpen))
        {
            mcFloat("追踪强度 tracking", "y_track", config.mc_y_tracking, 2.0f, 6.0f, 0.5f);
            mcFloat("震荡抑制 damping",  "y_damp", config.mc_y_damping, 0.02f, 0.12f, 0.01f);
            mcFloat("最大速度 maxSpeed", "y_speed", config.mc_y_maxspeed, 800.0f, 3000.0f, 100.0f);
            mcFloat("积分增益 integral", "y_int", config.mc_y_integral, 0.0f, 2.0f, 0.05f);
            mcFloat("死区 deadzone",     "y_dz", config.mc_y_deadzone, 1.0f, 5.0f, 1.0f);
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("全局参数", ImGuiTreeNodeFlags_DefaultOpen))
        {
            mcFloat("单帧最大移动 maxStepPerFrame", "g_maxstep", config.mc_maxstep, 15.0f, 50.0f, 5.0f);
            mcFloat("重瞄准阈值 retargetThreshold",  "g_retarget", config.mc_retarget, 10.0f, 200.0f, 5.0f);
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("算法参数", ImGuiTreeNodeFlags_DefaultOpen))
        {
            mcFloat("自适应前瞻 最小(s)", "a_min", config.mc_ahead_min, 0.02f, 0.15f, 0.01f);
            mcFloat("自适应前瞻 最大(s)", "a_max", config.mc_ahead_max, 0.05f, 0.30f, 0.01f);
            mcFloat("轨迹时长 最小(s)",   "d_min", config.mc_dur_min, 0.10f, 0.60f, 0.01f);
            mcFloat("轨迹时长 最大(s)",   "d_max", config.mc_dur_max, 0.15f, 1.00f, 0.01f);
            mcFloat("Kalman 过程噪声 q",  "k_q", config.mc_kalman_q, 100.0f, 5000.0f, 100.0f);
            mcFloat("Kalman 测量噪声 r",  "k_r", config.mc_kalman_r, 5.0f, 200.0f, 5.0f);
            ImGui::TreePop();
        }
    }

    bool localRecoil = profile.localBool("easynorecoil", config.easynorecoil);
    if (ImGui::Checkbox("开启压枪功能", &localRecoil))
    {
        profile.setLocalBool("easynorecoil", localRecoil);
        OverlayConfig_MarkDirty();
    }
    if (localRecoil)
        localFloatInput("调整压枪力度", "easynorecoilstrength", 0.0f, 100.0f, config.easynorecoilstrength);

    bool localDynamicRange = profile.localBool("dynamic_range_enabled", config.dynamic_range_enabled);
    if (ImGui::Checkbox("启用动态范围##local", &localDynamicRange))
    {
        profile.setLocalBool("dynamic_range_enabled", localDynamicRange);
        OverlayConfig_MarkDirty();
    }
    if (localDynamicRange)
    {
        int shrinkScope = profile.localInt("dynamic_range_shrink_scope", config.dynamic_range_shrink_scope);
        ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
        bool shrinkScopeChanged = ImGui::InputInt("##dynamic_range_shrink_scope", &shrinkScope, 0, 0);
        ImGui::SameLine(); if (ImGui::Button("-##dynamic_range_shrink_scope")) { --shrinkScope; shrinkScopeChanged = true; }
        ImGui::SameLine(); if (ImGui::Button("+##dynamic_range_shrink_scope")) { ++shrinkScope; shrinkScopeChanged = true; }
        ImGui::SameLine(); ImGui::TextDisabled("动态范围缩小范围(px) [10, 640]");
        ShowSettingTooltip("动态范围缩小范围(px)");
        if (shrinkScopeChanged)
        {
            profile.setLocalInt("dynamic_range_shrink_scope", std::clamp(shrinkScope, 10, 640));
            OverlayConfig_MarkDirty();
        }
        localIntInput("动态范围收缩时长(ms)", "dynamic_range_shrink_duration_ms", 50, 2000,
                      config.dynamic_range_shrink_duration_ms);
        localIntInput("动态范围恢复时长(ms)", "dynamic_range_restore_duration_ms", 50, 2000,
                      config.dynamic_range_cooldown_ms);
    }

    ImGui::TextDisabled("只能从全局已启用类别中选择；全部取消后当前热键不锁定任何类别。");
    if (!g_hotkeyClassKeys.built)
        g_hotkeyClassKeys.build(); // 首次进入时一次性预构建全部类别键名（跨帧复用，零堆分配）
    std::vector<int> classOrder;
    classOrder.reserve(static_cast<std::size_t>(Config::FIXED_TARGET_CLASS_COUNT));
    for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
        classOrder.push_back(cls);
    std::stable_sort(classOrder.begin(), classOrder.end(), [&](int lhs, int rhs) {
        return profile.localInt(g_hotkeyClassKeys.order[static_cast<std::size_t>(lhs)], lhs) <
            profile.localInt(g_hotkeyClassKeys.order[static_cast<std::size_t>(rhs)], rhs);
    });
    for (int orderIndex = 0; orderIndex < Config::FIXED_TARGET_CLASS_COUNT; ++orderIndex)
    {
        const int cls = classOrder[static_cast<std::size_t>(orderIndex)];
        if (!config.isClassEnabled(cls))
            continue;
        bool enabled = profile.localBool(g_hotkeyClassKeys.enabled[static_cast<std::size_t>(cls)], false);
        ImGui::PushID(cls);
        // 优化：原每帧为 15 个类别构造 std::string（"类别N"）触发堆分配。
        // 改用栈上定长缓冲区，零分配。
        char classLabel[16];
        snprintf(classLabel, sizeof(classLabel), "类别%d", cls);
        ImGui::TextUnformatted(classLabel);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            ImGui::SetDragDropPayload("AIMBOT_HOTKEY_CLASS_ORDER", &cls, sizeof(cls));
            ImGui::Text("移动 类别%d", cls);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("AIMBOT_HOTKEY_CLASS_ORDER"))
            {
                const int draggedClass = *static_cast<const int*>(payload->Data);
                const auto draggedIt = std::find(classOrder.begin(), classOrder.end(), draggedClass);
                const auto targetIt = std::find(classOrder.begin(), classOrder.end(), cls);
                if (draggedIt != classOrder.end() && targetIt != classOrder.end() && draggedClass != cls)
                {
                    const int draggedIndex = static_cast<int>(std::distance(classOrder.begin(), draggedIt));
                    const int targetIndex = static_cast<int>(std::distance(classOrder.begin(), targetIt));
                    std::rotate(classOrder.begin() + std::min(draggedIndex, targetIndex),
                                classOrder.begin() + (draggedIndex < targetIndex ? draggedIndex + 1 : draggedIndex),
                                classOrder.begin() + std::max(draggedIndex, targetIndex) + 1);
                    for (int i = 0; i < Config::FIXED_TARGET_CLASS_COUNT; ++i)
                        profile.setLocalInt(g_hotkeyClassKeys.order[static_cast<std::size_t>(classOrder[static_cast<std::size_t>(i)])], i);
                    OverlayConfig_MarkDirty();
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("启用##class_enabled", &enabled))
        {
            profile.setLocalBool(g_hotkeyClassKeys.enabled[static_cast<std::size_t>(cls)], enabled);
            OverlayConfig_MarkDirty();
        }
        if (enabled && ImGui::TreeNodeEx("目标瞄准点##class_aim_point", ImGuiTreeNodeFlags_SpanAvailWidth))
        {
            const std::size_t clsIdx = static_cast<std::size_t>(cls);
            float aimX = profile.localFloat(g_hotkeyClassKeys.aimX[clsIdx], 0.5f);
            float aimY = profile.localFloat(g_hotkeyClassKeys.aimY[clsIdx], 0.5f);
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool aimXChanged = ImGui::InputFloat("##class_aim_offset_x", &aimX, 0.0f, 0.0f);
            ImGui::SameLine(); if (ImGui::Button("-##class_aim_offset_x")) { aimX -= 0.01f; aimXChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##class_aim_offset_x")) { aimX += 0.01f; aimXChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("目标瞄准点 X [0, 1]");
            if (aimXChanged)
            {
                profile.setLocalFloat(g_hotkeyClassKeys.aimX[clsIdx], std::clamp(aimX, 0.0f, 1.0f));
                OverlayConfig_MarkDirty();
            }
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool aimYChanged = ImGui::InputFloat("##class_aim_offset_y", &aimY, 0.0f, 0.0f);
            ImGui::SameLine(); if (ImGui::Button("-##class_aim_offset_y")) { aimY -= 0.01f; aimYChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##class_aim_offset_y")) { aimY += 0.01f; aimYChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("目标瞄准点 Y [0, 1]");
            if (aimYChanged)
            {
                profile.setLocalFloat(g_hotkeyClassKeys.aimY[clsIdx], std::clamp(aimY, 0.0f, 1.0f));
                OverlayConfig_MarkDirty();
            }

            float zoneX = profile.localFloat(g_hotkeyClassKeys.zoneX[clsIdx], 0.1f);
            float zoneY = profile.localFloat(g_hotkeyClassKeys.zoneY[clsIdx], 0.1f);
            float zoneW = profile.localFloat(g_hotkeyClassKeys.zoneW[clsIdx], 0.8f);
            float zoneH = profile.localFloat(g_hotkeyClassKeys.zoneH[clsIdx], 0.8f);
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool zoneXChanged = ImGui::InputFloat("##trigger_zone_offset_x", &zoneX, 0.0f, 0.0f);
            ImGui::SameLine(); if (ImGui::Button("-##trigger_zone_offset_x")) { zoneX -= 0.01f; zoneXChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##trigger_zone_offset_x")) { zoneX += 0.01f; zoneXChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("扳机区域左边界 X [0, 1]");
            if (zoneXChanged)
            {
                profile.setLocalFloat(g_hotkeyClassKeys.zoneX[clsIdx], std::clamp(zoneX, 0.0f, 1.0f));
                OverlayConfig_MarkDirty();
            }
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool zoneYChanged = ImGui::InputFloat("##trigger_zone_offset_y", &zoneY, 0.0f, 0.0f);
            ImGui::SameLine(); if (ImGui::Button("-##trigger_zone_offset_y")) { zoneY -= 0.01f; zoneYChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##trigger_zone_offset_y")) { zoneY += 0.01f; zoneYChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("扳机区域上边界 Y [0, 1]");
            if (zoneYChanged)
            {
                profile.setLocalFloat(g_hotkeyClassKeys.zoneY[clsIdx], std::clamp(zoneY, 0.0f, 1.0f));
                OverlayConfig_MarkDirty();
            }
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool zoneWChanged = ImGui::InputFloat("##trigger_zone_size_x", &zoneW, 0.0f, 0.0f);
            ImGui::SameLine(); if (ImGui::Button("-##trigger_zone_size_x")) { zoneW -= 0.01f; zoneWChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##trigger_zone_size_x")) { zoneW += 0.01f; zoneWChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("扳机区域大小 X [0.01, 1]");
            if (zoneWChanged)
            {
                profile.setLocalFloat(g_hotkeyClassKeys.zoneW[clsIdx], std::clamp(zoneW, 0.01f, 1.0f));
                OverlayConfig_MarkDirty();
            }
            ImGui::SetNextItemWidth(UiLayout::kNumericWidth);
            bool zoneHChanged = ImGui::InputFloat("##trigger_zone_size_y", &zoneH, 0.0f, 0.0f);
            ImGui::SameLine(); if (ImGui::Button("-##trigger_zone_size_y")) { zoneH -= 0.01f; zoneHChanged = true; }
            ImGui::SameLine(); if (ImGui::Button("+##trigger_zone_size_y")) { zoneH += 0.01f; zoneHChanged = true; }
            ImGui::SameLine(); ImGui::TextDisabled("扳机区域大小 Y [0.01, 1]");
            if (zoneHChanged)
            {
                profile.setLocalFloat(g_hotkeyClassKeys.zoneH[clsIdx], std::clamp(zoneH, 0.01f, 1.0f));
                OverlayConfig_MarkDirty();
            }
            const bool aimPointInsideTriggerZone =
                aimX >= zoneX && aimX <= zoneX + zoneW &&
                aimY >= zoneY && aimY <= zoneY + zoneH;
            if (!aimPointInsideTriggerZone)
                ImGui::TextDisabled(
                    "警告：目标瞄准点位于当前扳机矩形外，违背了想用扳机的目的，建议重新修改扳机区域。");

            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    ImGui::SeparatorText("扳机功能区");
    TriggerConfig trigger = config.trigger_targeting;
    bool triggerEnabled = profile.localBool("trigger_enabled", trigger.enabled);
    if (ImGui::Checkbox("开启扳机功能", &triggerEnabled)) { profile.setLocalBool("trigger_enabled", triggerEnabled); OverlayConfig_MarkDirty(); }
    if (triggerEnabled)
    {
        bool continuous = profile.localBool("trigger_continuous", trigger.continuous);
        if (ImGui::Checkbox("持续扳机（无需按住热键）", &continuous))
        {
            profile.setLocalBool("trigger_continuous", continuous);
            if (continuous)
            {
                for (std::size_t otherSlot = 0; otherSlot < Config::MAX_MOUSE_HOTKEYS; ++otherSlot)
                {
                    if (otherSlot != slot)
                        config.mouse_hotkeys[otherSlot].setLocalBool("trigger_continuous", false);
                }
            }
            OverlayConfig_MarkDirty();
        }
        bool enabledForHotkey = profile.localBool("trigger_enabled_for_hotkey", true);
        if (ImGui::Checkbox("正式启用此热键扳机", &enabledForHotkey)) { profile.setLocalBool("trigger_enabled_for_hotkey", enabledForHotkey); OverlayConfig_MarkDirty(); }
        bool stopOnLoss = profile.localBool("trigger_stop_fire_on_loss", trigger.stop_fire_on_loss);
        if (ImGui::Checkbox("目标丢失时停止开火", &stopOnLoss)) { profile.setLocalBool("trigger_stop_fire_on_loss", stopOnLoss); OverlayConfig_MarkDirty(); }
        localIntInput("目标丢失时，松开火键的延迟(ms)", "trigger_targeting_stop_fire_delay_ms", 0, 5000, trigger.stop_fire_delay_ms);
        localIntInput("目标出现时，按开火键的延迟(ms)", "trigger_targeting_key_delay_ms", 0, 5000, trigger.key_delay_ms);
        localIntInput("前摇延迟(ms)", "trigger_targeting_pre_fire_delay_ms", 0, 5000, trigger.pre_fire_delay_ms);
        localIntInput("目标出现时，按开火键的时长(ms)", "trigger_targeting_fire_duration_ms", 1, 10000, trigger.fire_duration_ms);
        localIntInput("按开火键时，随机时间附加值(ms)", "trigger_targeting_fire_duration_random_ms", 0, 10000, trigger.fire_duration_random_ms);
        localIntInput("连续按开火，按键的冷却时间(ms)", "trigger_targeting_cooldown_ms", 0, 10000, trigger.cooldown_ms);
        localIntInput("连续按开火，随机时间附加值(ms)", "trigger_targeting_cooldown_random_ms", 0, 10000, trigger.cooldown_random_ms);
    }
    ImGui::PopID();
}
