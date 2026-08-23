#include "Humanizer.h"
#include <algorithm>
#include <cmath>

// ============================================================================
// Humanizer.cpp
// ============================================================================

namespace
{
    constexpr float kTwoPi = 6.2831853f;
    constexpr float kWobbleHz    = 8.0f;   // 曲线扰动频率 (Hz), 人手自然摆动量级
    constexpr float kSpeedHz     = 2.5f;   // 速度波动频率 (Hz), 低频更自然
    constexpr float kOvershootDist = 30.0f; // 距目标多少像素内触发过冲
    constexpr float kOvershootMinSpd = 50.0f; // 触发过冲的最低速度 (px/s)
    constexpr int   kOvershootFrames  = 5;    // 过冲分几帧输出
    constexpr int   kOvershootCooldown = 20;  // 过冲后冷却帧数 (0.2s), 防反复触发振荡
    constexpr float kPowerCurvBase = 0.10f;   // 曲率代理参考值 (px^-1)
    constexpr float kPowerScaleMin = 0.85f;   // 2/3 幂律速度整形下限
    constexpr float kPowerScaleMax = 1.15f;   // 2/3 幂律速度整形上限
}

Humanizer::Humanizer()
    : m_rng(20260804u) // 固定种子, 轨迹可复现
{
}

void Humanizer::setSettings(const HumanizerSettings& s)
{
    // 统一入口做参数钳制，避免配置注入 NaN/Inf、负数或超大值破坏轨迹。
    HumanizerSettings safe = s;
    safe.microJitter    = (std::isfinite(safe.microJitter)    ? std::clamp(safe.microJitter,    0.0f, 5.0f)  : 0.0f);
    safe.wobble         = (std::isfinite(safe.wobble)         ? std::clamp(safe.wobble,         0.0f, 8.0f)  : 0.0f);
    safe.overshoot      = (std::isfinite(safe.overshoot)      ? std::clamp(safe.overshoot,      0.0f, 10.0f) : 0.0f);
    safe.speedVariation = (std::isfinite(safe.speedVariation) ? std::clamp(safe.speedVariation, 0.0f, 1.0f)  : 0.0f);
    safe.powerLaw       = (std::isfinite(safe.powerLaw)       ? std::clamp(safe.powerLaw,       0.0f, 1.0f)  : 0.0f);
    m_s = safe;
    reset();
}

bool Humanizer::enabled() const
{
    return m_s.microJitter > 0.0f
        || m_s.wobble > 0.0f
        || m_s.overshoot > 0.0f
        || m_s.speedVariation > 0.0f
        || m_s.powerLaw > 0.0f;
}

void Humanizer::reset()
{
    m_wobblePhase = 0.0f;
    m_speedPhase  = 0.0f;
    m_overshootRemaining = 0;
    m_overshootCooldown = 0;
    m_oshootDX = 0.0f;
    m_oshootDY = 0.0f;
    m_hasStep = false;
    m_prevDx = 0.0f;
    m_prevDy = 0.0f;
}

void Humanizer::apply(float& dx, float& dy, float speed, float distToTarget, float dt)
{
    if (!enabled())
        return;

    // dt 防护: NaN/负值钳制为最小步长, 防止相位更新被污染
    if (!(dt > 0.0f) || !std::isfinite(dt))
        dt = 1.0e-3f;

    // 增量有效性防护 (第五轮加固): NaN/Inf 增量直接清零返回 (模块独立边界)。
    // 否则 sqrt(NaN) 经 baseLen 判定逃逸、法向扰动/乘性扰动使污染扩散,
    // 且 NaN 经 m_wobblePhase / m_speedPhase 相位累积无法自愈;
    // 主调用路径 (MouseController) 增量恒有限, 此防护零影响。
    if (!std::isfinite(dx) || !std::isfinite(dy))
    {
        dx = 0.0f;
        dy = 0.0f;
        return;
    }

    // 输入防护 (本轮审查加固): speed / distToTarget 为 NaN/Inf 时钳制为 0。
    // 否则 NaN 经 scale 传播进增量, 且 std::sin(NaN) 返回 NaN 会经
    // m_wobblePhase / m_speedPhase 永久污染后续所有帧 (相位不再有限)。
    // 主调用路径 (MouseController) 传入值恒有限, 此防护仅保模块独立安全。
    // (第九轮) speed 负值也钳为 0: 负 speed 使 scale = 0.4 + 0.6*min(speed/1500,1)
    // 可为负 (如 speed=-10000 -> scale=-3.6), 微观抖动/曲线扰动幅度反号
    // 并放大, 模块独立边界 (外部直调) 下的行为失真; 主调用路径 speed 为
    // sqrt(...) 恒非负, 此钳制零影响。
    if (!std::isfinite(speed) || speed < 0.0f) speed = 0.0f;
    if (!std::isfinite(distToTarget)) distToTarget = 0.0f;

    // 未在移动 (如已进入死区, moveDelta≈0) 时不扰动, 避免锁定后静止抖动
    const float baseLen = std::sqrt(dx * dx + dy * dy);
    if (baseLen < 1.0e-3f)
        return;

    // ---- 1) 微观抖动: 幅度随速度增大 (真实手抖特性), 用缓存的标准分布乘幅度 ----
    if (m_s.microJitter > 0.0f)
    {
        float scale = 0.4f + 0.6f * std::min(speed / 1500.0f, 1.0f);
        float amp = m_s.microJitter * scale;
        dx += m_gauss(m_rng) * amp;
        dy += m_gauss(m_rng) * amp;
    }

    // ---- 2) 曲线扰动: 垂直运动方向的低频摆动, 让路径弯曲 ----
    if (m_s.wobble > 0.0f)
    {
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.0e-6f)
        {
            float ux = dx / len, uy = dy / len;
            float wob = m_s.wobble * std::min(speed / 800.0f, 1.0f) * std::sin(m_wobblePhase);
            dx += -uy * wob;   // 法向分量
            dy +=  ux * wob;
            m_wobblePhase = std::fmod(m_wobblePhase + dt * kTwoPi * kWobbleHz, kTwoPi);
        }
    }

    // ---- 3) 速度波动: 低频乘性扰动, 加减速不规律 ----
    if (m_s.speedVariation > 0.0f)
    {
        float v = 1.0f + m_s.speedVariation * std::sin(m_speedPhase);
        dx *= v;
        dy *= v;
        m_speedPhase = std::fmod(m_speedPhase + dt * kTwoPi * kSpeedHz, kTwoPi);
    }

    // ---- 4) 末端过冲: 接近目标时沿当前方向轻微冲过, 由 PID 自然回正 ----
    if (m_s.overshoot > 0.0f)
    {
        if (m_overshootCooldown > 0)
            --m_overshootCooldown;

        if (m_overshootRemaining <= 0)
        {
            // 冷却期内不重复触发, 防止 tracking 调大后"过冲-回正-再过冲"振荡
            if (m_overshootCooldown <= 0
                && distToTarget < kOvershootDist && speed > kOvershootMinSpd)
            {
                float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.0e-6f)
                {
                    float per = m_s.overshoot / kOvershootFrames;
                    m_oshootDX = (dx / len) * per;
                    m_oshootDY = (dy / len) * per;
                    m_overshootRemaining = kOvershootFrames;
                    m_overshootCooldown = kOvershootCooldown;
                }
            }
        }
        else
        {
            dx += m_oshootDX;
            dy += m_oshootDY;
            --m_overshootRemaining;
        }
    }

    // ---- 5) 2/3 幂律 (可选, 默认关闭): 按曲率代理调整本帧速度 ----
    // 公式按需求: speed ~ curvature^(1/3)。
    // 曲率代理用"相邻两帧位移方向夹角 / 本帧步长"近似, 不改变最终方向,
    // 只轻微调整速度幅度。默认 powerLaw=0 时完全跳过, 不影响既有主链路。
    if (m_s.powerLaw > 0.0f)
    {
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.0e-3f)
        {
            if (m_hasStep)
            {
                const float dot = std::clamp((dx * m_prevDx + dy * m_prevDy) /
                                             (len * std::sqrt(m_prevDx * m_prevDx + m_prevDy * m_prevDy)),
                                             -1.0f, 1.0f);
                const float angle = std::acos(dot);
                const float curvature = angle / len;
                const float speedScale = std::pow(curvature / kPowerCurvBase, 1.0f / 3.0f);
                const float scale = std::clamp(1.0f + m_s.powerLaw * (speedScale - 1.0f),
                                               kPowerScaleMin, kPowerScaleMax);
                dx *= scale;
                dy *= scale;
            }
            m_prevDx = dx;
            m_prevDy = dy;
            m_hasStep = true;
        }
        else
        {
            m_hasStep = false;
        }
    }
}
