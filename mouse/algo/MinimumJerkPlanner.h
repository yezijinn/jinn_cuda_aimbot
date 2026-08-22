#pragma once

// ============================================================================
// MinimumJerkPlanner.h
// 通用五次多项式 (Minimum Jerk) 轨迹规划器
//
// 与"固定零边界"版本的区别:
//   经典实现 s(t)=10t^3-15t^4+6t^5 要求起点速度/加速度都为 0。
//   在"跟踪移动目标"场景下需要连续重规划, 若每帧都强制从零速度起步,
//   鼠标将永远无法积累速度而原地卡死 (本项目实测确认的严重 bug)。
//
//   本实现使用通用五次多项式, 允许指定"起点速度", 终点速度/加速度固定为 0。
//   重规划时以当前鼠标速度作为初速度, 保证轨迹平滑衔接、可持续加速跟踪。
//   当起点速度/加速度也为 0 时, 自动退化为经典 s(t) 曲线。
//
// 一维五次多项式:
//   p(t) = c0 + c1 t + c2 t^2 + c3 t^3 + c4 t^4 + c5 t^5
// ============================================================================

class MinimumJerkPlanner
{
public:
    MinimumJerkPlanner();

    // 点对点规划 (起点/终点速度、加速度均为 0) —— 向后兼容
    void plan(float startX, float startY,
              float targetX, float targetY,
              float duration);

    // 带初速度规划 (起点速度 = startVx/startVy, 起点加速度 = 0; 终点速度/加速度 = 0)
    // 用于跟踪移动目标时的连续重规划, 保证速度衔接、不急停
    void planWithVelocity(float startX, float startY,
                          float startVx, float startVy,
                          float targetX, float targetY,
                          float duration);

    // 取 t 秒时刻的参考位置 / 速度 / 加速度 (t 自动钳制到 [0, duration])
    void getPosition(float t, float& outX, float& outY) const;
    void getVelocity(float t, float& outVx, float& outVy) const;
    void getAcceleration(float t, float& outAx, float& outAy) const;

    float getDuration() const { return m_duration; }
    bool  isPlanned()  const { return m_planned; }
    void  reset() { m_planned = false; }

    float getTargetX() const { return m_targetX; }
    float getTargetY() const { return m_targetY; }

private:
    // 解一维五次多项式系数
    //   边界: p(0)=p0, p'(0)=v0, p''(0)=a0 ; p(T)=pT, p'(T)=vT, p''(T)=aT
    static void solveQuintic(float p0, float v0, float a0,
                             float pT, float vT, float aT,
                             float T, float c[6]);

private:
    float m_cx[6];              // X 轴五次多项式系数
    float m_cy[6];              // Y 轴五次多项式系数
    float m_targetX, m_targetY; // 终点
    float m_duration;           // 时长 T
    bool  m_planned;            // 是否已有有效规划
};
