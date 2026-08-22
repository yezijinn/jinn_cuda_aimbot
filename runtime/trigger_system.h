#pragma once

#include <algorithm>
#include <chrono>
#include <random>
#include <string>

struct TriggerZone
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// Returns false for invalid boxes, dimensions, or non-finite parameters.
bool calculateTriggerZone(const float* box,
                          int detectionWidth,
                          int detectionHeight,
                          float offsetX,
                          float offsetY,
                          float sizeX,
                          float sizeY,
                          TriggerZone& outZone);

bool isCrosshairInsideTriggerZone(const TriggerZone& zone,
                                  int detectionWidth,
                                  int detectionHeight);

// ── Per-hotkey trigger configuration ──
struct TriggerConfig {
    bool enabled = false;
    bool continuous = false;
    bool stop_fire_on_loss = true;
    int stop_fire_delay_ms = 200;
    int key_delay_ms = 50;
    int pre_fire_delay_ms = 100;
    int fire_duration_ms = 500;
    int fire_duration_random_ms = 100;
    int cooldown_ms = 300;
    int cooldown_random_ms = 100;
    float zone_offset_x = 0.1f;      // 内部矩形左边界，归一化 0..1
    float zone_offset_y = 0.1f;      // 内部矩形上边界，归一化 0..1
    float zone_size_x = 0.8f;        // 内部矩形宽度，归一化 0..1
    float zone_size_y = 0.8f;        // 内部矩形高度，归一化 0..1
};

// ── Trigger slot identifiers ──
enum class TriggerSlotId {
    Targeting = 0,
    Shoot = 1,
    Zoom = 2,
    Count = 3
};

inline const char* triggerSlotName(TriggerSlotId id) {
    switch (id) {
        case TriggerSlotId::Targeting: return "瞄准";
        case TriggerSlotId::Shoot:     return "射击";
        case TriggerSlotId::Zoom:      return "开镜";
        default:                       return "未知";
    }
}

// ── Trigger state machine phases ──
enum class TriggerPhase {
    Idle,         // inactive
    KeyDelay,     // waiting after the crosshair enters the trigger zone (key_delay_ms)
    Armed,        // ready, waiting for target in zone
    PreFireDelay, // target in zone, waiting pre_fire_delay_ms
    Firing,       // actively firing for fire_duration_ms (+random)
    Cooldown      // post-fire cooldown for cooldown_ms (+random)
};

// ── Per-slot runtime state ──
struct TriggerSlotState {
    TriggerPhase phase = TriggerPhase::Idle;
    std::chrono::steady_clock::time_point phaseStart;
    int activeDurationMs = 0;   // total fire duration (incl. random)
    int activeCooldownMs = 0;   // total cooldown (incl. random)
    bool targetInZone = false;
    std::chrono::steady_clock::time_point targetLostTime;
    bool targetLost = false;

    void reset() {
        phase = TriggerPhase::Idle;
        targetInZone = false;
        targetLost = false;
    }
};

// ── Trigger system manages all slots & continuous trigger logic ──
class TriggerSystem {
public:
    TriggerSystem() : rng_(std::random_device{}()) {}

    // Update config for all slots (call from config mutex scope)
    // 防御性 clamp：TriggerSystem 作为独立可复用模块，应对任意调用方传入的
    // 非法时长自洽，而非依赖上层 mouse_thread_loop 的 clamp。范围与上层一致。
    // 合法输入经 clamp 后值不变；非法值被钳到安全区间，避免状态机出现
    // 负 cooldown（elapsed>=负 恒真→瞬间回 Armed）或 fire_duration<1 的异常行为。
    static void sanitizeConfig(TriggerConfig& cfg) {
        cfg.fire_duration_ms        = std::clamp(cfg.fire_duration_ms, 1, 10000);
        cfg.fire_duration_random_ms = std::clamp(cfg.fire_duration_random_ms, 0, 10000);
        cfg.cooldown_ms             = std::clamp(cfg.cooldown_ms, 0, 10000);
        cfg.cooldown_random_ms      = std::clamp(cfg.cooldown_random_ms, 0, 10000);
        // 注意：不写 std::max(0, x)，因本项目大量 TU 包含 <windows.h> 且未定义
        // NOMINMAX，Windows 注入的 max 宏会与 std::max 模板冲突导致 C2589。
        // 用条件表达式规避该宏，语义等价且零成本。
        if (cfg.stop_fire_delay_ms < 0) cfg.stop_fire_delay_ms = 0;
        if (cfg.key_delay_ms < 0)       cfg.key_delay_ms = 0;
        if (cfg.pre_fire_delay_ms < 0)  cfg.pre_fire_delay_ms = 0;
    }

    void updateConfig(const TriggerConfig& targeting,
                      const TriggerConfig& shoot,
                      const TriggerConfig& zoom) {
        TriggerConfig t = targeting;
        TriggerConfig s = shoot;
        TriggerConfig z = zoom;
        sanitizeConfig(t);
        sanitizeConfig(s);
        sanitizeConfig(z);
        configs_[static_cast<int>(TriggerSlotId::Targeting)] = std::move(t);
        configs_[static_cast<int>(TriggerSlotId::Shoot)]     = std::move(s);
        configs_[static_cast<int>(TriggerSlotId::Zoom)]      = std::move(z);
    }

    // Per-frame update: run state machines, return whether mouse should be pressed
    // activeTargetBox: current locked target's bounding box (cx,cy,w,h) in detection coords,
    //   or nullptr if no target
    // keysHeld: bitmask of which hotkeys are physically pressed
    //   bit 0 = targeting, bit 1 = shoot, bit 2 = zoom
    // detectionWidth/Height: trigger-zone coordinate space dimensions
    bool update(const float* activeTargetBox,
                int keysHeld,
                int detectionWidth,
                int detectionHeight);

    // Reset all state (on config change, etc.)
    void resetAll();

    // Which slot is the continuous trigger active for (-1 = none)
    int continuousSlot() const { return continuousSlot_; }

private:
    TriggerConfig configs_[static_cast<int>(TriggerSlotId::Count)];
    TriggerSlotState states_[static_cast<int>(TriggerSlotId::Count)];
    std::mt19937 rng_;

    // Continuous trigger management
    int continuousSlot_ = -1;
    bool continuousTemporarilyDisabled_ = false;

    // Previous key states for edge detection
    int prevKeysHeld_ = 0;

    bool isTargetInTriggerZone(const float* box, int detectionResolution,
                               float offsetX, float offsetY,
                               float sizeX, float sizeY);

    void runSlotStateMachine(TriggerSlotId slot, const float* activeTargetBox,
                             bool slotActive, int detectionWidth, int detectionHeight);
};
