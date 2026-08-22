#include "PIDController.h"
#include <cmath>

// ============================================================================
// PIDController.cpp
//
// 稳定性设计 (本次审查加固):
//   - dt 有效性防护: 非正 / NaN / Inf 一律钳制为最小步长 (原判定 dt<=0 不覆盖 NaN)
//   - error 有效性防护: NaN/Inf 误差直接复位并输出 0, 防止状态被污染
//   - 构造参数有效性防护: 增益 NaN/Inf 按 0, 限幅非正/NaN 按 0, 防止首帧即污染
// ============================================================================

namespace
{
    constexpr float kMinDt = 1.0e-3f; // 最小有效时间步
}

PIDController::PIDController(float kp, float ki, float kd,
                             float integralLimit, float maxOutput)
    : m_kp(std::isfinite(kp) ? kp : 0.0f)
    , m_ki(std::isfinite(ki) ? ki : 0.0f)
    , m_kd(std::isfinite(kd) ? kd : 0.0f)
    , m_integralLimit((std::isfinite(integralLimit) && integralLimit > 0.0f) ? integralLimit : 0.0f)
    , m_maxOutput((std::isfinite(maxOutput) && maxOutput > 0.0f) ? maxOutput : 0.0f)
    , m_integral(0.0f)
    , m_prevError(0.0f)
    , m_dFiltered(0.0f)
    , m_lastOutput(0.0f)
    , m_first(true)
{
}

void PIDController::reset()
{
    m_integral   = 0.0f;
    m_prevError  = 0.0f;
    m_dFiltered  = 0.0f;
    m_lastOutput = 0.0f;
    m_first      = true;
}

float PIDController::compute(float error, float dt)
{
    // dt 防护: 非正 / NaN / Inf 一律钳制为最小步长, 防止除零与状态污染
    if (!(dt > 0.0f) || !std::isfinite(dt))
        dt = kMinDt;

    // error 防护: 无效误差 (上游 NaN/Inf) 时复位并输出 0,
    // 避免积分 / 微分 / 输出全部被污染
    if (!std::isfinite(error))
    {
        reset();
        return 0.0f;
    }

    // ---- 比例项 ----
    float p = m_kp * error;

    // ---- 微分项 (一阶低通滤波, 抑制噪声) ----
    float derivative = 0.0f;
    if (!m_first)
        derivative = (error - m_prevError) / dt;
    const float alpha = 0.2f; // 滤波系数, 越小越平滑
    m_dFiltered += alpha * (derivative - m_dFiltered);
    float d = m_kd * m_dFiltered;

    // ---- 积分项 (抗饱和: 仅当未饱和或误差有助于退出饱和时才累加) ----
    float tentativeIntegral = m_integral + error * dt;

    bool saturatingHigh = (m_lastOutput >= m_maxOutput)  && (error > 0.0f);
    bool saturatingLow  = (m_lastOutput <= -m_maxOutput) && (error < 0.0f);

    if (!saturatingHigh && !saturatingLow)
        m_integral = tentativeIntegral;

    // 积分限幅, 防止积分爆炸
    if (m_integral >  m_integralLimit) m_integral =  m_integralLimit;
    if (m_integral < -m_integralLimit) m_integral = -m_integralLimit;

    float i = m_ki * m_integral;

    // ---- 合成输出并限幅 ----
    float output = p + i + d;
    if (output >  m_maxOutput) output =  m_maxOutput;
    if (output < -m_maxOutput) output = -m_maxOutput;

    m_prevError  = error;
    m_lastOutput = output;
    m_first      = false;

    return output;
}

// ============================================================================
// 预测 / 提交 分离 (饱和感知抗积分饱和)
//
// 旧版 compute() 的抗饱和仅依据"PID 自身输出是否达 maxOutput";
// 但 AxisController 的真实轴速度 = 前馈 + PID 输出, 前馈也可能把轴推入饱和,
// 此时 PID 自身输出并未饱和 -> 积分照常累加 -> windup 累积 -> 激进参数下发散 (R11)。
// 分离后由 AxisController 在已知"合成速度是否饱和"的前提下, 通过 commit(integrate)
// 决定是否累加积分, 彻底关闭该盲区。
// ============================================================================

float PIDController::computeOutput(float error, float dt) const
{
    // dt 防护
    if (!(dt > 0.0f) || !std::isfinite(dt))
        dt = kMinDt;
    // 无效误差不贡献 (调用方 (AxisController) 已对 NaN/Inf 置零返回)
    if (!std::isfinite(error))
        return 0.0f;

    float p = m_kp * error;
    float d = m_kd * m_dFiltered;   // 微分项使用已滤波值 (由 commit 维护)
    float i = m_ki * m_integral;

    return p + i + d;
}

void PIDController::commit(float error, float dt, bool integrate)
{
    if (!(dt > 0.0f) || !std::isfinite(dt))
        dt = kMinDt;
    if (!std::isfinite(error))
        return; // 无效误差不更新状态 (上游已复位)

    // 微分项一阶低通 (更新滤波值供下一帧 computeOutput 使用)
    float derivative = 0.0f;
    if (!m_first)
        derivative = (error - m_prevError) / dt;
    const float alpha = 0.2f; // 滤波系数, 越小越平滑
    m_dFiltered += alpha * (derivative - m_dFiltered);

    if (integrate)
    {
        m_integral += error * dt;
        // 积分限幅 (绝对值上限), 防止积分爆炸
        if (m_integral >  m_integralLimit) m_integral =  m_integralLimit;
        if (m_integral < -m_integralLimit) m_integral = -m_integralLimit;
        // 附加抗饱和: 积分项单独贡献不得超越输出限幅,
        // 防止 Ki 较大时积分主导导致速度失控发散 (R11 根因)。
        if (m_ki > 1.0e-6f)
        {
            float iContrib = m_ki * m_integral;
            if (iContrib >  m_maxOutput) m_integral =  m_maxOutput / m_ki;
            if (iContrib < -m_maxOutput) m_integral = -m_maxOutput / m_ki;
        }
    }

    m_prevError = error;
    m_first     = false;
}
