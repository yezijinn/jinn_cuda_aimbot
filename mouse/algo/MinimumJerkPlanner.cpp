#include "MinimumJerkPlanner.h"
#include <cmath>

// ============================================================================
// MinimumJerkPlanner.cpp
//
// 稳定性设计 (本次审查加固):
//   - duration 判定改为 !(duration > 1e-3): 原 duration < 1e-3 对 NaN 恒为 false,
//     NaN 时长会逃过钳制, 使全部五次多项式系数化为 NaN
//   - 输入坐标防护: 任一参数非有限 (上游 NaN/Inf) 或超幅度 (第六轮: 巨大
//     有限值, 经 solveQuintic dp 项溢出 float) 时拒绝重规划 (保留旧轨迹),
//     避免 NaN/Inf 系数污染下游 PID / 鼠标位置
// ============================================================================

MinimumJerkPlanner::MinimumJerkPlanner()
    : m_targetX(0.0f), m_targetY(0.0f)
    , m_duration(0.0f)
    , m_planned(false)
{
    for (int i = 0; i < 6; ++i)
    {
        m_cx[i] = 0.0f;
        m_cy[i] = 0.0f;
    }
}

namespace
{
    // 最小有效时长: 防 NaN/负值/0 导致除零与系数爆炸
    constexpr float kMinDuration = 1.0e-3f;
    // 最大有效时长: 防超大 T 使 T5 = T^4*T 溢出 float (T > ~1.3e7 时溢出为 Inf),
    // 导致全部系数化为 NaN/Inf。正常调用时长 <= 0.6s, 此上限零影响,
    // 仅拦截外部误传的异常值。
    constexpr float kMaxDuration = 10.0f;
    // 输入幅度上限 (第六轮加固): 拦截"巨大但有限"的坐标/速度 (isfinite 仅挡
    // NaN/Inf)。solveQuintic 中 c3 ~ 10*dp/T3, dp=1e30 时经 1/T3 (T 最小 1e-3
    // 时达 1e9) 放大为 Inf, 且 m_planned 已置位后无法自愈 (后续 getPosition
    // 全部 Inf)。正常路径幅度分析: MouseController 内轨迹起点=鼠标位置
    // (<= 1e7, setMousePosition 防护), 终点=Kalman 预测点 (状态位置 <= 1e7 +
    // v*ahead + 0.5*a*ahead^2, 极端 ~1.1e7), 初速度=实际增量反推 (<= 3e4,
    // maxStep=30px/帧 约束)。1e8 上限覆盖全部正常路径且远低于 float 溢出
    // 安全边界 (~1e22), 仅拦截外部经 planner() 访问器直调的异常注入。
    constexpr float kMaxInput = 1.0e8f;

    inline bool validInput(float v)
    {
        return std::isfinite(v) && std::fabs(v) <= kMaxInput;
    }
}

// 解五次多项式系数 (边界条件见头文件)
void MinimumJerkPlanner::solveQuintic(float p0, float v0, float a0,
                                      float pT, float vT, float aT,
                                      float T, float c[6])
{
    c[0] = p0;
    c[1] = v0;
    c[2] = 0.5f * a0;

    float T2 = T * T;
    float T3 = T2 * T;
    float T4 = T3 * T;
    float T5 = T4 * T;
    float dp = pT - p0;

    c[3] = ( 20.0f * dp - (8.0f * vT + 12.0f * v0) * T - (3.0f * a0 - aT) * T2 ) / (2.0f * T3);
    c[4] = ( -30.0f * dp + (14.0f * vT + 16.0f * v0) * T + (3.0f * a0 - 2.0f * aT) * T2 ) / (2.0f * T4);
    c[5] = ( 12.0f * dp - (6.0f * vT + 6.0f * v0) * T - (a0 - aT) * T2 ) / (2.0f * T5);
}

void MinimumJerkPlanner::plan(float startX, float startY,
                              float targetX, float targetY,
                              float duration)
{
    planWithVelocity(startX, startY, 0.0f, 0.0f, targetX, targetY, duration);
}

void MinimumJerkPlanner::planWithVelocity(float startX, float startY,
                                          float startVx, float startVy,
                                          float targetX, float targetY,
                                          float duration)
{
    // duration 防护: NaN / 负值 / 0 / Inf 一律钳制为最小有效时长。
    // 注意不能写 duration < 1e-3 (NaN 比较恒为 false, 会逃逸), 须用 !(duration > 1e-3)。
    if (!(duration > kMinDuration))
        duration = kMinDuration;
    // 上限钳制: 防止超大 T 使 solveQuintic 内 T5 溢出 float 产生 Inf/NaN 系数
    if (duration > kMaxDuration)
        duration = kMaxDuration;

    // 输入坐标防护: 任一参数非有限 (上游 NaN/Inf) 或超幅度 (巨大有限异常值)
    // 时拒绝重规划, 保留旧轨迹不动。防止 NaN/Inf 系数污染下游 (m_planned
    // 置位后无自愈路径); 幅度检查拦截 solveQuintic dp 项溢出 float 的异常注入
    // (第五轮: isfinite; 第六轮: 幅度上限)。正常路径 (MouseController 内部
    // 已做 isfinite + 1e7 过滤, 预测点极端 < 1.1e7) 不受影响。
    if (!validInput(startX)  || !validInput(startY)  ||
        !validInput(startVx) || !validInput(startVy) ||
        !validInput(targetX) || !validInput(targetY))
        return;

    m_duration = duration;
    m_targetX  = targetX;
    m_targetY  = targetY;

    // 起点加速度取 0, 终点速度/加速度取 0
    solveQuintic(startX, startVx, 0.0f, targetX, 0.0f, 0.0f, duration, m_cx);
    solveQuintic(startY, startVy, 0.0f, targetY, 0.0f, 0.0f, duration, m_cy);

    m_planned = true;
}

void MinimumJerkPlanner::getPosition(float t, float& outX, float& outY) const
{
    if (!m_planned)
    {
        outX = m_cx[0];
        outY = m_cy[0];
        return;
    }
    // t 防护 (第五轮加固): 原 t < 0 判定对 NaN 恒为 false, NaN 会逃逸钳制
    // 进入 Horner 求值产出 NaN 参考点。改用 !(t >= 0): NaN/负值一律按 0。
    if (!(t >= 0.0f)) t = 0.0f;
    if (t > m_duration) t = m_duration;

    // Horner 法求值, 减少乘法次数
    outX = ((((m_cx[5] * t + m_cx[4]) * t + m_cx[3]) * t + m_cx[2]) * t + m_cx[1]) * t + m_cx[0];
    outY = ((((m_cy[5] * t + m_cy[4]) * t + m_cy[3]) * t + m_cy[2]) * t + m_cy[1]) * t + m_cy[0];
}

void MinimumJerkPlanner::getVelocity(float t, float& outVx, float& outVy) const
{
    if (!m_planned)
    {
        outVx = 0.0f;
        outVy = 0.0f;
        return;
    }
    if (!(t >= 0.0f)) t = 0.0f;
    if (t > m_duration) t = m_duration;

    outVx = (((5.0f * m_cx[5] * t + 4.0f * m_cx[4]) * t + 3.0f * m_cx[3]) * t + 2.0f * m_cx[2]) * t + m_cx[1];
    outVy = (((5.0f * m_cy[5] * t + 4.0f * m_cy[4]) * t + 3.0f * m_cy[3]) * t + 2.0f * m_cy[2]) * t + m_cy[1];
}

void MinimumJerkPlanner::getAcceleration(float t, float& outAx, float& outAy) const
{
    if (!m_planned)
    {
        outAx = 0.0f;
        outAy = 0.0f;
        return;
    }
    if (!(t >= 0.0f)) t = 0.0f;
    if (t > m_duration) t = m_duration;

    outAx = ((20.0f * m_cx[5] * t + 12.0f * m_cx[4]) * t + 6.0f * m_cx[3]) * t + 2.0f * m_cx[2];
    outAy = ((20.0f * m_cy[5] * t + 12.0f * m_cy[4]) * t + 6.0f * m_cy[3]) * t + 2.0f * m_cy[2];
}
