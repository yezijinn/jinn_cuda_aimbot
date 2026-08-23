#pragma once

#include <random>

// ============================================================================
// Humanizer.h
// 人手模拟 (拟人化轨迹扰动) —— 可选模块
//
// 作用: 对最终移动增量施加拟人化扰动, 使鼠标轨迹不像机器 (完美平滑/直线),
//       而更接近真人操作 (有微抖、路径弯曲、末端轻微过冲、加减速不规律)。
//
// 启用规则: 所有参数写 0 表示不启用对应功能; 全部为 0 时模块整体关闭(零开销)。
//
// 参数说明:
//   microJitter    : 微观抖动幅度 (px)。移动越快抖动越大 (真实手抖特性)。
//   wobble         : 曲线扰动幅度 (px)。垂直运动方向的低频摆动, 让路径弯曲。
//   overshoot      : 末端过冲幅度 (px)。接近目标时轻微冲过, 再由控制器自然回正。
//   speedVariation : 速度波动比例 (0~1)。低频乘性扰动, 加减速不规律。
// ============================================================================

struct HumanizerSettings
{
    float microJitter    = 0.0f; // 微观抖动幅度 (px)
    float wobble         = 0.0f; // 曲线扰动幅度 (px)
    float overshoot      = 0.0f; // 末端过冲幅度 (px)
    float speedVariation = 0.0f; // 速度波动比例 (0~1)
    float powerLaw       = 0.0f; // 2/3 幂律速度整形强度 (0~1, 0=关闭)
};

class Humanizer
{
public:
    Humanizer();

    void setSettings(const HumanizerSettings& s);
    const HumanizerSettings& settings() const { return m_s; }

    // 任一参数 > 0 即视为启用
    bool enabled() const;

    // 清空内部状态 (相位 / 过冲余量)
    void reset();

    // 对移动增量 (dx, dy) 就地施加拟人扰动
    //   speed        : 当前移动速度 (px/s)
    //   distToTarget : 距目标距离 (px)
    //   dt           : 时间步长 (秒)
    void apply(float& dx, float& dy, float speed, float distToTarget, float dt);

private:
    HumanizerSettings m_s;
    std::mt19937 m_rng;

    float m_wobblePhase = 0.0f;   // 曲线扰动相位
    float m_speedPhase  = 0.0f;   // 速度波动相位
    int   m_overshootRemaining = 0; // 剩余过冲帧数
    int   m_overshootCooldown = 0;  // 过冲后冷却帧数 (防大 tracking 下反复触发振荡)
    float m_oshootDX = 0.0f, m_oshootDY = 0.0f; // 每帧过冲增量
    bool  m_hasStep = false;          // 2/3 幂律: 是否有上一步增量
    float m_prevDx = 0.0f, m_prevDy = 0.0f; // 2/3 幂律: 上一帧增量方向
    std::normal_distribution<float> m_gauss{0.0f, 1.0f}; // 标准正态分布 (缓存, apply 时乘幅度)
};
