#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <set>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "config.h"
#include "modules/SimpleIni.h"

std::vector<std::string> Config::splitString(const std::string& str, char delimiter)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delimiter))
    {
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.erase(item.begin());
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.pop_back();

        tokens.push_back(item);
    }
    return tokens;
}

std::string Config::joinStrings(const std::vector<std::string>& vec, const std::string& delimiter)
{
    std::ostringstream oss;
    for (size_t i = 0; i < vec.size(); ++i)
    {
        if (i != 0) oss << delimiter;
        oss << vec[i];
    }
    return oss.str();
}

bool Config::MouseHotkey::localBool(const std::string& key, bool fallback) const
{
    const auto it = localConfig.find(key);
    if (it == localConfig.end()) return fallback;
    return it->second == "true" || it->second == "1" || it->second == "on";
}

int Config::MouseHotkey::localInt(const std::string& key, int fallback) const
{
    const auto it = localConfig.find(key);
    if (it == localConfig.end()) return fallback;
    try { return std::stoi(it->second); } catch (...) { return fallback; }
}

float Config::MouseHotkey::localFloat(const std::string& key, float fallback) const
{
    const auto it = localConfig.find(key);
    if (it == localConfig.end()) return fallback;
    try { return std::stof(it->second); } catch (...) { return fallback; }
}

std::string Config::MouseHotkey::localString(const std::string& key, const std::string& fallback) const
{
    const auto it = localConfig.find(key);
    return it == localConfig.end() ? fallback : it->second;
}

void Config::MouseHotkey::setLocalBool(const std::string& key, bool value)
{
    localConfig[key] = value ? "true" : "false";
}

void Config::MouseHotkey::setLocalInt(const std::string& key, int value)
{
    localConfig[key] = std::to_string(value);
}

void Config::MouseHotkey::setLocalFloat(const std::string& key, float value)
{
    std::ostringstream valueStream;
    valueStream << std::setprecision(9) << value;
    localConfig[key] = valueStream.str();
}

void Config::MouseHotkey::setLocalString(const std::string& key, const std::string& value)
{
    localConfig[key] = value;
}

const Config::MouseHotkey* Config::selectActiveMouseHotkey(
    const MouseHotkeyContainer& hotkeys,
    const std::unordered_map<std::string, bool>& pressedButtons)
{
    for (const auto& hotkey : hotkeys)
    {
        if (!hotkey.enabled || hotkey.id.empty() || hotkey.buttons.empty())
            continue;

        bool bindingPressed = false;
        for (const auto& button : hotkey.buttons)
        {
            const auto pressed = pressedButtons.find(button);
            if (pressed != pressedButtons.end() && pressed->second)
            {
                bindingPressed = true;
                break;
            }
        }

        if (bindingPressed)
            return &hotkey;
    }
    return nullptr;
}

void Config::normalizeMouseHotkeys()
{
    static const std::set<std::string> allowedButtons = {
        "LeftMouseButton", "RightMouseButton", "MiddleMouseButton",
        "X1MouseButton", "X2MouseButton"
    };
    std::set<std::string> usedButtons;
    std::set<std::string> usedIds;
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < mouse_hotkeys.size(); ++i)
    {
        auto& hotkey = mouse_hotkeys[i];
        hotkey.priority = std::max(0, std::min(9, hotkey.priority));
        hotkey.creationOrder = hotkey.creationOrder < 0 ? 0 : hotkey.creationOrder;
        if (hotkey.id.empty() || usedIds.count(hotkey.id) != 0)
        {
            hotkey.enabled = false;
            continue;
        }
        usedIds.insert(hotkey.id);
        std::vector<std::string> uniqueButtons;
        for (const auto& button : hotkey.buttons)
        {
            if (allowedButtons.count(button) != 0 && usedButtons.insert(button).second)
                uniqueButtons.push_back(button);
        }
        hotkey.buttons = std::move(uniqueButtons);
        if (hotkey.buttons.empty())
            hotkey.enabled = false;
        if (hotkey.enabled)
            order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return mouse_hotkeys[lhs].creationOrder < mouse_hotkeys[rhs].creationOrder;
    });
    for (std::size_t i = 0; i < order.size(); ++i)
        mouse_hotkeys[order[i]].creationOrder = static_cast<int>(i);
}

void Config::resetHotkeyClassSubsets()
{
    for (auto& hotkey : mouse_hotkeys)
    {
        for (int cls = 0; cls < FIXED_TARGET_CLASS_COUNT; ++cls)
        {
            const std::string key = "class_enabled_" + std::to_string(cls);
            if (!class_enabled[cls])
                hotkey.setLocalBool(key, false);
            else if (hotkey.localConfig.find(key) == hotkey.localConfig.end())
                hotkey.setLocalBool(key, true);
        }
    }
}

void Config::normalizeHotkeyClassAimOffsets()
{
    for (auto& hotkey : mouse_hotkeys)
    {
        for (int cls = 0; cls < FIXED_TARGET_CLASS_COUNT; ++cls)
        {
            const std::string prefix = "class_" + std::to_string(cls) + "_aim_offset_";
            hotkey.setLocalFloat(
                prefix + "x",
                std::clamp(hotkey.localFloat(prefix + "x", 0.5f), 0.0f, 1.0f));
            hotkey.setLocalFloat(
                prefix + "y",
                std::clamp(hotkey.localFloat(prefix + "y", 0.5f), 0.0f, 1.0f));

            const std::string triggerPrefix = "class_" + std::to_string(cls) + "_trigger_zone_";
            hotkey.setLocalFloat(
                triggerPrefix + "offset_x",
                std::clamp(hotkey.localFloat(triggerPrefix + "offset_x", 0.1f), 0.0f, 1.0f));
            hotkey.setLocalFloat(
                triggerPrefix + "offset_y",
                std::clamp(hotkey.localFloat(triggerPrefix + "offset_y", 0.1f), 0.0f, 1.0f));
            hotkey.setLocalFloat(
                triggerPrefix + "size_x",
                std::clamp(hotkey.localFloat(triggerPrefix + "size_x", 0.8f), 0.01f, 1.0f));
            hotkey.setLocalFloat(
                triggerPrefix + "size_y",
                std::clamp(hotkey.localFloat(triggerPrefix + "size_y", 0.8f), 0.01f, 1.0f));
        }
    }
}

bool Config::loadConfig(const std::string& filename)
{
    std::filesystem::path targetPath = filename.empty() ? std::filesystem::path("config.ini") : std::filesystem::path(filename);
    std::error_code absEc;
    targetPath = std::filesystem::absolute(targetPath, absEc);
    if (absEc)
    {
        std::cerr << "[配置] 无法解析配置文件路径:" << std::endl;
        std::cerr << filename << std::endl;
        std::cerr << "[配置] 原因: " << absEc.message() << std::endl;
        return false;
    }
    config_path = targetPath.lexically_normal().string();
    std::string target = config_path;

    std::cout << "\n========== 配置 ==========" << std::endl;
    std::cout << "配置文件:" << std::endl;
    // 路径经 path::string() 得到的是本地 ANSI 代码页(GBK)字符串，直接输出到 UTF-8 控制台会乱码。
    // 显示时统一转为 UTF-8：先按本地编码还原为 path，再取 u8string()。文件读写仍用原生窄字符串(config_path)，不受影响。
    std::cout << std::filesystem::path(target).u8string() << std::endl;

    if (!std::filesystem::exists(targetPath))
    {
        std::cout << "配置文件不存在，正在创建默认配置。" << std::endl;

        // Capture
        capture_method = "duplication_api"; // 捕获方式：桌面复制 API
        capture_target = "monitor"; // 捕获目标：显示器
        capture_window_title = ""; // 捕获窗口标题
        udp_ip = "0.0.0.0"; // UDP 接收 IP 地址，0.0.0.0 接受任意来源
        udp_port = 1234; // UDP 接收端口
        detection_resolution = 320; // 检测分辨率
        capture_fps = 60; // 采集帧率上限
        monitor_idx = 0; // 显示器索引
        circle_fov_enabled = true; // 启用圆形视野范围
        circle_fov_radius_percent = 100; // 圆形视野半径百分比
        circle_fov_show_preview = true; // 显示圆形视野预览
        capture_borders = false; // 捕获窗口边框
        capture_cursor = false; // 捕获鼠标光标
        virtual_camera_name = "None"; // 虚拟摄像头名称
        virtual_camera_width = 1920; // 虚拟摄像头宽度
        virtual_camera_heigth = 1080; // 虚拟摄像头高度
        virtual_camera_fps = 60; // 虚拟摄像头帧率

        // Target
        aim_offset_x = 0.5f; // 瞄准点水平偏移比例
        aim_offset_y = 0.5f; // 瞄准点垂直偏移比例
        auto_aim = false; // 启用自动瞄准
        tracker_enabled = false; // 启用目标追踪
        tracker_overlay_table_enabled = true; // 显示目标追踪列表
        targeting_mode = "closest_center"; // 选目标方式：最接近屏幕中心

        // Mouse
        // 修复：默认分支回退值须与读取分支（get_long("fovX",121)/
        // get_long("fovY",90)）及 UI ValidateIntParam 默认值保持一致。
        // 原 85/54 仅在首次生成 ini 时写入；若 ini 损坏缺键，读取分支会回退
        // 到 121/90，导致行为悄悄变化。
        fovX = 121; // 水平 FOV 范围
        fovY = 90; // 垂直 FOV 范围
        minSpeedMultiplier = 0.1f; // 最小瞄准速度倍率
        maxSpeedMultiplier = 0.1f; // 最大瞄准速度倍率

        predictionInterval = 0.01f; // 目标预测时间间隔（秒）
        prediction_futurePositions = 20; // 预测未来位置数量
        draw_futurePositions = false; // 绘制预测位置
        kalman_enabled = false; // 启用kalman滤波
        kalman_process_noise_position = 40.0f; // kalman位置过程噪声
        kalman_process_noise_velocity = 1800.0f; // kalman速度过程噪声
        kalman_measurement_noise = 35.0f; // kalman测量噪声
        kalman_velocity_damping = 0.08f; // kalman速度阻尼
        kalman_max_velocity = 20000.0f; // kalman最大速度
        kalman_warmup_frames = 2; // kalman预热帧数
        kalman_compensate_detection_delay = false; // 补偿检测延迟
        kalman_additional_prediction_ms = 0.0f; // 额外预测时间（毫秒）
        kalman_reset_timeout_sec = 0.5f; // kalman重置超时（秒）

        // MouseController 移植模块 (依据 鼠标调参指南.md)
        mc_enabled = true;             // 总开关: 启用 MouseController
        // 分轴 X (默认均衡型)
        mc_x_tracking = 3.0f;  mc_x_damping = 0.05f;  mc_x_maxspeed = 1500.0f;
        mc_x_integral = 0.0f;  mc_x_deadzone = 2.0f;
        // 分轴 Y (默认同 X)
        mc_y_tracking = 3.0f;  mc_y_damping = 0.05f;  mc_y_maxspeed = 1500.0f;
        mc_y_integral = 0.0f;  mc_y_deadzone = 2.0f;
        // 全局参数
        mc_maxstep = 30.0f;  mc_retarget = 50.0f;
        // 算法参数 (自适应范围)
        mc_ahead_min = 0.05f;  mc_ahead_max = 0.15f;
        mc_dur_min = 0.15f;    mc_dur_max = 0.60f;
        mc_kalman_q = 1000.0f; mc_kalman_r = 25.0f;

        snapRadius = 1.5f; // 吸附半径
        nearRadius = 25.0f; // 近距离半径
        speedCurveExponent = 3.0f; // 速度曲线指数
        snapBoostFactor = 1.15f; // 吸附加速倍率

        easynorecoil = false; // 启用简易压枪
        easynorecoilstrength = 0.0f; // 压枪强度
        input_method = "WIN32"; // 鼠标输入方式

        // Wind mouse
        wind_mouse_enabled = false; // 启用风鼠标轨迹
        wind_G = 18.0f; // 风鼠标重力参数
        wind_W = 15.0f; // 风鼠标风力参数
        wind_M = 10.0f; // 风鼠标最大步长
        wind_D = 8.0f; // 风鼠标距离阈值

        // kmbox_net
        kmbox_net_ip = "192.168.2.188"; // KmboxNet IP 地址
        kmbox_net_port = "8808"; // KmboxNet 端口
        kmbox_net_uuid = "0E0A3CAB"; // KmboxNet UUID

        // kmbox_a
        kmbox_a_pidvid = ""; // KmboxA PIDVID

        // makcu
        makcu_baudrate = 115200; // Makcu 串口波特率
        makcu_port = "COM0"; // Makcu 串口名称

        // Mouse shooting
        auto_shoot = false; // 启用自动开枪
        bScope_multiplier = 1.0f; // 瞄准镜速度倍率

        // Trigger (per-hotkey, defaults from TriggerConfig struct)

        // AI
        backend = "TRT"; // AI 推理后端：TensorRT

        ai_model = "Jinn.engine"; // AI 模型文件

        confidence_threshold = 0.50f; // 置信度阈值
        nms_threshold = 0.50f; // NMS 阈值
        max_detections = 8; // 每帧最大检测数量
#ifdef USE_CUDA
        export_enable_fp8 = false; // 导出 FP8 引擎
        export_enable_fp16 = true; // 导出 FP16 引擎
#endif
        fixed_input_size = true; // 使用固定模型输入尺寸

        // CUDA
#ifdef USE_CUDA
        use_cuda_graph = false; // 使用 CUDA Graph false
        use_pinned_memory = false; // 使用锁页内存
        cuda_device_index = 0; // CUDA 显卡索引
        gpuMemoryReserveMB = 2048; // 预留 GPU 内存（MB）
        enableGpuExclusiveMode = true; // 启用 GPU 独占模式
        capture_use_cuda = false; // 捕获阶段使用 CUDA  false
#endif

        // System
        cpuCoreReserveCount = 4; // 预留 CPU 核心数
        systemMemoryReserveMB = 2048; // 预留系统内存（MB）

        // Buttons
        button_targeting = splitString("RightMouseButton"); // 瞄准按键
        button_shoot = splitString("LeftMouseButton"); // 开枪按键
        button_zoom = splitString("RightMouseButton"); // 瞄准镜按键
        button_exit = splitString("F12"); // 退出程序按键
        button_pause = splitString("None"); // 暂停按键
        button_reload_config = splitString("None"); // 重载配置按键
        button_open_overlay = splitString("F10"); // 打开覆盖层按键
        enable_arrows_settings = false; // 启用方向键调参

        for (std::size_t i = 0; i < MAX_MOUSE_HOTKEYS; ++i)
        {
            mouse_hotkeys[i] = MouseHotkey{}; // 重置热键配置槽位
            mouse_hotkeys[i].creationOrder = static_cast<int>(i); // 热键创建顺序
        }
        mouse_hotkeys[0].id = "targeting"; // 默认热键标识：瞄准
        mouse_hotkeys[0].buttons = button_targeting; // 默认热键绑定按键
        mouse_hotkeys[0].priority = 0; // 默认热键优先级
        mouse_hotkeys[0].enabled = true; // 启用默认热键
        normalizeMouseHotkeys();

        // Overlay
        overlay_exclude_from_capture = false; // 从捕获画面排除覆盖层
        overlay_x = 0; // 覆盖层窗口 X 坐标
        overlay_y = 0; // 覆盖层窗口 Y 坐标
    overlay_width = 760; // 覆盖层窗口宽度
    overlay_height = 480; // 覆盖层窗口高度

        // Game overlay
        game_overlay_enabled = false; // 启用游戏内覆盖层
        game_overlay_max_fps = 0; // 游戏内覆盖层最大帧率，0 为不限
        game_overlay_draw_boxes = true; // 绘制目标框
        game_overlay_compensate_latency = true; // 补偿显示延迟
        game_overlay_draw_future = true; // 绘制预测点
        game_overlay_draw_wind_tail = true; // 绘制风鼠标尾迹
        game_overlay_draw_frame = true; // 绘制覆盖层边框
        game_overlay_draw_circle_fov = true; // 绘制圆形 FOV
        game_overlay_show_target_correction = true; // 显示目标校正信息
        game_overlay_box_a = 255; // 目标框颜色透明度
        game_overlay_box_r = 0; // 目标框颜色红色分量
        game_overlay_box_g = 255; // 目标框颜色绿色分量
        game_overlay_box_b = 0; // 目标框颜色蓝色分量
        game_overlay_frame_a = 180; // 边框颜色透明度
        game_overlay_frame_r = 255; // 边框颜色红色分量
        game_overlay_frame_g = 255; // 边框颜色绿色分量
        game_overlay_frame_b = 255; // 边框颜色蓝色分量
        game_overlay_box_thickness = 2.0f; // 目标框线条粗细
        game_overlay_frame_thickness = 1.5f; // 边框线条粗细
        game_overlay_future_point_radius = 5.0f; // 预测点半径
        game_overlay_future_alpha_falloff = 1.0f; // 预测点透明度衰减

        game_overlay_icon_enabled = false; // 启用游戏内图标
        game_overlay_icon_path = "icon.png"; // 图标文件路径
        game_overlay_icon_width = 64; // 图标宽度
        game_overlay_icon_height = 64; // 图标高度
        game_overlay_icon_offset_x = 0.0f; // 图标水平偏移
        game_overlay_icon_offset_y = 0.0f; // 图标垂直偏移
        game_overlay_icon_anchor = "center"; // 图标锚点位置
        game_overlay_icon_class = -1; // 图标适用类别，-1 为全部

        // Data collection
        collect_data_while_playing = false; // 游戏时采集数据
        collect_only_when_aimbot_running = false; // 仅在自瞄运行时采集
        collect_only_when_targets_present = true; // 仅在有目标时采集
        collect_save_every_n_frames = 300; // 每隔多少帧保存一次
        collect_jpeg_quality = 100; // JPEG 保存质量
        collect_output_dir = ""; // 数据采集输出目录
        auto_label_data = true; // 自动标注采集数据
        auto_label_min_conf = 0.20f; // 自动标注最低置信度
        auto_label_max_boxes = 20; // 自动标注最大框数
        auto_label_record_classes.clear();

        // Dynamic Range
        dynamic_range_enabled = false; // 启用动态范围
        dynamic_range_shrink_scope = 320; // 动态范围缩小尺寸
        dynamic_range_shrink_duration_ms = 300; // 动态范围持续时间（毫秒）
        dynamic_range_cooldown_ms = 300; // 动态范围冷却时间（毫秒）
        dynamic_range_target_classes = ""; // 动态范围目标类别

        // Classes (global target class system)
        for (int i = 0; i < MAX_CLASSES; ++i)
        {
            class_enabled[i] = i == 0; // 默认仅启用第一个类别
        }
        class_player = 0; // 玩家类别索引
        class_head = 2; // 头部类别索引
        resetHotkeyClassSubsets();
        normalizeHotkeyClassAimOffsets();

        // Debug
        show_window = false; // 显示预览窗口
        show_fps = false; // 显示帧率
        screenshot_button = splitString("None"); // 截图按键
        screenshot_delay = 500; // 截图延迟（毫秒）
        verbose = false; // 输出详细日志

        // Game profiles
        game_profiles.clear();
        GameProfile uni;
        uni.name = "默认内置"; // 统一游戏配置名称
        uni.sens = 1.0; // 游戏鼠标灵敏度
        uni.yaw = 0.022; // 水平旋转比例
        uni.pitch = uni.yaw; // 垂直旋转比例
        uni.fovScaled = false; // 按视野缩放修正
        uni.baseFOV = 0.0; // 游戏基础视野角度
        game_profiles[uni.name] = uni;
        active_game = uni.name; // 当前激活的游戏配置

        saveConfig(target);
        return true;
    }

    CSimpleIniA ini;
    ini.SetUnicode();
    // 中文路径修复：CSimpleIniA::LoadFile(const char*) 内部用窄字符 fopen（ANSI/GBK），
    // 当 config.ini 位于含中文的目录时可能因代码页不匹配而打不开。SimpleIni 在 _WIN32
    // 下提供了 SI_HAS_WIDE_FILE 宽路径重载，这里用 UTF-16 路径打开，任何 Unicode 路径都能解析。
    SI_Error rc = ini.LoadFile(std::filesystem::path(target).wstring().c_str());
    if (rc < 0)
    {
        std::cerr << "[配置] 解析配置文件失败:" << std::endl;
        std::cerr << target << std::endl;
        return false;
    }

    auto get_string = [&](const char* key, const std::string& defval)
    {
        const char* val = ini.GetValue("", key, defval.c_str());
        return val ? std::string(val) : defval;
    };

    auto get_bool = [&](const char* key, bool defval)
        {
            return ini.GetBoolValue("", key, defval);
        };

    auto get_long = [&](const char* key, long defval, long minval = static_cast<long>(std::numeric_limits<int>::min()), long maxval = static_cast<long>(std::numeric_limits<int>::max()))
        {
            const long value = ini.GetLongValue("", key, defval);
            if (value < minval || value > maxval)
            {
                std::cerr << "[Config] 数值超出范围，使用默认值: " << key << std::endl;
                const long clamped = defval < minval ? minval : (defval > maxval ? maxval : defval);
                return static_cast<int>(clamped);
            }
            return static_cast<int>(value);
        };

    auto get_double = [&](const char* key, double defval)
        {
            return ini.GetDoubleValue("", key, defval);
        };

    game_profiles.clear();

    CSimpleIniA::TNamesDepend keys;
    ini.GetAllKeys("Games", keys);

    for (const auto& k : keys)
    {
        std::string name = k.pItem;
        std::string val = ini.GetValue("Games", k.pItem, "");
        auto parts = splitString(val, ',');

        try
        {
            if (parts.size() < 2)
                throw std::runtime_error("not enough values");

            GameProfile gp;
            gp.name = name;
            gp.sens = std::stod(parts[0]);
            gp.yaw = std::stod(parts[1]);
            gp.pitch = parts.size() > 2 ? std::stod(parts[2]) : gp.yaw;
            gp.fovScaled = parts.size() > 3 && (parts[3] == "true" || parts[3] == "1");
            gp.baseFOV = parts.size() > 4 ? std::stod(parts[4]) : 0.0;

            game_profiles[name] = gp;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Config] Failed to parse profile: " << name
                << " = " << val << " (" << e.what() << ")" << std::endl;
        }
    }

    if (!game_profiles.count("UNIFIED"))
    {
        GameProfile uni;
        uni.name = "UNIFIED";
        uni.sens = 1.0;
        uni.yaw = 0.022;
        uni.pitch = uni.yaw;
        uni.fovScaled = false;
        uni.baseFOV = 0.0;
        game_profiles[uni.name] = uni;
    }

    active_game = get_string("active_game", active_game);
    if (!game_profiles.count(active_game) && !game_profiles.empty())
        active_game = game_profiles.begin()->first;

    // Capture
    capture_method = get_string("capture_method", "duplication_api");
    capture_target = get_string("capture_target", "monitor");
    capture_window_title = get_string("capture_window_title", "");
    udp_ip = get_string("udp_ip", "0.0.0.0");
    udp_port = get_long("udp_port", 1234);
    if (udp_port < 1 || udp_port > 65535)
        udp_port = 1234;
    detection_resolution = get_long("detection_resolution", 320);
    detection_resolution = std::clamp(detection_resolution, 160, 640);
    detection_resolution = (detection_resolution / 32) * 32;

    capture_fps = get_long("capture_fps", 120);
    // 采集帧率范围钳制：0 表示不限制（capture.cpp 显式支持 unlimited 语义），
    // 负值按 0 处理避免帧限速器出现负时长；上限对齐 UI ValidateIntParam(&config.capture_fps, 1, 240, 60)。
    if (capture_fps < 0) capture_fps = 0;
    if (capture_fps > 240) capture_fps = 240;
    monitor_idx = get_long("monitor_idx", 0);
    // 显示器索引钳制：消费端有 max(0) 下界防护，但上界越界会令 DXGI 枚举失败导致采集不可用；
    // 钳制到 [0, 63]（最多 64 屏）对齐键盘热键循环切换场景，防止误配大索引静默失效。
    monitor_idx = std::clamp(monitor_idx, 0, 63);
    circle_fov_enabled = get_bool("circle_fov_enabled", true);
    circle_fov_radius_percent = get_long("circle_fov_radius_percent", 100);
    if (circle_fov_radius_percent < 1) circle_fov_radius_percent = 1;
    if (circle_fov_radius_percent > 100) circle_fov_radius_percent = 100;
    circle_fov_show_preview = get_bool("circle_fov_show_preview", true);
    capture_borders = get_bool("capture_borders", true);
    capture_cursor = get_bool("capture_cursor", true);
    virtual_camera_name = get_string("virtual_camera_name", "None");
    virtual_camera_width = get_long("virtual_camera_width", 1920);
    virtual_camera_heigth = get_long("virtual_camera_heigth", 1080);
    virtual_camera_fps = get_long("virtual_camera_fps", 60);

    // Target
    aim_offset_x = (float)get_double("aim_offset_x", 0.5);
    aim_offset_y = (float)get_double("aim_offset_y", 0.5);
    aim_offset_x = std::clamp(aim_offset_x, 0.0f, 1.0f);
    aim_offset_y = std::clamp(aim_offset_y, 0.0f, 1.0f);
    auto_aim = get_bool("auto_aim", false);
    tracker_enabled = get_bool("tracker_enabled", true);
    tracker_overlay_table_enabled = get_bool("tracker_overlay_table_enabled", true);
    targeting_mode = get_string("targeting_mode", "closest_center");

    // Mouse
    fovX = get_long("fovX", 121);
    fovY = get_long("fovY", 90);
    // FOV 必须为正：calc_movement 中 degPerPx = fov / screen_size，
    // fov<=0 会导致瞄准位移恒为 0（失效）或负值（反向移动）。
    // 取值范围与 UI 的 ValidateIntParam(&config.fovX, 1, 360, 121) 保持一致。
    fovX = std::clamp(fovX, 1, 360);
    fovY = std::clamp(fovY, 1, 360);
    minSpeedMultiplier = (float)get_double("minSpeedMultiplier", 0.1);
    maxSpeedMultiplier = (float)get_double("maxSpeedMultiplier", 0.1);

    predictionInterval = (float)get_double("predictionInterval", 0.01);
    prediction_futurePositions = get_long("prediction_futurePositions", 20);
    draw_futurePositions = get_bool("draw_futurePositions", true);
    kalman_enabled = get_bool("kalman_enabled", true);
    kalman_process_noise_position = (float)get_double("kalman_process_noise_position", 40.0);
    kalman_process_noise_velocity = (float)get_double("kalman_process_noise_velocity", 1800.0);
    kalman_measurement_noise = (float)get_double("kalman_measurement_noise", 35.0);
    kalman_velocity_damping = (float)get_double("kalman_velocity_damping", 0.08);
    kalman_max_velocity = (float)get_double("kalman_max_velocity", 20000.0);
    kalman_warmup_frames = get_long("kalman_warmup_frames", 2);
    kalman_compensate_detection_delay = get_bool("kalman_compensate_detection_delay", true);
    kalman_additional_prediction_ms = (float)get_double("kalman_additional_prediction_ms", 0.0);
    kalman_reset_timeout_sec = (float)get_double("kalman_reset_timeout_sec", 0.5);

    // MouseController 移植模块参数 (依据 鼠标调参指南.md)
    mc_enabled = get_bool("mc_enabled", true);
    mc_x_tracking = (float)get_double("mc_x_tracking", 3.0);
    mc_x_damping  = (float)get_double("mc_x_damping", 0.05);
    mc_x_maxspeed = (float)get_double("mc_x_maxspeed", 1500.0);
    mc_x_integral = (float)get_double("mc_x_integral", 0.0);
    mc_x_deadzone = (float)get_double("mc_x_deadzone", 2.0);
    mc_y_tracking = (float)get_double("mc_y_tracking", 3.0);
    mc_y_damping  = (float)get_double("mc_y_damping", 0.05);
    mc_y_maxspeed = (float)get_double("mc_y_maxspeed", 1500.0);
    mc_y_integral = (float)get_double("mc_y_integral", 0.0);
    mc_y_deadzone = (float)get_double("mc_y_deadzone", 2.0);
    mc_maxstep  = (float)get_double("mc_maxstep", 30.0);
    mc_retarget = (float)get_double("mc_retarget", 50.0);
    mc_ahead_min = (float)get_double("mc_ahead_min", 0.05);
    mc_ahead_max = (float)get_double("mc_ahead_max", 0.15);
    mc_dur_min   = (float)get_double("mc_dur_min", 0.15);
    mc_dur_max   = (float)get_double("mc_dur_max", 0.60);
    mc_kalman_q  = (float)get_double("mc_kalman_q", 1000.0);
    mc_kalman_r  = (float)get_double("mc_kalman_r", 25.0);

    snapRadius = (float)get_double("snapRadius", 1.5);
    nearRadius = (float)get_double("nearRadius", 25.0);
    speedCurveExponent = (float)get_double("speedCurveExponent", 3.0);
    snapBoostFactor = (float)get_double("snapBoostFactor", 1.15);

    easynorecoil = get_bool("easynorecoil", false);
    easynorecoilstrength = (float)get_double("easynorecoilstrength", 0.0);
    input_method = get_string("input_method", "WIN32");

    // Wind mouse
    wind_mouse_enabled = get_bool("wind_mouse_enabled", false);
    wind_G = (float)get_double("wind_G", 18.0f);
    wind_W = (float)get_double("wind_W", 15.0f);
    wind_M = (float)get_double("wind_M", 10.0f);
    wind_D = (float)get_double("wind_D", 8.0f);

    // kmbox_net
    kmbox_net_ip = get_string("kmbox_net_ip", "192.168.2.188");
    kmbox_net_port = get_string("kmbox_net_port", "8808");
    kmbox_net_uuid = get_string("kmbox_net_uuid", "0E0A3CAB");

    // kmbox_a
    kmbox_a_pidvid = get_string("kmbox_a_pidvid", "");

    // makcu
    makcu_baudrate = get_long("makcu_baudrate", 115200);
    makcu_port = get_string("makcu_port", "COM0");

    // Mouse shooting
    auto_shoot = get_bool("auto_shoot", false);
    bScope_multiplier = (float)get_double("bScope_multiplier", 1.2);

    // Trigger (per-hotkey)
    trigger_targeting.enabled = get_bool("trigger_targeting_enabled", false);
    trigger_targeting.continuous = get_bool("trigger_targeting_continuous", false);
    trigger_targeting.stop_fire_on_loss = get_bool("trigger_targeting_stop_fire_on_loss", true);
    trigger_targeting.stop_fire_delay_ms = get_long("trigger_targeting_stop_fire_delay_ms", 200);
    trigger_targeting.key_delay_ms = get_long("trigger_targeting_key_delay_ms", 50);
    trigger_targeting.pre_fire_delay_ms = get_long("trigger_targeting_pre_fire_delay_ms", 100);
    trigger_targeting.fire_duration_ms = get_long("trigger_targeting_fire_duration_ms", 500);
    trigger_targeting.fire_duration_random_ms = get_long("trigger_targeting_fire_duration_random_ms", 100);
    trigger_targeting.cooldown_ms = get_long("trigger_targeting_cooldown_ms", 300);
    trigger_targeting.cooldown_random_ms = get_long("trigger_targeting_cooldown_random_ms", 100);
    trigger_targeting.zone_offset_x = (float)get_double("trigger_targeting_zone_offset_x", 0.1);
    trigger_targeting.zone_offset_y = (float)get_double("trigger_targeting_zone_offset_y", 0.1);
    trigger_targeting.zone_size_x = (float)get_double("trigger_targeting_zone_size_x", 0.8);
    trigger_targeting.zone_size_y = (float)get_double("trigger_targeting_zone_size_y", 0.8);

    trigger_shoot.enabled = get_bool("trigger_shoot_enabled", false);
    trigger_shoot.continuous = get_bool("trigger_shoot_continuous", false);
    trigger_shoot.stop_fire_on_loss = get_bool("trigger_shoot_stop_fire_on_loss", true);
    trigger_shoot.stop_fire_delay_ms = trigger_targeting.stop_fire_delay_ms;
    trigger_shoot.key_delay_ms = trigger_targeting.key_delay_ms;
    trigger_shoot.pre_fire_delay_ms = trigger_targeting.pre_fire_delay_ms;
    trigger_shoot.fire_duration_ms = trigger_targeting.fire_duration_ms;
    trigger_shoot.fire_duration_random_ms = trigger_targeting.fire_duration_random_ms;
    trigger_shoot.cooldown_ms = trigger_targeting.cooldown_ms;
    trigger_shoot.cooldown_random_ms = trigger_targeting.cooldown_random_ms;
    trigger_shoot.zone_offset_x = (float)get_double("trigger_shoot_zone_offset_x", 0.1);
    trigger_shoot.zone_offset_y = (float)get_double("trigger_shoot_zone_offset_y", 0.1);
    trigger_shoot.zone_size_x = (float)get_double("trigger_shoot_zone_size_x", 0.8);
    trigger_shoot.zone_size_y = (float)get_double("trigger_shoot_zone_size_y", 0.8);

    trigger_zoom.enabled = get_bool("trigger_zoom_enabled", false);
    trigger_zoom.continuous = get_bool("trigger_zoom_continuous", false);
    trigger_zoom.stop_fire_on_loss = get_bool("trigger_zoom_stop_fire_on_loss", true);
    trigger_zoom.stop_fire_delay_ms = trigger_targeting.stop_fire_delay_ms;
    trigger_zoom.key_delay_ms = trigger_targeting.key_delay_ms;
    trigger_zoom.pre_fire_delay_ms = trigger_targeting.pre_fire_delay_ms;
    trigger_zoom.fire_duration_ms = trigger_targeting.fire_duration_ms;
    trigger_zoom.fire_duration_random_ms = trigger_targeting.fire_duration_random_ms;
    trigger_zoom.cooldown_ms = trigger_targeting.cooldown_ms;
    trigger_zoom.cooldown_random_ms = trigger_targeting.cooldown_random_ms;
    trigger_zoom.zone_offset_x = (float)get_double("trigger_zoom_zone_offset_x", 0.1);
    trigger_zoom.zone_offset_y = (float)get_double("trigger_zoom_zone_offset_y", 0.1);
    trigger_zoom.zone_size_x = (float)get_double("trigger_zoom_zone_size_x", 0.8);
    trigger_zoom.zone_size_y = (float)get_double("trigger_zoom_zone_size_y", 0.8);

    // Trigger timing is global. The targeting slot is the canonical storage;
    // all trigger slots receive the same seven timing values after loading.
    trigger_shoot.stop_fire_delay_ms = trigger_targeting.stop_fire_delay_ms;
    trigger_shoot.key_delay_ms = trigger_targeting.key_delay_ms;
    trigger_shoot.pre_fire_delay_ms = trigger_targeting.pre_fire_delay_ms;
    trigger_shoot.fire_duration_ms = trigger_targeting.fire_duration_ms;
    trigger_shoot.fire_duration_random_ms = trigger_targeting.fire_duration_random_ms;
    trigger_shoot.cooldown_ms = trigger_targeting.cooldown_ms;
    trigger_shoot.cooldown_random_ms = trigger_targeting.cooldown_random_ms;
    trigger_zoom.stop_fire_delay_ms = trigger_targeting.stop_fire_delay_ms;
    trigger_zoom.key_delay_ms = trigger_targeting.key_delay_ms;
    trigger_zoom.pre_fire_delay_ms = trigger_targeting.pre_fire_delay_ms;
    trigger_zoom.fire_duration_ms = trigger_targeting.fire_duration_ms;
    trigger_zoom.fire_duration_random_ms = trigger_targeting.fire_duration_random_ms;
    trigger_zoom.cooldown_ms = trigger_targeting.cooldown_ms;
    trigger_zoom.cooldown_random_ms = trigger_targeting.cooldown_random_ms;

    // AI
    backend = "TRT";

    ai_model = get_string("ai_model", "sunxds_0.8.0.engine");
    confidence_threshold = (float)get_double("confidence_threshold", 0.5);
    nms_threshold = (float)get_double("nms_threshold", 0.4);
    max_detections = get_long("max_detections", 8);
#ifdef USE_CUDA
    export_enable_fp8 = get_bool("export_enable_fp8", true);
    export_enable_fp16 = get_bool("export_enable_fp16", true);
#endif

    // CUDA
#ifdef USE_CUDA
    use_cuda_graph = get_bool("use_cuda_graph", false);
    use_pinned_memory = get_bool("use_pinned_memory", true);
    cuda_device_index = get_long("cuda_device_index", 0);
    gpuMemoryReserveMB = get_long("gpuMemoryReserveMB", 2048);
    enableGpuExclusiveMode = get_bool("enableGpuExclusiveMode", true);
    capture_use_cuda = get_bool("capture_use_cuda", false);
#endif

    // System
    cpuCoreReserveCount = get_long("cpuCoreReserveCount", 4);
    systemMemoryReserveMB = get_long("systemMemoryReserveMB", 2048);

    // Buttons
    button_targeting = splitString(get_string("button_targeting", "RightMouseButton"));
    button_shoot = splitString(get_string("button_shoot", "LeftMouseButton"));
    button_zoom = splitString(get_string("button_zoom", "RightMouseButton"));
    button_exit = splitString(get_string("button_exit", "F12"));
    button_pause = splitString(get_string("button_pause", "None"));
    button_reload_config = splitString(get_string("button_reload_config", "None"));
    button_open_overlay = splitString(get_string("button_open_overlay", "F10"));
    enable_arrows_settings = get_bool("enable_arrows_settings", false);

    // Classes are loaded first so hotkey subsets can be normalized against the global limit.
    for (int i = 0; i < MAX_CLASSES; ++i)
    {
        const std::string enabledKey = "class_" + std::to_string(i) + "_enabled";
        class_enabled[i] = i < FIXED_TARGET_CLASS_COUNT && get_bool(enabledKey.c_str(), false);
    }
    class_player = get_long("class_player", 0);
    class_head = get_long("class_head", 1);

    // New hotkey storage is optional. Missing Slot 1 is migrated from the legacy targeting binding.
    for (std::size_t i = 0; i < MAX_MOUSE_HOTKEYS; ++i)
    {
        auto& hotkey = mouse_hotkeys[i];
        const std::string prefix = "mouse_hotkey_" + std::to_string(i) + "_";
        hotkey.id = get_string((prefix + "id").c_str(), "");
        hotkey.buttons = splitString(get_string((prefix + "buttons").c_str(), ""));
        hotkey.priority = get_long((prefix + "priority").c_str(), 0);
        hotkey.creationOrder = get_long((prefix + "creation_order").c_str(), static_cast<long>(i));
        hotkey.enabled = get_bool((prefix + "enabled").c_str(), false);
        const std::string localPrefix = prefix + "local_";
        hotkey.localConfig.clear();
        const std::string localKeys = get_string((prefix + "local_keys").c_str(), "");
        for (const auto& key : splitString(localKeys, '|'))
        {
            if (key.empty()) continue;
            const auto value = get_string((localPrefix + key).c_str(), "");
            hotkey.localConfig[key] = value;
        }
    }
    if (mouse_hotkeys[0].id.empty())
    {
        mouse_hotkeys[0].id = "targeting";
        mouse_hotkeys[0].buttons = button_targeting;
        mouse_hotkeys[0].enabled = true;
    }
    normalizeMouseHotkeys();
    for (auto& hotkey : mouse_hotkeys)
    {
        for (int cls = FIXED_TARGET_CLASS_COUNT; cls < MAX_CLASSES; ++cls)
        {
            const std::string classKey = std::to_string(cls);
            hotkey.localConfig.erase("class_enabled_" + classKey);
            hotkey.localConfig.erase("class_order_" + classKey);
            hotkey.localConfig.erase("class_" + classKey + "_aim_offset_x");
            hotkey.localConfig.erase("class_" + classKey + "_aim_offset_y");
            hotkey.localConfig.erase("class_" + classKey + "_trigger_zone_offset_x");
            hotkey.localConfig.erase("class_" + classKey + "_trigger_zone_offset_y");
            hotkey.localConfig.erase("class_" + classKey + "_trigger_zone_size_x");
            hotkey.localConfig.erase("class_" + classKey + "_trigger_zone_size_y");
        }
        for (int cls = 0; cls < FIXED_TARGET_CLASS_COUNT; ++cls)
        {
            const std::string key = "class_enabled_" + std::to_string(cls);
            if (!class_enabled[cls])
                hotkey.setLocalBool(key, false);
            else if (hotkey.localConfig.find(key) == hotkey.localConfig.end())
                hotkey.setLocalBool(key, true);
        }
    }
    normalizeHotkeyClassAimOffsets();

    int continuousHotkeyCount = 0;
    for (const auto& hotkey : mouse_hotkeys)
    {
        if (hotkey.localBool("trigger_continuous", false))
            ++continuousHotkeyCount;
    }
    if (continuousHotkeyCount > 1)
    {
        for (auto& hotkey : mouse_hotkeys)
            hotkey.setLocalBool("trigger_continuous", false);
    }

    // Overlay
    overlay_exclude_from_capture = get_bool("overlay_exclude_from_capture", false);
    overlay_x = get_long("overlay_x", 0);
    overlay_y = get_long("overlay_y", 0);
    overlay_width = get_long("overlay_width", 760);
    overlay_height = get_long("overlay_height", 480);

    game_overlay_enabled = get_bool("game_overlay_enabled", false);
    game_overlay_max_fps = get_long("game_overlay_max_fps", 0);
    game_overlay_draw_boxes = get_bool("game_overlay_draw_boxes", true);
    game_overlay_compensate_latency = get_bool("game_overlay_compensate_latency", true);
    game_overlay_draw_future = get_bool("game_overlay_draw_future", true);
    game_overlay_draw_wind_tail = get_bool("game_overlay_draw_wind_tail", true);
    game_overlay_draw_frame = get_bool("game_overlay_draw_frame", true);
    game_overlay_draw_circle_fov = get_bool("game_overlay_draw_circle_fov", true);
    game_overlay_show_target_correction = get_bool("game_overlay_show_target_correction", true);
    game_overlay_box_a = get_long("game_overlay_box_a", 255);
    game_overlay_box_r = get_long("game_overlay_box_r", 0);
    game_overlay_box_g = get_long("game_overlay_box_g", 255);
    game_overlay_box_b = get_long("game_overlay_box_b", 0);
    game_overlay_frame_a = get_long("game_overlay_frame_a", 180);
    game_overlay_frame_r = get_long("game_overlay_frame_r", 255);
    game_overlay_frame_g = get_long("game_overlay_frame_g", 255);
    game_overlay_frame_b = get_long("game_overlay_frame_b", 255);
    game_overlay_box_thickness = (float)get_double("game_overlay_box_thickness", 2.0);
    game_overlay_frame_thickness = (float)get_double("game_overlay_frame_thickness", 1.5);
    game_overlay_future_point_radius = (float)get_double("game_overlay_future_point_radius", 5.0);
    game_overlay_future_alpha_falloff = (float)get_double("game_overlay_future_alpha_falloff", 1.0);
    clampGameOverlayColor();

    game_overlay_icon_enabled = get_bool("game_overlay_icon_enabled", false);
    game_overlay_icon_path = get_string("game_overlay_icon_path", "icon.png");
    game_overlay_icon_width = get_long("game_overlay_icon_width", 64);
    game_overlay_icon_height = get_long("game_overlay_icon_height", 64);
    game_overlay_icon_offset_x = (float)get_double("game_overlay_icon_offset_x", 0.0f);
    game_overlay_icon_offset_y = (float)get_double("game_overlay_icon_offset_y", 0.0f);
    game_overlay_icon_anchor = get_string("game_overlay_icon_anchor", "center");
    game_overlay_icon_class = get_long("game_overlay_icon_class", -1);

    collect_data_while_playing = get_bool("collect_data_while_playing", false);
    collect_only_when_aimbot_running = get_bool("collect_only_when_aimbot_running", false);
    collect_only_when_targets_present = get_bool("collect_only_when_targets_present", true);
    collect_save_every_n_frames = get_long("collect_save_every_n_frames", 300);
    collect_output_dir = get_string("collect_output_dir", "");
    if (collect_output_dir == "models")
        collect_output_dir.clear();
    collect_jpeg_quality = get_long("collect_jpeg_quality", 100);
    auto_label_data = get_bool("auto_label_data", true);
    auto_label_min_conf = (float)get_double("auto_label_min_conf", 0.20);
    auto_label_max_boxes = get_long("auto_label_max_boxes", 8);
    auto_label_record_classes = get_string("auto_label_record_classes", "");

    // Dynamic Range
    dynamic_range_enabled = get_bool("dynamic_range_enabled", false);
    dynamic_range_shrink_scope = get_long("dynamic_range_shrink_scope", 320);
    dynamic_range_shrink_duration_ms = get_long("dynamic_range_shrink_duration_ms", 300);
    dynamic_range_cooldown_ms = get_long("dynamic_range_cooldown_ms", 300);
    dynamic_range_target_classes = get_string("dynamic_range_target_classes", "");

    if (kalman_process_noise_position < 0.0001f) kalman_process_noise_position = 0.0001f;
    if (kalman_process_noise_position > 5000.0f) kalman_process_noise_position = 5000.0f;
    if (kalman_process_noise_velocity < 0.0001f) kalman_process_noise_velocity = 0.0001f;
    if (kalman_process_noise_velocity > 50000.0f) kalman_process_noise_velocity = 50000.0f;
    if (kalman_measurement_noise < 0.0001f) kalman_measurement_noise = 0.0001f;
    if (kalman_measurement_noise > 5000.0f) kalman_measurement_noise = 5000.0f;
    if (kalman_velocity_damping < 0.0f) kalman_velocity_damping = 0.0f;
    if (kalman_velocity_damping > 3.0f) kalman_velocity_damping = 3.0f;
    if (kalman_max_velocity < 100.0f) kalman_max_velocity = 100.0f;
    if (kalman_max_velocity > 60000.0f) kalman_max_velocity = 60000.0f;
    if (kalman_warmup_frames < 0) kalman_warmup_frames = 0;
    if (kalman_warmup_frames > 20) kalman_warmup_frames = 20;
    if (kalman_additional_prediction_ms < -80.0f) kalman_additional_prediction_ms = -80.0f;
    if (kalman_additional_prediction_ms > 120.0f) kalman_additional_prediction_ms = 120.0f;
    if (kalman_reset_timeout_sec < 0.05f) kalman_reset_timeout_sec = 0.05f;
    if (kalman_reset_timeout_sec > 3.0f) kalman_reset_timeout_sec = 3.0f;

    // MouseController 移植模块参数范围钳制（与 draw_mouse.cpp 面板 mcFloat 范围一致）。
    // 若不钳制，用户手动编辑 config.ini 写入越界值（如 mc_maxstep=0 或 mc_kalman_q=0）
    // 会经 syncMouseController() 直达算法：maxStep=0 使每帧位移恒 0（瞄准静默失效）、
    // tracking=0 使 PID 无比例项（响应极慢）、q=0 使卡尔曼协方差迅速收敛不再跟随观测。
    // 钳制后行为与 UI 步进控件一致，杜绝"无声失效"类问题。
    mc_x_tracking  = std::clamp(mc_x_tracking, 2.0f, 6.0f);
    mc_y_tracking  = std::clamp(mc_y_tracking, 2.0f, 6.0f);
    mc_x_damping   = std::clamp(mc_x_damping, 0.02f, 0.12f);
    mc_y_damping   = std::clamp(mc_y_damping, 0.02f, 0.12f);
    mc_x_maxspeed  = std::clamp(mc_x_maxspeed, 800.0f, 3000.0f);
    mc_y_maxspeed  = std::clamp(mc_y_maxspeed, 800.0f, 3000.0f);
    mc_x_integral  = std::clamp(mc_x_integral, 0.0f, 2.0f);
    mc_y_integral  = std::clamp(mc_y_integral, 0.0f, 2.0f);
    mc_x_deadzone  = std::clamp(mc_x_deadzone, 1.0f, 5.0f);
    mc_y_deadzone  = std::clamp(mc_y_deadzone, 1.0f, 5.0f);
    mc_maxstep     = std::clamp(mc_maxstep, 15.0f, 50.0f);
    mc_retarget    = std::clamp(mc_retarget, 10.0f, 200.0f);
    mc_ahead_min   = std::clamp(mc_ahead_min, 0.02f, 0.15f);
    mc_ahead_max   = std::clamp(mc_ahead_max, 0.05f, 0.30f);
    mc_dur_min     = std::clamp(mc_dur_min, 0.10f, 0.60f);
    mc_dur_max     = std::clamp(mc_dur_max, 0.15f, 1.00f);
    mc_kalman_q    = std::clamp(mc_kalman_q, 100.0f, 5000.0f);
    mc_kalman_r    = std::clamp(mc_kalman_r, 5.0f, 200.0f);
    // 自适应范围不得颠倒：ahead/duration 的 min > max 会让自适应策略输出异常前瞻/时长。
    if (mc_ahead_min > mc_ahead_max) std::swap(mc_ahead_min, mc_ahead_max);
    if (mc_dur_min > mc_dur_max) std::swap(mc_dur_min, mc_dur_max);

    if (overlay_width < 560) overlay_width = 560;
    if (overlay_width > 3840) overlay_width = 3840;
    if (overlay_height < 340) overlay_height = 340;
    if (overlay_height > 2160) overlay_height = 2160;

    if (collect_save_every_n_frames < 1) collect_save_every_n_frames = 1;
    if (collect_save_every_n_frames > 600) collect_save_every_n_frames = 600;
    if (collect_jpeg_quality < 50) collect_jpeg_quality = 50;
    if (collect_jpeg_quality > 100) collect_jpeg_quality = 100;
    if (auto_label_min_conf < 0.01f) auto_label_min_conf = 0.01f;
    if (auto_label_min_conf > 0.99f) auto_label_min_conf = 0.99f;
    if (auto_label_max_boxes < 1) auto_label_max_boxes = 1;
    if (auto_label_max_boxes > 200) auto_label_max_boxes = 200;

    if (dynamic_range_shrink_scope < 1) dynamic_range_shrink_scope = 1;
    if (dynamic_range_shrink_scope > 640) dynamic_range_shrink_scope = 640;
    if (dynamic_range_shrink_duration_ms < 50) dynamic_range_shrink_duration_ms = 50;
    if (dynamic_range_shrink_duration_ms > 2000) dynamic_range_shrink_duration_ms = 2000;
    if (dynamic_range_cooldown_ms < 50) dynamic_range_cooldown_ms = 50;
    if (dynamic_range_cooldown_ms > 2000) dynamic_range_cooldown_ms = 2000;
    virtual_camera_width = std::clamp(virtual_camera_width, 1, 7680);
    virtual_camera_heigth = std::clamp(virtual_camera_heigth, 1, 4320);
    virtual_camera_fps = std::clamp(virtual_camera_fps, 1, 240);

    confidence_threshold = std::clamp(confidence_threshold, 0.1f, 0.9f);
    nms_threshold = std::clamp(nms_threshold, 0.1f, 0.9f);
    max_detections = std::clamp(max_detections, 1, 20);
    minSpeedMultiplier = std::clamp(minSpeedMultiplier, 0.001f, 10.0f);
    maxSpeedMultiplier = std::clamp(maxSpeedMultiplier, minSpeedMultiplier, 10.0f);
    nearRadius = std::clamp(nearRadius, 0.0f, 500.0f);
    snapRadius = std::clamp(snapRadius, nearRadius, 500.0f);
    speedCurveExponent = std::clamp(speedCurveExponent, 0.01f, 10.0f);
    snapBoostFactor = std::clamp(snapBoostFactor, 0.0f, 10.0f);
    for (TriggerConfig* trigger : { &trigger_targeting, &trigger_shoot, &trigger_zoom })
    {
        trigger->stop_fire_delay_ms = std::clamp(trigger->stop_fire_delay_ms, 0, 5000);
        trigger->key_delay_ms = std::clamp(trigger->key_delay_ms, 0, 5000);
        trigger->pre_fire_delay_ms = std::clamp(trigger->pre_fire_delay_ms, 0, 5000);
        trigger->fire_duration_ms = std::clamp(trigger->fire_duration_ms, 1, 10000);
        trigger->fire_duration_random_ms = std::clamp(trigger->fire_duration_random_ms, 0, 10000);
        trigger->cooldown_ms = std::clamp(trigger->cooldown_ms, 0, 10000);
        trigger->cooldown_random_ms = std::clamp(trigger->cooldown_random_ms, 0, 10000);
        trigger->zone_offset_x = std::clamp(trigger->zone_offset_x, 0.0f, 1.0f);
        trigger->zone_offset_y = std::clamp(trigger->zone_offset_y, 0.0f, 1.0f);
        trigger->zone_size_x = std::clamp(trigger->zone_size_x, 0.01f, 1.0f);
        trigger->zone_size_y = std::clamp(trigger->zone_size_y, 0.01f, 1.0f);
    }

    // Debug window
    show_window = get_bool("show_window", true);
    show_fps = get_bool("show_fps", false);
    screenshot_button = splitString(get_string("screenshot_button", "None"));
    screenshot_delay = get_long("screenshot_delay", 500);
    // 截图延迟钳制：负值会使 capture.cpp 的 screenshotElapsedMs >= delay 恒真 → 每帧触发截图写盘；
    // 上限取 10 分钟，防止误配超大值导致截图永不触发。
    if (screenshot_delay < 0) screenshot_delay = 0;
    if (screenshot_delay > 600000) screenshot_delay = 600000;
    verbose = get_bool("verbose", false);

    return true;
}

bool Config::saveConfig(const std::string& filename)
{
    std::string target = filename.empty() ? "config.ini" : filename;
    if (target == "config.ini" && !config_path.empty())
    {
        target = config_path;
    }

    std::filesystem::path targetPath(target);
    std::error_code absEc;
    targetPath = std::filesystem::absolute(targetPath, absEc).lexically_normal();
    if (absEc)
    {
        std::cerr << "[配置] 无法解析配置文件路径:" << std::endl;
        std::cerr << target << std::endl;
        return false;
    }
    config_path = targetPath.string();
    target = config_path;

    std::ofstream file(targetPath);
    if (!file.is_open())
    {
        std::cerr << "[配置] 无法写入配置文件:" << std::endl;
        std::cerr << target << std::endl;
        return false;
    }

    file << "# An explanation of the options can be found at:\n";
    file << "# ./docs/config.md\n\n";

    // Capture
    file << "# Capture\n"
        << "capture_method = " << capture_method << "\n"
        << "capture_target = " << capture_target << "\n"
        << "capture_window_title = " << capture_window_title << "\n"
        << "udp_ip = " << udp_ip << "\n"
        << "udp_port = " << udp_port << "\n"
        << "detection_resolution = " << detection_resolution << "\n"
        << "capture_fps = " << capture_fps << "\n"
        << "monitor_idx = " << monitor_idx << "\n"
        << "circle_fov_enabled = " << (circle_fov_enabled ? "true" : "false") << "\n"
        << "circle_fov_radius_percent = " << circle_fov_radius_percent << "\n"
        << "circle_fov_show_preview = " << (circle_fov_show_preview ? "true" : "false") << "\n"
        << "capture_borders = " << (capture_borders ? "true" : "false") << "\n"
        << "capture_cursor = " << (capture_cursor ? "true" : "false") << "\n"
        << "virtual_camera_name = " << virtual_camera_name << "\n"
        << "virtual_camera_width = " << virtual_camera_width << "\n"
        << "virtual_camera_heigth = " << virtual_camera_heigth << "\n"
        << "virtual_camera_fps = " << virtual_camera_fps << "\n\n";

    // Target
    file << "# Target\n"
        << std::fixed << std::setprecision(2)
        << "aim_offset_x = " << aim_offset_x << "\n"
        << "aim_offset_y = " << aim_offset_y << "\n"
        << "auto_aim = " << (auto_aim ? "true" : "false") << "\n"
        << "tracker_enabled = " << (tracker_enabled ? "true" : "false") << "\n"
        << "tracker_overlay_table_enabled = " << (tracker_overlay_table_enabled ? "true" : "false") << "\n"
        << "targeting_mode = " << targeting_mode << "\n\n";

    // Mouse
    file << "# Mouse move\n"
        << "fovX = " << fovX << "\n"
        << "fovY = " << fovY << "\n"
        << "minSpeedMultiplier = " << minSpeedMultiplier << "\n"
        << "maxSpeedMultiplier = " << maxSpeedMultiplier << "\n"

        << std::fixed << std::setprecision(2)
        << "predictionInterval = " << predictionInterval << "\n"
        << "prediction_futurePositions = " << prediction_futurePositions << "\n"
        << "draw_futurePositions = " << (draw_futurePositions ? "true" : "false") << "\n"
        << "kalman_enabled = " << (kalman_enabled ? "true" : "false") << "\n"
        << "kalman_process_noise_position = " << kalman_process_noise_position << "\n"
        << "kalman_process_noise_velocity = " << kalman_process_noise_velocity << "\n"
        << "kalman_measurement_noise = " << kalman_measurement_noise << "\n"
        << "kalman_velocity_damping = " << kalman_velocity_damping << "\n"
        << "kalman_max_velocity = " << kalman_max_velocity << "\n"
        << std::setprecision(0)
        << "kalman_warmup_frames = " << kalman_warmup_frames << "\n"
        << "kalman_compensate_detection_delay = " << (kalman_compensate_detection_delay ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(2)
        << "kalman_additional_prediction_ms = " << kalman_additional_prediction_ms << "\n"
        << "kalman_reset_timeout_sec = " << kalman_reset_timeout_sec << "\n"

        // MouseController 移植模块参数 (依据 鼠标调参指南.md)
        << "mc_enabled = " << (mc_enabled ? "true" : "false") << "\n"
        << "mc_x_tracking = " << mc_x_tracking << "\n"
        << "mc_x_damping = " << mc_x_damping << "\n"
        << "mc_x_maxspeed = " << mc_x_maxspeed << "\n"
        << "mc_x_integral = " << mc_x_integral << "\n"
        << "mc_x_deadzone = " << mc_x_deadzone << "\n"
        << "mc_y_tracking = " << mc_y_tracking << "\n"
        << "mc_y_damping = " << mc_y_damping << "\n"
        << "mc_y_maxspeed = " << mc_y_maxspeed << "\n"
        << "mc_y_integral = " << mc_y_integral << "\n"
        << "mc_y_deadzone = " << mc_y_deadzone << "\n"
        << "mc_maxstep = " << mc_maxstep << "\n"
        << "mc_retarget = " << mc_retarget << "\n"
        << "mc_ahead_min = " << mc_ahead_min << "\n"
        << "mc_ahead_max = " << mc_ahead_max << "\n"
        << "mc_dur_min = " << mc_dur_min << "\n"
        << "mc_dur_max = " << mc_dur_max << "\n"
        << "mc_kalman_q = " << mc_kalman_q << "\n"
        << "mc_kalman_r = " << mc_kalman_r << "\n"

        << "snapRadius = " << snapRadius << "\n"
        << "nearRadius = " << nearRadius << "\n"
        << "speedCurveExponent = " << speedCurveExponent << "\n"
        << std::fixed << std::setprecision(2)
        << "snapBoostFactor = " << snapBoostFactor << "\n"

        << "easynorecoil = " << (easynorecoil ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(1)
        << "easynorecoilstrength = " << easynorecoilstrength << "\n"

        << "# WIN32, KMBOX_NET, KMBOX_A, MAKCU\n"
        << "input_method = " << input_method << "\n\n";

    // 轨迹曲线
    file << "# Mouse curve\n"
        << "wind_mouse_enabled = " << (wind_mouse_enabled ? "true" : "false") << "\n"
        << "wind_G = " << wind_G << "\n"
        << "wind_W = " << wind_W << "\n"
        << "wind_M = " << wind_M << "\n"
        << "wind_D = " << wind_D << "\n\n";


    // kmbox_net
    file << "# Kmbox_net\n"
        << "kmbox_net_ip = " << kmbox_net_ip << "\n"
        << "kmbox_net_port = " << kmbox_net_port << "\n"
        << "kmbox_net_uuid = " << kmbox_net_uuid << "\n\n";

    file << "# Kmbox_a\n"
        << "kmbox_a_pidvid = " << kmbox_a_pidvid << "\n\n";

    // makcu
    file << "# Makcu\n"
        << "makcu_baudrate = " << makcu_baudrate << "\n"
		<< "makcu_port = " << makcu_port << "\n\n";

    // Mouse shooting
    file << "# Mouse shooting\n"
        << "auto_shoot = " << (auto_shoot ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(1)
        << "bScope_multiplier = " << bScope_multiplier << "\n\n";

    // Trigger - Targeting
    file << "# Trigger - Targeting\n"
        << "trigger_targeting_enabled = " << (trigger_targeting.enabled ? "true" : "false") << "\n"
        << "trigger_targeting_continuous = " << (trigger_targeting.continuous ? "true" : "false") << "\n"
        << "trigger_targeting_stop_fire_on_loss = " << (trigger_targeting.stop_fire_on_loss ? "true" : "false") << "\n"
        << "trigger_targeting_stop_fire_delay_ms = " << trigger_targeting.stop_fire_delay_ms << "\n"
        << "trigger_targeting_key_delay_ms = " << trigger_targeting.key_delay_ms << "\n"
        << "trigger_targeting_pre_fire_delay_ms = " << trigger_targeting.pre_fire_delay_ms << "\n"
        << "trigger_targeting_fire_duration_ms = " << trigger_targeting.fire_duration_ms << "\n"
        << "trigger_targeting_fire_duration_random_ms = " << trigger_targeting.fire_duration_random_ms << "\n"
        << "trigger_targeting_cooldown_ms = " << trigger_targeting.cooldown_ms << "\n"
        << "trigger_targeting_cooldown_random_ms = " << trigger_targeting.cooldown_random_ms << "\n"
        << std::fixed << std::setprecision(2)
        << "trigger_targeting_zone_offset_x = " << trigger_targeting.zone_offset_x << "\n"
        << "trigger_targeting_zone_offset_y = " << trigger_targeting.zone_offset_y << "\n"
        << "trigger_targeting_zone_size_x = " << trigger_targeting.zone_size_x << "\n"
        << "trigger_targeting_zone_size_y = " << trigger_targeting.zone_size_y << "\n\n";

    // Trigger - Shoot
    file << "# Trigger - Shoot\n"
        << "trigger_shoot_enabled = " << (trigger_shoot.enabled ? "true" : "false") << "\n"
        << "trigger_shoot_continuous = " << (trigger_shoot.continuous ? "true" : "false") << "\n"
        << "trigger_shoot_stop_fire_on_loss = " << (trigger_shoot.stop_fire_on_loss ? "true" : "false") << "\n"
        << "trigger_shoot_stop_fire_delay_ms = " << trigger_shoot.stop_fire_delay_ms << "\n"
        << "trigger_shoot_key_delay_ms = " << trigger_shoot.key_delay_ms << "\n"
        << "trigger_shoot_pre_fire_delay_ms = " << trigger_shoot.pre_fire_delay_ms << "\n"
        << "trigger_shoot_fire_duration_ms = " << trigger_shoot.fire_duration_ms << "\n"
        << "trigger_shoot_fire_duration_random_ms = " << trigger_shoot.fire_duration_random_ms << "\n"
        << "trigger_shoot_cooldown_ms = " << trigger_shoot.cooldown_ms << "\n"
        << "trigger_shoot_cooldown_random_ms = " << trigger_shoot.cooldown_random_ms << "\n"
        << std::fixed << std::setprecision(2)
        << "trigger_shoot_zone_offset_x = " << trigger_shoot.zone_offset_x << "\n"
        << "trigger_shoot_zone_offset_y = " << trigger_shoot.zone_offset_y << "\n"
        << "trigger_shoot_zone_size_x = " << trigger_shoot.zone_size_x << "\n"
        << "trigger_shoot_zone_size_y = " << trigger_shoot.zone_size_y << "\n\n";

    // Trigger - Zoom
    file << "# Trigger - Zoom\n"
        << "trigger_zoom_enabled = " << (trigger_zoom.enabled ? "true" : "false") << "\n"
        << "trigger_zoom_continuous = " << (trigger_zoom.continuous ? "true" : "false") << "\n"
        << "trigger_zoom_stop_fire_on_loss = " << (trigger_zoom.stop_fire_on_loss ? "true" : "false") << "\n"
        << "trigger_zoom_stop_fire_delay_ms = " << trigger_zoom.stop_fire_delay_ms << "\n"
        << "trigger_zoom_key_delay_ms = " << trigger_zoom.key_delay_ms << "\n"
        << "trigger_zoom_pre_fire_delay_ms = " << trigger_zoom.pre_fire_delay_ms << "\n"
        << "trigger_zoom_fire_duration_ms = " << trigger_zoom.fire_duration_ms << "\n"
        << "trigger_zoom_fire_duration_random_ms = " << trigger_zoom.fire_duration_random_ms << "\n"
        << "trigger_zoom_cooldown_ms = " << trigger_zoom.cooldown_ms << "\n"
        << "trigger_zoom_cooldown_random_ms = " << trigger_zoom.cooldown_random_ms << "\n"
        << std::fixed << std::setprecision(2)
        << "trigger_zoom_zone_offset_x = " << trigger_zoom.zone_offset_x << "\n"
        << "trigger_zoom_zone_offset_y = " << trigger_zoom.zone_offset_y << "\n"
        << "trigger_zoom_zone_size_x = " << trigger_zoom.zone_size_x << "\n"
        << "trigger_zoom_zone_size_y = " << trigger_zoom.zone_size_y << "\n\n";

    // AI
    file << "# AI\n"
        << "backend = " << backend << "\n";
    file << "ai_model = " << ai_model << "\n"
        << std::fixed << std::setprecision(2)
        << "confidence_threshold = " << confidence_threshold << "\n"
        << "nms_threshold = " << nms_threshold << "\n"
        << std::setprecision(0)
        << "max_detections = " << max_detections << "\n"
#ifdef USE_CUDA
        << "export_enable_fp8 = " << (export_enable_fp8 ? "true" : "false") << "\n"
        << "export_enable_fp16 = " << (export_enable_fp16 ? "true" : "false") << "\n"
#endif
        ;

    // CUDA
#ifdef USE_CUDA
    file << "# CUDA\n"
        << "use_cuda_graph = " << (use_cuda_graph ? "true" : "false") << "\n"
        << "use_pinned_memory = " << (use_pinned_memory ? "true" : "false") << "\n"
        << "cuda_device_index = " << cuda_device_index << "\n"
        << "gpuMemoryReserveMB = " << gpuMemoryReserveMB << "\n"
        << "enableGpuExclusiveMode = " << (enableGpuExclusiveMode ? "true" : "false") << "\n"
        << "capture_use_cuda = " << (capture_use_cuda ? "true" : "false") << "\n\n";
#endif

	// System
    file << "# System\n"
        << "cpuCoreReserveCount = " << cpuCoreReserveCount << "\n"
        << "systemMemoryReserveMB = " << systemMemoryReserveMB << "\n\n";

    // Buttons
    file << "# Buttons\n"
        << "button_targeting = " << joinStrings(button_targeting) << "\n"
        << "button_shoot = " << joinStrings(button_shoot) << "\n"
        << "button_zoom = " << joinStrings(button_zoom) << "\n"
        << "button_exit = " << joinStrings(button_exit) << "\n"
        << "button_pause = " << joinStrings(button_pause) << "\n"
        << "button_reload_config = " << joinStrings(button_reload_config) << "\n"
        << "button_open_overlay = " << joinStrings(button_open_overlay) << "\n"
        << "enable_arrows_settings = " << (enable_arrows_settings ? "true" : "false") << "\n\n";

    file << "# Mouse hotkeys (maximum 3, hold only)\n";
    for (std::size_t i = 0; i < MAX_MOUSE_HOTKEYS; ++i)
    {
        const auto& hotkey = mouse_hotkeys[i];
        const std::string prefix = "mouse_hotkey_" + std::to_string(i) + "_";
        file << prefix << "id = " << hotkey.id << "\n"
             << prefix << "buttons = " << joinStrings(hotkey.buttons) << "\n"
             << prefix << "priority = " << hotkey.priority << "\n"
             << prefix << "creation_order = " << hotkey.creationOrder << "\n"
             << prefix << "enabled = " << (hotkey.enabled ? "true" : "false") << "\n";
        std::vector<std::string> localKeys;
        localKeys.reserve(hotkey.localConfig.size());
        for (const auto& entry : hotkey.localConfig)
            localKeys.push_back(entry.first);
        std::sort(localKeys.begin(), localKeys.end());
        file << prefix << "local_keys = " << joinStrings(localKeys, "|") << "\n";
        for (const auto& key : localKeys)
            file << prefix << "local_" << key << " = " << hotkey.localConfig.at(key) << "\n";
    }
    file << "\n";

    // Overlay
    file << "# Overlay\n"
        << "overlay_exclude_from_capture = " << (overlay_exclude_from_capture ? "true" : "false") << "\n"
        << std::setprecision(0)
        << "overlay_x = " << overlay_x << "\n"
        << "overlay_y = " << overlay_y << "\n"
        << "overlay_width = " << overlay_width << "\n"
        << "overlay_height = " << overlay_height << "\n\n";

    file << "# Game Overlay\n"
        << "game_overlay_enabled = " << (game_overlay_enabled ? "true" : "false") << "\n"
        << "game_overlay_max_fps = " << game_overlay_max_fps << "\n"
        << "game_overlay_draw_boxes = " << (game_overlay_draw_boxes ? "true" : "false") << "\n"
        << "game_overlay_compensate_latency = " << (game_overlay_compensate_latency ? "true" : "false") << "\n"
        << "game_overlay_draw_future = " << (game_overlay_draw_future ? "true" : "false") << "\n"
        << "game_overlay_draw_wind_tail = " << (game_overlay_draw_wind_tail ? "true" : "false") << "\n"
        << "game_overlay_draw_frame = " << (game_overlay_draw_frame ? "true" : "false") << "\n"
        << "game_overlay_draw_circle_fov = " << (game_overlay_draw_circle_fov ? "true" : "false") << "\n"
        << "game_overlay_show_target_correction = " << (game_overlay_show_target_correction ? "true" : "false") << "\n"
        << "game_overlay_box_a = " << game_overlay_box_a << "\n"
        << "game_overlay_box_r = " << game_overlay_box_r << "\n"
        << "game_overlay_box_g = " << game_overlay_box_g << "\n"
        << "game_overlay_box_b = " << game_overlay_box_b << "\n"
        << "game_overlay_frame_a = " << game_overlay_frame_a << "\n"
        << "game_overlay_frame_r = " << game_overlay_frame_r << "\n"
        << "game_overlay_frame_g = " << game_overlay_frame_g << "\n"
        << "game_overlay_frame_b = " << game_overlay_frame_b << "\n"
        << std::fixed << std::setprecision(2)
        << "game_overlay_box_thickness = " << game_overlay_box_thickness << "\n"
        << "game_overlay_frame_thickness = " << game_overlay_frame_thickness << "\n"
        << "game_overlay_future_point_radius = " << game_overlay_future_point_radius << "\n"
        << "game_overlay_future_alpha_falloff = " << game_overlay_future_alpha_falloff << "\n\n";

    file << "game_overlay_icon_enabled = " << (game_overlay_icon_enabled ? "true" : "false") << "\n"
        << "game_overlay_icon_path = " << game_overlay_icon_path << "\n"
        << "game_overlay_icon_width = " << game_overlay_icon_width << "\n"
        << "game_overlay_icon_height = " << game_overlay_icon_height << "\n"
        << std::fixed << std::setprecision(2)
        << "game_overlay_icon_offset_x = " << game_overlay_icon_offset_x << "\n"
        << std::fixed << std::setprecision(2)
        << "game_overlay_icon_offset_y = " << game_overlay_icon_offset_y << "\n"
        << "game_overlay_icon_anchor = " << game_overlay_icon_anchor << "\n"
        << "game_overlay_icon_class = " << game_overlay_icon_class << "\n\n";

    file << "# Data Collection\n"
        << "collect_data_while_playing = " << (collect_data_while_playing ? "true" : "false") << "\n"
        << "collect_only_when_aimbot_running = " << (collect_only_when_aimbot_running ? "true" : "false") << "\n"
        << "collect_only_when_targets_present = " << (collect_only_when_targets_present ? "true" : "false") << "\n"
        << "collect_save_every_n_frames = " << collect_save_every_n_frames << "\n"
        << "collect_jpeg_quality = " << collect_jpeg_quality << "\n"
        << "collect_output_dir = " << collect_output_dir << "\n"
        << "auto_label_data = " << (auto_label_data ? "true" : "false") << "\n"
        << std::fixed << std::setprecision(2)
        << "auto_label_min_conf = " << auto_label_min_conf << "\n"
        << std::setprecision(0)
        << "auto_label_max_boxes = " << auto_label_max_boxes << "\n"
        << "auto_label_record_classes = " << auto_label_record_classes << "\n\n";

    // Dynamic Range
    file << "# Dynamic Range\n"
        << "dynamic_range_enabled = " << (dynamic_range_enabled ? "true" : "false") << "\n"
        << "dynamic_range_shrink_scope = " << dynamic_range_shrink_scope << "\n"
        << "dynamic_range_shrink_duration_ms = " << dynamic_range_shrink_duration_ms << "\n"
        << "dynamic_range_cooldown_ms = " << dynamic_range_cooldown_ms << "\n"
        << "dynamic_range_target_classes = " << dynamic_range_target_classes << "\n";

    // Classes (global target class system)
    file << "# Target Classes (global)\n";
    for (int i = 0; i < FIXED_TARGET_CLASS_COUNT; ++i)
    {
        file << "class_" << i << "_enabled = " << (class_enabled[i] ? "true" : "false") << "\n";
    }
    file << "class_player = " << class_player << "\n"
        << "class_head = " << class_head << "\n\n";

    // Debug
    file << "# Debug\n"
        << "show_window = " << (show_window ? "true" : "false") << "\n"
        << "show_fps = " << (show_fps ? "true" : "false") << "\n"
        << "screenshot_button = " << joinStrings(screenshot_button) << "\n"
        << "screenshot_delay = " << screenshot_delay << "\n"
        << "verbose = " << (verbose ? "true" : "false") << "\n\n";

    // Active game
    file << "# Active game profile\n";
    file << "active_game = " << active_game << "\n\n";
    file << std::defaultfloat << std::setprecision(6);
    file << "[Games]\n";
    for (auto& kv : game_profiles)
    {
        auto & gp = kv.second;
        file << gp.name << " = "
             << gp.sens << "," << gp.yaw;
        file << "," << gp.pitch;
        if (gp.fovScaled)
            file << ",true," << gp.baseFOV;
        file << "\n";
    }

    file.close();
    return true;
}

bool Config::isClassEnabled(int classId) const noexcept
{
    return classId >= 0 && classId < FIXED_TARGET_CLASS_COUNT && class_enabled[classId];
}


std::filesystem::path Config::configPath() const
{
    return config_path.empty() ? std::filesystem::path("config.ini") : std::filesystem::path(config_path);
}

bool Config::resetToFactoryDefaults()
{
    const std::filesystem::path target = configPath();
    std::error_code ec;
    if (std::filesystem::exists(target, ec))
    {
        const std::filesystem::path backup = target.string() + ".before_factory_reset";
        std::filesystem::remove(backup, ec);
        ec.clear();
        std::filesystem::rename(target, backup, ec);
        if (ec)
            return false;
    }
    return loadConfig(target.string());
}

const Config::GameProfile& Config::currentProfile() const
{
    auto it = game_profiles.find(active_game);
    if (it != game_profiles.end()) return it->second;
    throw std::runtime_error("Active game profile not found: " + active_game);
}

std::pair<double, double> Config::degToCounts(double degX, double degY, double fovNow) const
{
    const auto& gp = currentProfile();
    double scale = (gp.fovScaled && gp.baseFOV > 1.0) ? (fovNow / gp.baseFOV) : 1.0;

    if (gp.sens == 0.0 || gp.yaw == 0.0 || gp.pitch == 0.0)
        return { 0.0, 0.0 };

    double cx = degX / (gp.sens * gp.yaw * scale);
    double cy = degY / (gp.sens * gp.pitch * scale);
    return { cx, cy };
}
