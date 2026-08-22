#include "MouseController.h"
#include <cmath>

// ============================================================================
// MouseController.cpp
//
// 本次审查加固:
//   - dt 上下限防护提取为命名常量, 语义清晰
//   - update() 对 YOLO 目标坐标做 isfinite 防护: 检测无效 (丢帧/NaN/Inf) 时
//     改用 tracker.predictOnly(dt) 保持目标运动外推, 控制链其余部分照常工作,
//     目标跟踪平滑过渡, 不会跳飞或冻结
//   - enableAdaptiveAhead / enableAdaptiveDuration 参数归一化:
//     min>max 自动交换, 非正/NaN/Inf 参数钳制, 避免运行时出现奇异区间
//   - (第四轮) 未初始化 + 无效观测: 输出零增量保持静止, 防止向 (0,0) 漂移
//   - (第四轮) 全部 setter 拒绝 Inf/NaN: 防止前瞻/限幅/时长等被 Inf 污染
//   - (第五轮) 目标坐标/鼠标位置加幅度上限 (1e7): 拦截"巨大但有限"异常值,
//     防止轨迹规划五次系数溢出 float 污染全链 (isfinite 仅挡 NaN/Inf)
//   - (第五轮) [5.6] 增量有效性兜底: 全链最后防线, 保证鼠标位置恒有限
//   - (第五轮) 移除 [5] 冗余 lastVel 赋值 (被末尾 m_moveX/dt 覆盖的死代码)
//   - (第六轮) setPredictAheadTime 补幅度上限 (> 10s 拒绝): 巨大有限前瞻
//     虽被 KalmanTracker::predictAhead 内部钳制不溢出, 但产生外推语义失真,
//     与 Inf 同样视为错误输入
//   - (第七轮) setFixedMoveDuration 补幅度上限 (> 10s 拒绝, 与前瞻/planner
//     对齐); 重规划被拒时状态一致性 (成功才重置 elapsed, 无轨迹被拒则静止
//     返回, 防参考点回落 (0,0) 漂移变体)
//   - (第八轮) setter 幅度上限盲区系统性补齐:
//     * enableAdaptiveAhead / enableAdaptiveDuration 区间参数补 >10s 拒绝
//       (巨大有限区间值使 ahead/duration 恒钳 10s, 语义失真);
//     * setRetargetThreshold 非法值由"设 0"改为"拒绝保留旧值" (阈值 0 使
//       每帧重规划, 跟踪退化为蠕动; 巨大有限值平方溢出 float 使重瞄准失效);
//     * setMaxStepPerFrame 补 1e5 px/帧 上限 (巨大有限值使限幅判定恒 false,
//       限幅静默失效)
// ============================================================================

namespace
{
    constexpr float kMinDt = 1.0e-3f;  // 单帧 dt 下限: 防除零
    constexpr float kMaxDt = 0.10f;    // 单帧 dt 上限: 防"暂停/卡顿后恢复"的巨帧冲击
                                       // (100Hz 控制周期容忍 10 倍抖动, 超出按 100ms 处理)
    // 目标坐标幅度上限 (第五轮加固): isfinite 仅挡 NaN/Inf, 不挡"巨大但有限"的
    // 异常检测值。超大坐标会使轨迹规划 solveQuintic 的 dp 项溢出 float
    // (c3 ~ 1e10*dp, dp=1e30 -> Inf), 污染全链无法自愈。
    // 真实 YOLO 坐标 < 1e5 px, 1e7 上限零影响, 仅拦截异常注入。
    constexpr float kMaxTargetCoord = 1.0e7f;
}

MouseController::MouseController()
    : m_mouseX(0.0f), m_mouseY(0.0f)
    , m_predictedX(0.0f), m_predictedY(0.0f)
    , m_trajRefX(0.0f), m_trajRefY(0.0f)
    , m_moveX(0.0f), m_moveY(0.0f)
    , m_lastVelX(0.0f), m_lastVelY(0.0f)
    , m_prevMouseX(0.0f), m_prevMouseY(0.0f)
    , m_hasPrevMouse(false)
    , m_predictAheadTime(0.03f)    // 固定前瞻 30ms (自适应开启时不用)
    , m_adaptiveAhead(true)        // 默认开启自适应前瞻
    , m_minAhead(0.05f)
    , m_maxAhead(0.15f)
    , m_retargetThreshold(50.0f)   // 目标"突变"阈值(像素); 匀速移动靠轨迹走完驱动, 不走此分支
    , m_elapsed(0.0f)
    , m_adaptiveDuration(true)
    , m_fixedDuration(0.35f)
    , m_minDuration(0.15f)
    , m_maxDuration(0.60f)
    , m_pxPerSec(2500.0f)
    , m_maxStepPerFrame(30.0f)
{
}

void MouseController::setMousePosition(float x, float y)
{
    // 防护: 拒绝 NaN/Inf/超幅度 (|v| > 1e7) 写入, 否则鼠标位置被污染后
    //       全部误差恒为 NaN, PID 复位保护虽能挡 compute, 但轨迹/预测输出
    //       仍会全部失效; 超大幅度还会经轨迹规划 dp 项溢出 float。
    //       此防护保证 m_mouseX/Y 自写入起恒有限。
    if (!std::isfinite(x) || !std::isfinite(y) ||
        std::fabs(x) > kMaxTargetCoord || std::fabs(y) > kMaxTargetCoord)
        return;
    m_mouseX = x;
    m_mouseY = y;
}

void MouseController::setPredictAheadTime(float seconds)
{
    // 防护: Inf/NaN 前瞻时长会使 predictAhead 外推溢出
    // (p + v*Inf + 0.5*a*Inf^2 -> Inf, 当 v/a 非零), 预测点被污染后
    // 重规划轨迹系数、PID 误差、鼠标位置全链失效且无法自愈。
    // (第六轮) 补充幅度上限: "巨大但有限"的前瞻 (如 1e20) 虽经 predictAhead
    // 内部 kMaxAhead 钳制不会溢出, 但会产生 10s 量级外推的语义失真,
    // 与 Inf 同样属于错误输入, 统一拒绝保留旧值。
    // 非法输入直接忽略, 保留旧值 (与 Kalman/PID setter 防护策略一致)。
    if (!std::isfinite(seconds) || seconds > 10.0f)
        return;
    m_predictAheadTime = (seconds > 0.0f) ? seconds : 0.0f;
    m_adaptiveAhead = false; // 手动设固定前瞻 -> 关闭自适应
}

void MouseController::enableAdaptiveAhead(bool enable, float minAhead, float maxAhead)
{
    m_adaptiveAhead = enable;
    // 参数归一化: 下限>上限自动交换。
    // (第四轮) isfinite 校验: Inf 上限会绕过 ahead 钳制 (语义失真)。
    // (第八轮) 幅度上限: "巨大但有限"的区间值 (如 1e20) 会存入 m_minAhead/m_maxAhead,
    //          使自适应 ahead = clamp(dist/pxPerSec, 1e20, 1e20) 恒被钳到
    //          KalmanTracker 内部 kMaxAhead=10s, 前瞻退化为恒定 10s (语义失真,
    //          与 setPredictAheadTime >10s 拒绝的策略不一致)。故超 10s 的输入
    //          一律拒绝、保留旧区间 (与其它 setter "非法输入保留旧值"策略对齐)。
    if (minAhead > maxAhead)
    {
        const float t = minAhead;
        minAhead = maxAhead;
        maxAhead = t;
    }
    if (!std::isfinite(minAhead) || !std::isfinite(maxAhead) ||
        minAhead < 0.0f || maxAhead > 10.0f)
        return; // 非法区间: 保留旧值, 开关状态已更新
    m_minAhead = minAhead;
    m_maxAhead = maxAhead;
}

void MouseController::setRetargetThreshold(float pixels)
{
    // (第八轮) 防护升级: 原实现非法值 (NaN/Inf/非正) 一律设 0, 存在两个缺陷:
    //   a) 阈值 0 使辅助重规划判定 drift2 > 0 恒 true (任何微小漂移都触发),
    //      退化为"每帧重规划"——轨迹起点恒为当前位置、参考点≈起点,
    //      PID 误差≈0, 鼠标跟踪退化为蠕动 (头文件注释所述卡死模式的诱因之一);
    //   b) "巨大但有限"的阈值 (如 1e20) 平方溢出 float (1e20^2=Inf),
    //      使 drift2 > Inf 恒 false, 辅助重瞄准静默失效。
    // 修复: 非有限 / 非正 / 超 1e6 px 一律拒绝保留旧值 (与其它 setter 对齐)。
    // 正常范围 (1~1000px) 零影响。
    if (!std::isfinite(pixels) || pixels <= 0.0f || pixels > 1.0e6f)
        return;
    m_retargetThreshold = pixels;
}

void MouseController::setMaxStepPerFrame(float pixels)
{
    // 防护: Inf 会绕过限幅 (mag > Inf 恒 false), 静默关闭全局限幅; 拒绝
    // (第八轮) 幅度上限: "巨大但有限"的值 (如 1e38, float 上限内) 使限幅
    //          判定 mag > 1e38 恒 false, 限幅同样静默失效。正常单帧步长
    //          <= 数百 px, 1e5 px/帧 (100Hz 下 1e7 px/s) 已远超任何真实
    //          场景, 作为上限零误伤, 超限按异常输入拒绝保留旧值。
    if (std::isfinite(pixels) && pixels > 0.0f && pixels <= 1.0e5f)
        m_maxStepPerFrame = pixels;
}

void MouseController::setHumanization(int level)
{
    HumanizerSettings s;
    switch (level)
    {
        case 1: // 轻度: 细微拟人, 几乎不影响精度
            s.microJitter    = 0.3f;
            s.wobble         = 0.4f;
            s.overshoot      = 2.0f;
            s.speedVariation = 0.03f;
            break;
        case 2: // 中度: 明显人感, 推荐默认
            s.microJitter    = 0.6f;
            s.wobble         = 0.8f;
            s.overshoot      = 4.0f;
            s.speedVariation = 0.06f;
            break;
        case 3: // 高度: 强烈拟人, 精度让位于自然手感
            s.microJitter    = 1.2f;
            s.wobble         = 1.6f;
            s.overshoot      = 7.0f;
            s.speedVariation = 0.10f;
            break;
        default: // 0 或非法值 = 关闭
            break;
    }
    m_humanizer.setSettings(s);
}

void MouseController::setFixedMoveDuration(float seconds)
{
    // 防护: Inf 时长会经 computeDuration 直接传入 planner (被上限钳到 10s,
    // 产生"极慢轨迹"语义失真); NaN 比较恒 false 也会落回下限。统一拒绝。
    // (第七轮) 补充幅度上限: 与 setPredictAheadTime(10s)/planner kMaxDuration(10s)
    // 对齐。巨大有限时长 (如 1e20) 虽经 planner 内部钳制不产生安全危害,
    // 但产生 10s 级语义失真, 与 Inf 同样视为错误输入, 拒绝保留旧值。
    if (!std::isfinite(seconds) || seconds > 10.0f)
        return;
    m_fixedDuration = (seconds > 1.0e-3f) ? seconds : 1.0e-3f;
    m_adaptiveDuration = false;
}

void MouseController::enableAdaptiveDuration(bool enable,
                                             float minDur, float maxDur,
                                             float pxPerSec)
{
    m_adaptiveDuration = enable;
    // 参数归一化: 下限>上限自动交换; 非正/NaN/Inf 钳制, 保证区间有效。
    // (第四轮) 上限与 pxPerSec 加 isfinite 校验。
    // (第八轮) 幅度上限: "巨大但有限"的区间值 (如 1e20) 使自适应时长
    //          clamp(dist/pxPerSec, 1e20, 1e20) 恒被 planner kMaxDuration=10s
    //          钳制, 轨迹时长退化为恒定 10s (语义失真, 与 setFixedMoveDuration
    //          >10s 拒绝的策略不一致)。超 10s 的区间输入一律拒绝、保留旧值。
    if (minDur > maxDur)
    {
        const float t = minDur;
        minDur = maxDur;
        maxDur = t;
    }
    if (!std::isfinite(minDur) || !std::isfinite(maxDur) ||
        minDur <= 0.0f || maxDur > 10.0f)
        return; // 非法区间: 保留旧值, 开关状态已更新
    m_minDuration = minDur;
    m_maxDuration = maxDur;
    // (第九轮) pxPerSec 补幅度上限 (<= 1e6 px/s): 原实现仅查 isfinite 与 >1,
    // "巨大但有限"的参考速度 (如 1e30) 使 dur = dist/1e30 ≈ 0, 自适应时长
    // 恒被钳到 minDuration, 退化为固定最短时长 (语义失真, 与其它 setter
    // "异常输入拒绝保留旧值"策略不一致)。正常参考速度 2500 量级, 1e6
    // 上限零误伤 (100Hz 下 1e6 px/s 已远超任何真实场景)。
    if (std::isfinite(pxPerSec) && pxPerSec > 1.0f && pxPerSec <= 1.0e6f)
        m_pxPerSec = pxPerSec;
}

float MouseController::computeDuration(float distX, float distY) const
{
    if (!m_adaptiveDuration)
        return m_fixedDuration;

    float dist = std::sqrt(distX * distX + distY * distY);
    float dur = dist / m_pxPerSec;   // 期望匀速参考时间
    if (dur < m_minDuration) dur = m_minDuration;
    if (dur > m_maxDuration) dur = m_maxDuration;
    return dur;
}

void MouseController::reset()
{
    // KalmanTracker 无显式 reset(), 重建默认实例即回到"未初始化"状态
    m_tracker = KalmanTracker();
    m_planner.reset();
    m_humanizer.reset();
    m_elapsed = 0.0f;
    m_moveX = 0.0f;
    m_moveY = 0.0f;
    m_lastVelX = 0.0f;
    m_lastVelY = 0.0f;
    m_predictedX = 0.0f;
    m_predictedY = 0.0f;
    m_trajRefX = 0.0f;
    m_trajRefY = 0.0f;
    m_prevMouseX = 0.0f;
    m_prevMouseY = 0.0f;
    m_hasPrevMouse = false;   // 自运动补偿: 下一帧作为首帧, 不做补偿
}

void MouseController::update(float targetX, float targetY, float dt)
{
    // dt 健壮性: 下限防除零; 上限防"暂停/卡顿/窗口切换后恢复"时单帧 dt 过大,
    //           导致 Kalman 外推跳飞、MJ 轨迹瞬时完成、PID 输出爆炸。
    if (!(dt > 0.0f) || !std::isfinite(dt)) dt = kMinDt;
    if (dt > kMaxDt) dt = kMaxDt;

    // ---- [0] 目标有效性判定 + 滤波器未初始化防护 (第四轮修复) ----
    // (第五轮) 有效性判定补充幅度上限: |坐标| > 1e7 的"巨大但有限"异常
    // 检测值视为无效观测 (退化为仅外推), 防止轨迹规划系数溢出 float。
    const bool validObs = std::isfinite(targetX) && std::isfinite(targetY)
                       && std::fabs(targetX) <= kMaxTargetCoord
                       && std::fabs(targetY) <= kMaxTargetCoord;
    // 尚无任何有效目标信息 (YOLO 启动预热期 / 首帧即丢帧) 时滤波器未初始化,
    // 其状态恒为 (0,0,0,0,0,0), predictAhead 输出的"预测落点"恒为 (0,0)。
    // 若照常走重规划, 轨迹会指向屏幕原点, 鼠标从当前位置向 (0,0) 漂移 --
    // 这是真实危险行为 (目标可能在任何位置, 鼠标却冲向原点)。
    // 修复: 直接输出零增量保持静止, 等待首个有效观测进入控制环。
    if (!validObs && !m_tracker.isInitialized())
    {
        m_moveX    = 0.0f;
        m_moveY    = 0.0f;
        m_lastVelX = 0.0f;
        m_lastVelY = 0.0f;
        return;
    }

    // ---- [0.5] 自运动补偿 (ego-motion compensation, 修复 R1 极限环 / R11 发散) ----
    // 根因: FPS 中准星锁在屏幕中心, 鼠标输出转动镜头, 静止目标在屏幕上会反向
    //       移动。滤波器因此估计到的是"目标 - 准星"的相对速度; 前瞻外推用它
    //       等于自我引用 —— 准星冲得越快, 预测落点被甩到目标后方越远
    //       (实测残距 75px 时预测点已在目标后方 90px), 控制器误判"已过靶"
    //       而提前停摆并反向, 形成狩猎振荡; 高 tracking/maxSpeed 下振幅发散。
    // 修复: 把"镜头运动引起的目标屏幕位移"先平移进滤波状态, 使新息只含目标
    //       自身运动 -> 速度估计收敛为目标真实速度, 前瞻恢复正确语义
    //       (静止目标前瞻量归零; 移动目标仍得到正确提前量, 跟踪不受影响)。
    // 位移量取"外部回读修正量" (m_mouseX - 上帧结束位置), 见头文件成员注释:
    // 锁定准星模式自动得到 -上帧位移, 桌面光标模式自动得到 0, 无需模式开关。
    if (m_hasPrevMouse)
        m_tracker.applyEgoShift(m_mouseX - m_prevMouseX, m_mouseY - m_prevMouseY);
    // 先记录"移动前"位置: 下方各提前返回分支不推进鼠标, 此值即为帧末位置
    m_prevMouseX   = m_mouseX;
    m_prevMouseY   = m_mouseY;
    m_hasPrevMouse = true;

    // ---- [1] Kalman 滤波更新目标状态 ----
    // 输入防护: YOLO 检测失败 (NaN/Inf/丢帧) 时不进行观测校正,
    //           仅按运动模型外推, 保持目标状态连续, 不污染滤波。
    if (validObs)
        m_tracker.update(targetX, targetY, dt);
    else
        m_tracker.predictOnly(dt);

    // ---- [2] 预测未来落点 (前瞻瞄准) ----
    float ahead = m_predictAheadTime;
    if (m_adaptiveAhead)
    {
        // 用 Kalman 滤波后的目标位置估计到达时间, 自适应前瞻时长:
        // 距离远 -> 大前瞻(利于匀速拦截); 距离近/在变向 -> 小前瞻(避免外推偏差)
        float kx, ky, kvx, kvy;
        m_tracker.getState(kx, ky, kvx, kvy);
        float ddx = kx - m_mouseX, ddy = ky - m_mouseY;
        ahead = std::sqrt(ddx * ddx + ddy * ddy) / m_pxPerSec;
        if (ahead < m_minAhead) ahead = m_minAhead;
        if (ahead > m_maxAhead) ahead = m_maxAhead;
    }
    // 提前预测身位为模块固有行为, 始终执行前瞻预测
    m_tracker.predictAhead(ahead, m_predictedX, m_predictedY);

    // ---- [3] 判断是否需要重新规划轨迹 ----
    // 主驱动: 当前轨迹走完 -> 接续下一段。这是持续跟踪移动目标的关键:
    //         让 elapsed 持续累加、参考点沿轨迹前进, PID 才有非零误差可追。
    //         若每帧重规划, 轨迹起点永远是当前位置、参考点≈当前位置,
    //         PID 误差≈0, 鼠标将原地卡死。
    bool needReplan = (!m_planner.isPlanned()
                   || (m_elapsed >= m_planner.getDuration()));

    // 性能优化: 轨迹走完但已到达目标附近且低速 -> 不必反复重规划,
    //           避免静止/锁定后每帧空转解多项式系数浪费 CPU。
    //           (移动目标走完时距预测点仍远, 不会被误判, 仍正常接续下一段)
    if (needReplan && m_planner.isPlanned())
    {
        float rdx = m_predictedX - m_mouseX, rdy = m_predictedY - m_mouseY;
        float speed2 = m_lastVelX * m_lastVelX + m_lastVelY * m_lastVelY;
        if (rdx * rdx + rdy * rdy < 9.0f        // 距预测点 < ~3px
            && speed2 < 25.0f)                   // 速度 < ~5px/s
            needReplan = false;
    }

    // 辅驱动: 预测点显著跳变(换目标/大机动) -> 立即重瞄准。
    //         阈值取较大值, 使"目标匀速移动"不被误判为跳变。
    if (!needReplan)
    {
        float dx = m_predictedX - m_planner.getTargetX();
        float dy = m_predictedY - m_planner.getTargetY();
        float drift2 = dx * dx + dy * dy;                  // 用平方比较, 省一次 sqrt
        if (drift2 > m_retargetThreshold * m_retargetThreshold)
            needReplan = true;
    }

    if (needReplan)
    {
        float dur = computeDuration(m_predictedX - m_mouseX,
                                    m_predictedY - m_mouseY);
        // 以"当前鼠标速度"作为轨迹初速度, 保证重规划时速度连续,
        // 避免对移动目标每帧从零起步导致卡死
        const bool hadPlan = m_planner.isPlanned();
        m_planner.planWithVelocity(m_mouseX, m_mouseY,
                                   m_lastVelX, m_lastVelY,
                                   m_predictedX, m_predictedY, dur);
        // (第七轮加固) 规划被拒时的状态一致性:
        //   planWithVelocity 仅在输入超幅度/非有限时拒绝 (保留旧轨迹不写 target)。
        //   旧实现无条件重置 m_elapsed, 存在两个缺陷:
        //     a) 无旧轨迹且本次被拒 -> 参考点回落 m_cx[0]=0 -> 鼠标追向原点漂移
        //        (第四轮"未初始化漂移"的同类变体, 仅在极端注入下可达);
        //     b) 有旧轨迹但被拒 -> elapsed 重置导致旧轨迹从头重复播放。
        //   修复: 规划成功才重置 elapsed (成功时 m_target 精确等于传入的
        //   m_predictedX/Y, 用相等比较判定); 首次规划被拒则静止等待有效输入。
        if (m_planner.getTargetX() == m_predictedX &&
            m_planner.getTargetY() == m_predictedY)
        {
            m_elapsed = 0.0f;          // 规划成功
        }
        else if (!hadPlan)
        {
            // 无旧轨迹可用 + 本次被拒: 无参考点可追, 输出零增量保持静止,
            // 防止向 (0,0) 漂移, 等待上游输入恢复正常
            m_moveX    = 0.0f;
            m_moveY    = 0.0f;
            m_lastVelX = 0.0f;
            m_lastVelY = 0.0f;
            return;
        }
        // 有旧轨迹 + 被拒: 不重置 elapsed, 旧轨迹继续播放, 下一帧重试
    }

    // ---- [4] 取当前时刻的轨迹参考点 (拟人轨迹曲线为模块固有行为, 始终执行) ----
    m_elapsed += dt;
    if (m_elapsed > m_planner.getDuration())
        m_elapsed = m_planner.getDuration(); // 钳制, 防 float 长时间累加精度下降
    m_planner.getPosition(m_elapsed, m_trajRefX, m_trajRefY);

    // ---- [5] 分轴控制: 死区 + (MJ速度前馈 + PID反馈), 得到本周期 X / Y 移动增量 ----
    float errX = m_trajRefX - m_mouseX;
    float errY = m_trajRefY - m_mouseY;
    // MJ 参考速度作为前馈: 承担跟踪主体速度, 消除移动目标的稳态滞后
    float ffX = 0.0f, ffY = 0.0f;
    m_planner.getVelocity(m_elapsed, ffX, ffY);

    m_moveX = m_axisX.compute(errX, dt, ffX);
    m_moveY = m_axisY.compute(errY, dt, ffY);
    // 注: 此处不读取 m_axisX.lastVelocity() 同步 m_lastVel,
    //     因 [5.5]/[6] 会改变实际增量, 末尾统一以 m_moveX/dt 反推真实速度
    //     (第三轮修复), 提前赋值只会被覆盖 (死代码)。

    // ---- [5.5] 人手模拟 (可选): 对增量施加拟人扰动, 使轨迹不像机器 ----
    // 注意: 模块在"未移动"时自动跳过, 锁定后(死区内)不会抖动;
    //       扰动后由 [6] 全局单帧限幅兜底, 保证不瞬移。
    if (m_humanizer.enabled())
    {
        float hSpeed = std::sqrt(m_moveX * m_moveX + m_moveY * m_moveY) / dt;
        float hdx = m_predictedX - m_mouseX, hdy = m_predictedY - m_mouseY;
        float hDist = std::sqrt(hdx * hdx + hdy * hdy);
        m_humanizer.apply(m_moveX, m_moveY, hSpeed, hDist, dt);
    }

    // ---- [5.6] 增量有效性兜底 (第五轮加固, 全链最后防线) ----
    // 上游任何异常 (理论上经 axis feedforward / humanizer 输入逃逸) 使增量
    // 非有限时清零, 保证 m_mouseX/Y 在整个生命周期内恒有限:
    //   1) 否则 m_mouseX += NaN 会污染鼠标位置, 且无自愈路径;
    //   2) [6] 限幅中 Inf * (maxStep/Inf) = Inf*0 = NaN, 需先行拦截。
    // 正常路径增量恒有限, 此分支永不进入, 零开销零行为影响。
    if (!std::isfinite(m_moveX) || !std::isfinite(m_moveY))
    {
        m_moveX = 0.0f;
        m_moveY = 0.0f;
    }

    // ---- [5.7] 抵达减速 (arrival brake, 修复 R1 极限环 / R11 发散) ----
    // 根因: 逼近目标时 MJ 前馈仍按"重规划到旧 pivot"的高速度剖面推进, 缺少随
    //       真实残距递减的制动 -> 高速穿越死区 -> 反向再冲, 形成狩猎振荡;
    //       高 tracking/maxSpeed 组合下振幅逐次放大, 残差可达数千 px (R11 发散)。
    // 修复: 以"运动学可停条件"限制本帧合成速度, 保证始终有足够距离减速到目标速度:
    //         vAllow = sqrt(Vt^2 + 2 * aDecel * res)
    //       其中 res = 到预测目标点的真实残距, Vt = 目标自身速度估计。
    //   - 静止目标 Vt≈0 -> vAllow = sqrt(2*a*res): 越近越慢, 平滑收敛进死区;
    //     即使已高速冲向目标, 该上限也会强制减速, 从根上拦截过冲/发散。
    //   - 移动目标 Vt≈敌人速度 -> vAllow >= Vt: 上限恒高于跟随所需速度,
    //     制动不触发, 移动目标跟踪速度与滞后完全不受影响。
    //   公式对任意目标速度自洽, 无需"是否静止"硬门限。
    // Vt 直接取 Kalman 速度: 经 [0.5] 自运动补偿后它已是目标真实速度。
    // 准星速度取本帧实际合成增量反推 (m_move/dt), 无 1 帧滞后, 制动量精确。
    {
        float arx  = m_predictedX - m_mouseX;
        float ary  = m_predictedY - m_mouseY;
        float ares = std::sqrt(arx * arx + ary * ary);

        float tkx = 0.0f, tky = 0.0f, tvx = 0.0f, tvy = 0.0f;
        m_tracker.getState(tkx, tky, tvx, tvy);            // 目标真实速度 (已补偿自运动)
        float vtMag = std::sqrt(tvx * tvx + tvy * tvy);

        const float kArrivalDecel = 8000.0f;               // 制动减速度 (px/s^2)
        float vAllow   = std::sqrt(vtMag * vtMag + 2.0f * kArrivalDecel * ares);
        float curSpeed = std::sqrt(m_moveX * m_moveX + m_moveY * m_moveY) / dt;
        if (curSpeed > vAllow && curSpeed > 1.0e-3f)
        {
            float k = vAllow / curSpeed;
            m_moveX *= k;
            m_moveY *= k;
        }
    }

    // ---- [6] 全局单帧最大移动像素限制: 约束合成位移模长, 保持移动方向 ----
    float mag = std::sqrt(m_moveX * m_moveX + m_moveY * m_moveY);
    if (mag > m_maxStepPerFrame && mag > 1.0e-6f)
    {
        float scale = m_maxStepPerFrame / mag;
        m_moveX *= scale;
        m_moveY *= scale;
    }

    // ---- 推进鼠标位置 ----
    m_mouseX += m_moveX;
    m_mouseY += m_moveY;
    // 自运动补偿: 记录帧末位置, 下一帧据此推算镜头运动量 (见 [0.5])
    m_prevMouseX = m_mouseX;
    m_prevMouseY = m_mouseY;

    // ---- 同步实际移动速度 (本轮审查修复) ----
    // 问题: [5.5] humanizer 扰动与 [6] maxStep 缩放都会改变实际增量,
    //       但 m_lastVelX/Y 在此之前取自 AxisController 的输出速度,
    //       两者不一致会导致:
    //         a) 下一帧 planWithVelocity 以错误初速度规划轨迹, 起点斜率
    //            与实际运动方向/速率不符, 重规划瞬间出现速度跳变;
    //         b) getOutputVelocity() 报告的速度与实际位移不符 (失真)。
    // 修复: 以实际增量反推真实速度, 保证"轨迹初速度 = 实际运动速度",
    //       dt 已在函数入口钳制到 [kMinDt, kMaxDt], 无除零风险。
    m_lastVelX = m_moveX / dt;
    m_lastVelY = m_moveY / dt;
}

void MouseController::getMoveDelta(float& moveX, float& moveY) const
{
    moveX = m_moveX;
    moveY = m_moveY;
}

void MouseController::getPredictedTarget(float& x, float& y) const
{
    x = m_predictedX;
    y = m_predictedY;
}

void MouseController::getTrajectoryRef(float& x, float& y) const
{
    x = m_trajRefX;
    y = m_trajRefY;
}

void MouseController::getMousePosition(float& x, float& y) const
{
    x = m_mouseX;
    y = m_mouseY;
}

void MouseController::getOutputVelocity(float& vx, float& vy) const
{
    vx = m_lastVelX;
    vy = m_lastVelY;
}
