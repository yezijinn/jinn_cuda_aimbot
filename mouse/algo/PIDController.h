#pragma once

#include <cmath>

// ============================================================================
// PIDController.h
// 增量安全型 PID 速度控制器
//
// 控制结构:
//     Position Error  -->  PID  -->  Velocity Output
//
// 特性:
//   - 积分限幅 (integralLimit), 防止积分爆炸
//   - 抗积分饱和 (anti-windup): 输出饱和且误差继续同向增大时冻结积分
//   - 输出限幅 (maxOutput), 限制最大速度
//   - 微分项带一阶低通滤波, 抑制微分噪声放大
// ============================================================================

class PIDController
{
public:
    // kp, ki, kd        : 比例 / 积分 / 微分增益
    // integralLimit     : 积分项绝对值上限
    // maxOutput         : 输出(速度)绝对值上限
    explicit PIDController(float kp = 3.0f,
                           float ki = 0.0f,
                           float kd = 0.05f,
                           float integralLimit = 500.0f,
                           float maxOutput = 4000.0f);

    // 计算控制输出
    //   error : 当前位置误差 (期望 - 实际)
    //   dt    : 时间步长
    // 返回: 速度输出
    float compute(float error, float dt);

    // 预测控制输出 (使用当前积分状态, 不修改内部状态)
    //   供调用方 (AxisController) 先做"合成速度是否饱和"判定,
    //   再决定本帧是否累加积分 (饱和感知抗 windup)。
    //   返回 P + Ki*integral + Kd*filteredDerivative (未做输出限幅)。
    float computeOutput(float error, float dt) const;

    // 提交本帧积分 / 微分状态
    //   integrate = false 时冻结积分 (饱和感知抗积分饱和, 防 windup 发散)。
    //   积分项单独贡献被钳制在 ±maxOutput 内, 防止 Ki 较大时积分主导失控。
    void commit(float error, float dt, bool integrate);

    // 清空内部状态 (积分 / 上一次误差)
    void reset();

    // 在线整定 (本轮审查加固: 非法输入防污染)
    //   kp/ki/kd : NaN/Inf 按 0 处理 (负增益允许, 保留特殊控制场景)
    //   limit    : 非正 / NaN / Inf 按 0 处理 (限幅无负值语义)
    void setGains(float kp, float ki, float kd)
    {
        m_kp = std::isfinite(kp) ? kp : 0.0f;
        m_ki = std::isfinite(ki) ? ki : 0.0f;
        m_kd = std::isfinite(kd) ? kd : 0.0f;
    }
    void setIntegralLimit(float limit)
    {
        m_integralLimit = (std::isfinite(limit) && limit > 0.0f) ? limit : 0.0f;
    }
    void setMaxOutput(float maxOut)
    {
        m_maxOutput = (std::isfinite(maxOut) && maxOut > 0.0f) ? maxOut : 0.0f;
    }

    float getLastOutput() const { return m_lastOutput; }
    float getIntegral() const { return m_integral; }

private:
    float m_kp, m_ki, m_kd;      // PID 增益
    float m_integralLimit;       // 积分限幅
    float m_maxOutput;           // 输出限幅

    float m_integral;            // 积分累加器
    float m_prevError;           // 上一次误差
    float m_dFiltered;           // 滤波后的微分项
    float m_lastOutput;          // 上一次输出 (用于 anti-windup)
    bool  m_first;               // 首次计算标志
};
