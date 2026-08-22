#pragma once

#include "KalmanTracker.h"
#include "MinimumJerkPlanner.h"
#include "AxisController.h"
#include "Humanizer.h"

// ============================================================================
// MouseController.h
// 实时鼠标运动控制核心
//
// 数据流 (每个控制周期调用一次 update):
//
//   YOLO 目标点 (targetX, targetY)
//        |
//        v
//   [1] KalmanTracker.update       -- 滤波 + 估计目标状态 [x,y,vx,vy]
//        |
//        v
//   [2] KalmanTracker.predictAhead -- 预测未来落点 (predictedX, predictedY)
//        |
//        v
//   [3] MinimumJerkPlanner.plan    -- 目标变化超阈值时, 重新生成平滑轨迹
//        |
//        v
//   [4] MinimumJerkPlanner.getPosition -- 取当前时刻轨迹参考点
//        |
//        v
//   [5] AxisController.compute     -- 死区 + PID + 单帧限幅 (X, Y 各一路)
//        |
//        v
//   [6] moveDelta = velocity * dt  -- 输出本周期鼠标增量 (moveX, moveY)
//
// 说明:
//   - 类内部维护一份"当前鼠标位置"用于仿真闭环; 接入真实系统时,
//     应在执行 SendInput 后调用 setMousePosition() 用系统真实坐标回读同步。
//   - 线程模型: 本类及其子模块 (Kalman/MJ/PID/Humanizer) 均非线程安全。
//     所有 update() / setXxx() 调用必须由同一线程串行执行;
//     若 YOLO 推理线程与鼠标控制线程并发, 需外部加锁串行化或仅由控制线程
//     读取最新检测结果 (原子快照), 否则存在数据竞争。
//   - 延迟设计: 全部使用栈上固定数组, 无动态内存分配、无锁、无 IO,
//     单帧计算量 < 1us, 满足 100Hz 实时控制。
// ============================================================================

class MouseController
{
public:
    MouseController();

    // 核心更新接口 (每 10ms 调用一次)
    //   targetX, targetY : YOLO 检测到的目标坐标 (含噪声)
    //   dt               : 距上次调用的时间间隔 (秒)
    void update(float targetX, float targetY, float dt);

    // ---- 结果读取 ----
    void getMoveDelta(float& moveX, float& moveY) const;       // 本周期移动增量
    void getPredictedTarget(float& x, float& y) const;         // Kalman 预测落点
    void getTrajectoryRef(float& x, float& y) const;           // 轨迹参考点
    void getMousePosition(float& x, float& y) const;           // 当前鼠标位置
    void getOutputVelocity(float& vx, float& vy) const;        // PID 输出速度

    // ---- 参数设置 ----
    void setMousePosition(float x, float y);      // 同步/初始化鼠标位置
    void setPredictAheadTime(float seconds);      // 固定前瞻时长 (自适应关闭时生效)
    // 自适应前瞻: ahead = clamp(dist/pxPerSec, minAhead, maxAhead)
    // 远距离大前瞻(利于匀速拦截), 近距离/变向小前瞻(避免外推偏差)
    void enableAdaptiveAhead(bool enable, float minAhead = 0.05f, float maxAhead = 0.15f);
    void setRetargetThreshold(float pixels);      // 目标偏移多少像素触发重规划
    void setMaxStepPerFrame(float pixels);        // 全局: 单帧最大移动像素量
    // ---- 人手模拟 (可选): 拟人化轨迹扰动, 避免轨迹像机器 ----
    //   快速预设: 0=关闭, 1=轻度, 2=中度, 3=高度 (内部映射到 HumanizerSettings)
    void setHumanization(int level);
    //   自定义参数 (全部为 0 = 关闭)
    void setHumanizerSettings(const HumanizerSettings& s) { m_humanizer.setSettings(s); }
    Humanizer& humanizer() { return m_humanizer; }
    void setFixedMoveDuration(float seconds);     // 固定轨迹时长 (关闭自适应)
    // 自适应轨迹时长: duration = clamp(dist / pxPerSec, minDur, maxDur)
    void enableAdaptiveDuration(bool enable,
                                float minDur = 0.15f,
                                float maxDur = 0.60f,
                                float pxPerSec = 2500.0f);

    // 重置内部滤波/轨迹/速度状态 (目标丢失或切换时调用)
    void reset();

    // ---- 分轴调参 (X / Y 各一套 6 参数面板) ----
    void setAxisTuningX(const AxisTuning& t) { m_axisX.setTuning(t); }
    void setAxisTuningY(const AxisTuning& t) { m_axisY.setTuning(t); }

    // ---- 子模块访问 (便于外部整定) ----
    KalmanTracker&      tracker()  { return m_tracker; }
    AxisController&     axisX()    { return m_axisX; }
    AxisController&     axisY()    { return m_axisY; }
    MinimumJerkPlanner& planner()  { return m_planner; }

private:
    float computeDuration(float distX, float distY) const; // 自适应时长

private:
    KalmanTracker      m_tracker;   // 目标跟踪
    MinimumJerkPlanner m_planner;   // 轨迹规划
    AxisController     m_axisX;     // X 轴控制 (死区+PID+限幅)
    AxisController     m_axisY;     // Y 轴控制 (死区+PID+限幅)
    Humanizer          m_humanizer; // 人手模拟 (可选扰动)

    float m_mouseX, m_mouseY;       // 当前鼠标位置
    float m_predictedX, m_predictedY; // 预测目标
    float m_trajRefX, m_trajRefY;   // 轨迹参考点
    float m_moveX, m_moveY;         // 本周期增量
    float m_lastVelX, m_lastVelY;   // PID 输出速度

    // 自运动补偿状态: 上一次 update 结束时的鼠标位置。
    //   本帧入口处 (m_mouseX - m_prevMouseX) 即"外部回读修正量":
    //     - FPS 锁定准星: 调用方每帧把位置写回屏幕中心, 修正量 = -上帧位移,
    //       说明整帧位移都转化成了镜头转动 -> 目标屏幕位置反向平移同样多;
    //     - 桌面光标模式: 光标确实移动了, 修正量 ≈ 0 -> 镜头未动, 无需补偿。
    //   该差值直接就是要喂给 KalmanTracker::applyEgoShift 的目标屏幕位移,
    //   无需模式开关, 两种接入方式自动自洽。
    float m_prevMouseX, m_prevMouseY;
    bool  m_hasPrevMouse;

    float m_predictAheadTime;       // 固定前瞻时长 (秒)
    bool  m_adaptiveAhead;          // 是否自适应前瞻
    float m_minAhead, m_maxAhead;   // 自适应前瞻范围 (秒)
    float m_retargetThreshold;      // 重规划阈值 (像素)
    float m_elapsed;                // 当前轨迹已进行时间 (秒)

    bool  m_adaptiveDuration;       // 是否自适应时长
    float m_fixedDuration;          // 固定时长
    float m_minDuration, m_maxDuration; // 自适应时长范围
    float m_pxPerSec;               // 自适应参考速度 (像素/秒)
    float m_maxStepPerFrame;        // 全局: 单帧最大移动像素量 (限制合成位移模长)

    // 预测/轨迹/修正为模块固有行为, 始终启用 (移植时移除独立阶段开关)
};
