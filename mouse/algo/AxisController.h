#pragma once

#include "PIDController.h"
#include <cmath>

// ============================================================================
// AxisController.h
// 单轴 (X 或 Y) 运动控制器 + 语义化调参面板
//
// 调参面板 AxisTuning 共 5 个分轴参数, X 轴与 Y 轴各持有一套, 互不影响。
// 每个参数均为语义化命名, 并标注"调大收益 / 过大风险", 便于用户直观取舍:
//
//   参数          作用                     调大效果          过大风险
//   -----------   ----------------------   ---------------   ---------------
//   maxSpeed      跟踪目标的速度上限        锁得更快          过冲震荡
//   damping       抑制过冲与震荡 (Kd)       消除过冲          高频抖动
//   tracking      对移动目标的跟踪力度(Kp)  跟死移动目标      过冲晃动
//   integral      跟不上时的累积增益 (Ki)   改善跟踪滞后      机械抽帧
//   deadzone      死区范围内不输出          减少细碎抖动      影响精度
//
// 注意: "单帧最大移动像素量(maxStep)"是全局参数, 不属于单轴面板,
//       由 MouseController 对合成位移统一限制 (见 MouseController.h)。
// ============================================================================

// ---- 单轴调参面板 (5 参数) ----
struct AxisTuning
{
    float maxSpeed;   // 最大速度 (px/s)
    float damping;    // 震荡抑制 -> PID 微分 Kd
    float tracking;   // 追踪强度 -> PID 比例 Kp
    float integral;   // 积分增益 -> PID 积分 Ki
    float deadzone;   // 死区 (px)

    AxisTuning()
        : maxSpeed(1500.0f)
        , damping(0.05f)
        , tracking(3.0f)
        , integral(0.0f)
        , deadzone(2.0f)
    {
    }
};

// ---- 单轴控制器: 死区 -> PID ----
class AxisController
{
public:
    AxisController();

    // 设置整轴参数 (自动同步到底层 PID)
    void setTuning(const AxisTuning& t);
    const AxisTuning& tuning() const { return m_tuning; }

    // 由位置误差计算本帧移动增量 (前馈 + 反馈)
    //   error       : 期望位置 - 实际位置
    //   dt          : 时间步长
    //   feedforward : 前馈速度 (通常为轨迹参考速度), 承担主体速度,
    //                 使 PID 只修正残差, 消除跟踪移动目标的稳态滞后
    // 返回: 本帧移动增量 (已含死区处理; 单帧限幅由 MouseController 全局执行)
    float compute(float error, float dt, float feedforward = 0.0f);

    // 清空内部状态
    void reset();

    // 上一次 PID 输出速度 (供打印/监控)
    float lastVelocity() const { return m_lastVelocity; }

    // 专家级: 直接访问底层 PID
    PIDController& pid() { return m_pid; }

private:
    AxisTuning    m_tuning;       // 调参面板
    PIDController m_pid;          // 底层 PID
    float         m_lastVelocity; // 上一次输出速度
};
