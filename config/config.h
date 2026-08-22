#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <filesystem>
#include <array>
#include <cstddef>

#include "runtime/trigger_system.h"

class Config
{
public:
    static constexpr std::size_t MAX_MOUSE_HOTKEYS = 3;

    struct MouseHotkey
    {
        std::string id;
        std::vector<std::string> buttons;
        int priority = 0;
        int creationOrder = 0;
        std::unordered_map<std::string, std::string> localConfig;
        bool enabled = true;

        bool localBool(const std::string& key, bool fallback) const;
        int localInt(const std::string& key, int fallback) const;
        float localFloat(const std::string& key, float fallback) const;
        std::string localString(const std::string& key, const std::string& fallback) const;
        void setLocalBool(const std::string& key, bool value);
        void setLocalInt(const std::string& key, int value);
        void setLocalFloat(const std::string& key, float value);
        void setLocalString(const std::string& key, const std::string& value);
    };

    using MouseHotkeyContainer = std::array<MouseHotkey, MAX_MOUSE_HOTKEYS>;

    // Pure logic: button states are keyed by binding name; all hotkeys use hold semantics.
    static const MouseHotkey* selectActiveMouseHotkey(
        const MouseHotkeyContainer& hotkeys,
        const std::unordered_map<std::string, bool>& pressedButtons);

    // Capture
    std::string capture_method; // "duplication_api", "winrt", "virtual_camera", "udp_capture"
    std::string capture_target;
    std::string capture_window_title;
    std::string udp_ip;
    int udp_port;
    int detection_resolution;
    int capture_fps;
    int monitor_idx;
    bool circle_fov_enabled;
    int circle_fov_radius_percent;
    bool circle_fov_show_preview;
    bool capture_borders;
    bool capture_cursor;
    std::string virtual_camera_name;
    int virtual_camera_width;
    int virtual_camera_heigth;
    int virtual_camera_fps;

    // Target
    float aim_offset_x;    // 目标框归一化瞄准点 X，0=左边界，1=右边界
    float aim_offset_y;    // 目标框归一化瞄准点 Y，0=上边界，1=下边界
    bool auto_aim;
    bool tracker_enabled;
    bool tracker_overlay_table_enabled;
    std::string targeting_mode; // "closest_center" or "largest_box"

    // Mouse
    int fovX;
    int fovY;
    float minSpeedMultiplier;
    float maxSpeedMultiplier;

    float predictionInterval;
    int prediction_futurePositions;
    bool draw_futurePositions;
    bool kalman_enabled;
    float kalman_process_noise_position;
    float kalman_process_noise_velocity;
    float kalman_measurement_noise;
    float kalman_velocity_damping;
    float kalman_max_velocity;
    int kalman_warmup_frames;
    bool kalman_compensate_detection_delay;
    float kalman_additional_prediction_ms;
    float kalman_reset_timeout_sec;

    float snapRadius;
    float nearRadius;
    float speedCurveExponent;
    float snapBoostFactor;
    bool wind_mouse_enabled;
    float wind_G;
    float wind_W;
    float wind_M;
    float wind_D;

    bool easynorecoil;
    float easynorecoilstrength;
    std::string input_method; // "WIN32", "KMBOX_NET", "KMBOX_A", "MAKCU"

    // Mouse curve (trajectory curve & perturbation)
    bool curve_enabled;
    float curve_intensity;
    float perturbation_strength;
    float curve_max_speed;
    float curve_distance;

    // ---- MouseController 移植模块参数 (依据 鼠标调参指南.md) ----
    // 总开关: 启用 MouseController (Kalman+MinJerk+PID) 替代原比例移动算法。
    // 预测/轨迹/修正三功能为模块固有行为, 始终启用, 不再单独开关。
    bool  mc_enabled;
    // 分轴参数 (X / Y 各一套): tracking / damping / maxSpeed / integral / deadzone
    float mc_x_tracking, mc_x_damping, mc_x_maxspeed, mc_x_integral, mc_x_deadzone;
    float mc_y_tracking, mc_y_damping, mc_y_maxspeed, mc_y_integral, mc_y_deadzone;
    // 全局参数
    float mc_maxstep;   // 单帧最大移动像素量 (防瞬移)
    float mc_retarget;  // 目标跳变超过该值才重新规划轨迹
    // 算法参数 (自适应)
    float mc_ahead_min, mc_ahead_max;  // 自适应前瞻范围 (秒)
    float mc_dur_min,   mc_dur_max;    // 自适应轨迹时长范围 (秒)
    float mc_kalman_q,  mc_kalman_r;   // Kalman 过程噪声 q / 测量噪声 r

    // kmbox_net
    std::string kmbox_net_ip;
    std::string kmbox_net_port;
    std::string kmbox_net_uuid;

    // kmbox_a
    std::string kmbox_a_pidvid; // PIDVID in one field, format: PPPPVVVV

    // makcu
    int makcu_baudrate;
    std::string makcu_port;

    // Mouse shooting
    bool auto_shoot;
    float bScope_multiplier;

    // Trigger (per-hotkey)
    TriggerConfig trigger_targeting;
    TriggerConfig trigger_shoot;
    TriggerConfig trigger_zoom;

    // AI
    std::string backend;
    std::string ai_model;
    float confidence_threshold;
    float nms_threshold;
    int max_detections;
#ifdef USE_CUDA
    bool export_enable_fp8;
    bool export_enable_fp16;
#endif
    bool fixed_input_size;

    // CUDA
#ifdef USE_CUDA
    bool use_cuda_graph;
    bool use_pinned_memory;
    int cuda_device_index;
    int gpuMemoryReserveMB;
    bool enableGpuExclusiveMode;
    bool capture_use_cuda;
#endif

    // System
    int cpuCoreReserveCount;
    int systemMemoryReserveMB;

    // Buttons
    std::vector<std::string> button_targeting;
    std::vector<std::string> button_shoot;
    std::vector<std::string> button_zoom;
    std::vector<std::string> button_exit;
    std::vector<std::string> button_pause;
    std::vector<std::string> button_reload_config;
    std::vector<std::string> button_open_overlay;
    bool enable_arrows_settings;

    // Core mouse hotkeys. Empty slots are disabled and preserve room for future UI.
    MouseHotkeyContainer mouse_hotkeys{};

    // Applies the storage invariants without changing the legacy button API.
    void normalizeMouseHotkeys();
    void resetHotkeyClassSubsets();

    // Overlay
    bool overlay_exclude_from_capture;
    int overlay_x;
    int overlay_y;
    int overlay_width;
    int overlay_height;

    // Game Overlay
    bool game_overlay_enabled;
    int game_overlay_max_fps;
    bool game_overlay_draw_boxes;
    bool game_overlay_compensate_latency;
    bool game_overlay_draw_future;
    bool game_overlay_draw_curve_trail;
    bool game_overlay_draw_wind_tail;
    bool game_overlay_draw_frame;
    bool game_overlay_draw_circle_fov;
    bool game_overlay_show_target_correction;
    int game_overlay_box_a;
    int game_overlay_box_r;
    int game_overlay_box_g;
    int game_overlay_box_b;
    int game_overlay_frame_a;
    int game_overlay_frame_r;
    int game_overlay_frame_g;
    int game_overlay_frame_b;
    float game_overlay_box_thickness;
    float game_overlay_frame_thickness;
    float game_overlay_future_point_radius;
    float game_overlay_future_alpha_falloff;

    bool game_overlay_icon_enabled;
    std::string game_overlay_icon_path;
    int game_overlay_icon_width;
    int game_overlay_icon_height;
    float game_overlay_icon_offset_x;
    float game_overlay_icon_offset_y;
    std::string game_overlay_icon_anchor; // "center", "top", "bottom", "head"
    int game_overlay_icon_class; // -1 = all

    // Data collection
    bool collect_data_while_playing;
    bool collect_only_when_aimbot_running;
    bool collect_only_when_targets_present;
    int collect_save_every_n_frames;
    int collect_jpeg_quality;
    std::string collect_output_dir;
    bool auto_label_data;
    float auto_label_min_conf;
    int auto_label_max_boxes;
    std::string auto_label_record_classes;

    void clampGameOverlayColor()
    {
        auto clamp255 = [](int& v) { if (v < 0) v = 0; if (v > 255) v = 255; };
        clamp255(game_overlay_box_a);
        clamp255(game_overlay_box_r);
        clamp255(game_overlay_box_g);
        clamp255(game_overlay_box_b);
        clamp255(game_overlay_frame_a);
        clamp255(game_overlay_frame_r);
        clamp255(game_overlay_frame_g);
        clamp255(game_overlay_frame_b);
    }

    // Dynamic Range
    bool dynamic_range_enabled;
    int dynamic_range_shrink_scope;
    int dynamic_range_shrink_duration_ms;
    int dynamic_range_cooldown_ms;
    std::string dynamic_range_target_classes;

    // Classes (global target class system)
    static constexpr int MAX_CLASSES = 80;
    static constexpr int FIXED_TARGET_CLASS_COUNT = 15;
    static constexpr int MAX_MODEL_CLASSES = 19; // 文档约束：模型类别数 1..19
    bool class_enabled[MAX_CLASSES];   // 类别是否启用瞄准
    int class_player;
    int class_head;

    // Debug
    bool show_window;
    bool show_fps;
    std::vector<std::string> screenshot_button;
    int screenshot_delay;
    bool verbose;

    struct GameProfile
    {
        std::string name;
        double sens;
        double yaw;
        double pitch;
        bool fovScaled;
        double baseFOV;
    };

    std::unordered_map<std::string, GameProfile> game_profiles;
    std::string                                  active_game;

    const GameProfile & currentProfile() const;
    std::pair<double, double> degToCounts(double degX, double degY, double fovNow) const;

    bool isClassEnabled(int classId) const noexcept;
    std::filesystem::path configPath() const;

    bool loadConfig(const std::string& filename = "config.ini");
    bool saveConfig(const std::string& filename = "config.ini");
    bool resetToFactoryDefaults();

    std::string joinStrings(const std::vector<std::string>& vec, const std::string& delimiter = ",");
private:
    void normalizeHotkeyClassAimOffsets();
    std::vector<std::string> splitString(const std::string& str, char delimiter = ',');
    std::string config_path;
};

#endif // CONFIG_H
