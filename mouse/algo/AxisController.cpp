#include "AxisController.h"

// ============================================================================
// AxisController.cpp
// ============================================================================

AxisController::AxisController()
    : m_lastVelocity(0.0f)
{
    setTuning(AxisTuning()); // 用默认面板初始化底层 PID
}

void AxisController::setTuning(const AxisTuning& t)
{
    // 参数归一化 (本轮审查加固): deadzone 为 NaN/负值时钳为 0。
    // NaN deadzone 会使死区判定 fabs(error) < NaN 恒为 false (死区永不生效),
    // 钳为 0 后行为一致 (0 死区同样永不触发), 但语义明确、可复现;
    // 其余参数 (maxSpeed/tracking/integral/damping) 已由 PID setter 内部防护。
    m_tuning = t;
    if (!std::isfinite(m_tuning.deadzone) || m_tuning.deadzone < 0.0f)
        m_tuning.deadzone = 0.0f;

    // (第七轮加固) maxSpeed 防护: isfinite 仅挡 NaN/Inf, 不挡"巨大但有限"值。
    // compute() 的 [3] 限幅比较 `velocity > maxSpeed` 对 NaN 恒为 false,
    // NaN maxSpeed 会静默绕过速度限幅 (前馈直通, 速度失控风险);
    // 负值无意义 (与 PID setMaxOutput 的非正按 0 策略不一致, 但此处语义为
    // "速度限制", 负值属错误输入)。NaN/Inf/负值回退默认 1500;
    // == 0 保留 (maxSpeed=0 语义 = 停用该轴, PID maxOutput=0 使输出恒 0)。
    if (!std::isfinite(m_tuning.maxSpeed) || m_tuning.maxSpeed < 0.0f)
        m_tuning.maxSpeed = 1500.0f;

    // 语义参数 -> 底层 PID 映射
    //   tracking -> Kp, integral -> Ki, damping -> Kd
    m_pid.setGains(m_tuning.tracking, m_tuning.integral, m_tuning.damping);
    //   maxSpeed -> 速度输出限幅
    m_pid.setMaxOutput(m_tuning.maxSpeed);
}

void AxisController::reset()
{
    m_pid.reset();
    m_lastVelocity = 0.0f;
}

float AxisController::compute(float error, float dt, float feedforward)
{
    // dt 防护: 非正 / NaN / Inf 一律钳制为最小步长。
    // 注意不能只判 dt <= 0 (NaN 比较恒为 false, 会逃逸, 导致 delta = velocity*NaN)。
    if (!(dt > 0.0f) || !std::isfinite(dt))
        dt = 1.0e-3f;

    // 前馈有效性防护 (第五轮加固): NaN/Inf 前馈按 0 处理 (模块独立边界)。
    // 否则 NaN 经 velocity 传播 -> delta = NaN*dt = NaN, 污染调用方鼠标位置;
    // 主调用路径 (MouseController 传轨迹参考速度) 恒有限, 此防护零影响。
    if (!std::isfinite(feedforward))
        feedforward = 0.0f;

    // ---- [1] 死区: 误差进入死区 -> 不动, 并清积分防止末端抖动与积分累积 ----
    if (std::fabs(error) < m_tuning.deadzone)
    {
        m_pid.reset();
        m_lastVelocity = 0.0f;
        return 0.0f;
    }

    // ---- [2] 前馈(轨迹参考速度) + 反馈(PID位置误差修正) ----
    float pidOut  = m_pid.computeOutput(error, dt);
    float velocity = feedforward + pidOut;

    // ---- [3] 饱和感知抗积分饱和 (关闭前馈盲区) ----
    // 仅当"合成速度未达 maxSpeed"或"PID 修正方向有助于退出饱和"时才累加积分;
    // 若轴已被前馈+PID 推入饱和且 PID 仍在同向加力, 则冻结积分, 避免 windup 累积。
    bool saturatedHigh = velocity >  m_tuning.maxSpeed;
    bool saturatedLow  = velocity < -m_tuning.maxSpeed;
    bool pushingHigh   = saturatedHigh && (pidOut > 0.0f);
    bool pushingLow    = saturatedLow  && (pidOut < 0.0f);
    m_pid.commit(error, dt, !(pushingHigh || pushingLow));

    // ---- [4] 整体速度限幅 (前馈+反馈合成后再限到 maxSpeed) ----
    if (velocity >  m_tuning.maxSpeed) velocity =  m_tuning.maxSpeed;
    if (velocity < -m_tuning.maxSpeed) velocity = -m_tuning.maxSpeed;
    m_lastVelocity = velocity;

    // ---- [6] 速度积分 -> 本帧移动增量 ----
    // 注意: 单帧最大移动像素限制为全局约束, 不在此处理,
    //       由 MouseController 对 (moveX, moveY) 的合成位移统一限制。
    float delta = velocity * dt;

    return delta;
}
