#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <cmath>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <atomic>
#include <vector>
#include <iostream>
#include <random>

#include "mouse.h"
#include "capture.h"
#include "mybot.h"

namespace
{
aim::AimKalmanSettings buildKalmanSettingsFromConfigUnlocked()
{
    aim::AimKalmanSettings settings;
    settings.enabled = config.kalman_enabled;
    settings.process_noise_position = static_cast<double>(config.kalman_process_noise_position);
    settings.process_noise_velocity = static_cast<double>(config.kalman_process_noise_velocity);
    settings.measurement_noise = static_cast<double>(config.kalman_measurement_noise);
    settings.velocity_damping = static_cast<double>(config.kalman_velocity_damping);
    settings.max_velocity = static_cast<double>(config.kalman_max_velocity);
    settings.warmup_frames = config.kalman_warmup_frames;
    return settings;
}

aim::AimKalmanSettings buildKalmanSettingsFromConfig()
{
    std::lock_guard<std::mutex> lock(configMutex);
    return buildKalmanSettingsFromConfigUnlocked();
}

double currentFrameIntervalSec()
{
    double fps = static_cast<double>(captureFps.load());
    if (fps <= 0.0)
    {
        std::lock_guard<std::mutex> lock(configMutex);
        fps = (config.capture_fps > 0) ? static_cast<double>(config.capture_fps) : 60.0;
    }

    fps = std::clamp(fps, 15.0, 500.0);
    return 1.0 / fps;
}

// 将浮点位移安全折算为鼠标计数。
// 1) 拦截 NaN/Inf: static_cast<int>(非有限值) 属未定义行为，实测会得到
//    INT_MIN 级别的巨量数值，再被后端窄化为 short 后表现为准星瞬移。
// 2) 钳制到 short 值域: KM_move / kmNet_mouse_move 形参均为 short，
//    超范围整数在窄化时会回绕成反向位移。
// 对有限且在值域内的输入，结果与原先的 static_cast<int> 截断逐位一致。
int toMoveCount(double value)
{
    if (!std::isfinite(value))
        return 0;
    return static_cast<int>(std::clamp(value, -32767.0, 32767.0));
}
}

MouseThread::MouseThread(
    int resolution,
    int fovX,
    int fovY,
    double minSpeedMultiplier,
    double maxSpeedMultiplier,
    double predictionInterval,
    bool auto_shoot,
    float bScope_multiplier,
    IMouseInput* mouseInputDevice)
    : screen_width(resolution),
    screen_height(resolution),
    prediction_interval(predictionInterval),
    fov_x(fovX),
    fov_y(fovY),
    max_distance(std::hypot(resolution, resolution) / 2.0),
    min_speed_multiplier(minSpeedMultiplier),
    max_speed_multiplier(maxSpeedMultiplier),
    center_x(resolution / 2.0),
    center_y(resolution / 2.0),
    auto_shoot(auto_shoot),
    bScope_multiplier(bScope_multiplier),
    mouseInput(mouseInputDevice),

    prev_velocity_x(0.0),
    prev_velocity_y(0.0),
    prev_x(0.0),
    prev_y(0.0)
{
    prev_time = std::chrono::steady_clock::time_point();
    last_target_time = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(configMutex);
    const int activeSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
    const Config::MouseHotkey* profile =
        activeSlot >= 0 && activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS)
            ? &config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)] : nullptr;
    curve_enabled = profile ? profile->localBool("curve_enabled", config.curve_enabled) : config.curve_enabled;
    curve_intensity = profile ? profile->localFloat("curve_intensity", config.curve_intensity) : config.curve_intensity;
    perturbation_strength = profile ? profile->localFloat("perturbation_strength", config.perturbation_strength) : config.perturbation_strength;
    curve_max_speed = profile ? profile->localFloat("curve_max_speed", config.curve_max_speed) : config.curve_max_speed;
    curve_distance = profile ? profile->localFloat("curve_distance", config.curve_distance) : config.curve_distance;
    snapRadius = profile ? profile->localFloat("snapRadius", config.snapRadius) : static_cast<float>(config.snapRadius);
    nearRadius = profile ? profile->localFloat("nearRadius", config.nearRadius) : static_cast<float>(config.nearRadius);
    speedCurveExponent = profile ? profile->localFloat("speedCurveExponent", config.speedCurveExponent) : config.speedCurveExponent;
    snapBoostFactor = profile ? profile->localFloat("snapBoostFactor", config.snapBoostFactor) : config.snapBoostFactor;
    profileMinSpeedMultiplier = profile ? profile->localFloat("minSpeedMultiplier", static_cast<float>(min_speed_multiplier)) : static_cast<float>(min_speed_multiplier);
    profileMaxSpeedMultiplier = profile ? profile->localFloat("maxSpeedMultiplier", static_cast<float>(max_speed_multiplier)) : static_cast<float>(max_speed_multiplier);
    }
    resetCurveState();
    clearCurveDebugTrail();
    targetKalman.setSettings(buildKalmanSettingsFromConfig());
    targetKalman.reset();
    lastKalmanTelemetry = {};
    lastPredictionLookaheadSec = 0.0;
    lastDetectionDelaySec = 0.0;

    // mc_enabled_ 由 applyPendingConfigLocked 设置，syncMouseController 只读返回
    mc_enabled_ = false;
    syncMouseController();
    moveWorker = std::thread(&MouseThread::moveWorkerLoop, this);
}

void MouseThread::updateConfig(
    int resolution,
    int fovX,
    int fovY,
    double minSpeedMultiplier,
    double maxSpeedMultiplier,
    double predictionInterval,
    bool auto_shoot,
    float bScope_multiplier
)
{
    // 调用方（overlay 渲染线程 / keyboard 监听线程 / 鼠标线程）进入时已持有
    // configMutex，故此处对 config 的读取是安全的。
    //
    // 但本函数原先直接写入 screen_width/fov/center/max_distance/curve_*、
    // 并调用 resetCurveState()（改写 std::mt19937 状态）、targetKalman.reset()、
    // syncMouseController()（改写 m_mc 内部 PID/Kalman 状态）。这些数据同时被
    // 鼠标线程在 moveMousePivot() 热路径中读写 —— 构成数据竞争（UB）。
    // 典型触发：瞄准过程中拖动 ImGui 的 FOV / 速度滑块。
    //
    // 又因鼠标线程的锁序是 input_method_mutex -> configMutex，此处（已持
    // configMutex）不能反向获取 input_method_mutex，否则 ABBA 死锁。
    // 故改为暂存快照，由鼠标线程在持有 input_method_mutex 时应用。
    PendingConfig staged;
    staged.resolution = resolution;
    staged.fovX = fovX;
    staged.fovY = fovY;
    staged.minSpeedMultiplier = minSpeedMultiplier;
    staged.maxSpeedMultiplier = maxSpeedMultiplier;
    staged.predictionInterval = predictionInterval;
    staged.autoShoot = auto_shoot;
    staged.bScopeMultiplier = bScope_multiplier;
    staged.curveEnabled = config.curve_enabled;
    staged.curveIntensity = config.curve_intensity;
    staged.perturbationStrength = config.perturbation_strength;
    staged.curveMaxSpeed = config.curve_max_speed;
    staged.curveDistance = config.curve_distance;
    staged.kalmanSettings = buildKalmanSettingsFromConfigUnlocked();
    // ---- PendingConfig 新增字段填充 (mc_* / 预测 / 射击 / 速度曲线 profile) ----
    // 注: 调用方已持 configMutex，此处直读 config 安全。
    staged.mcEnabled = config.mc_enabled;
    staged.mcMaxStep = config.mc_maxstep;
    staged.mcRetarget = config.mc_retarget;
    staged.mcAheadMin = config.mc_ahead_min;  staged.mcAheadMax = config.mc_ahead_max;
    staged.mcDurMin = config.mc_dur_min;      staged.mcDurMax = config.mc_dur_max;
    staged.mcKalmanQ = config.mc_kalman_q;    staged.mcKalmanR = config.mc_kalman_r;
    staged.mcXTracking = config.mc_x_tracking; staged.mcXDamping = config.mc_x_damping;
    staged.mcXMaxspeed = config.mc_x_maxspeed; staged.mcXIntegral = config.mc_x_integral;
    staged.mcXDeadzone = config.mc_x_deadzone;
    staged.mcYTracking = config.mc_y_tracking; staged.mcYDamping = config.mc_y_damping;
    staged.mcYMaxspeed = config.mc_y_maxspeed; staged.mcYIntegral = config.mc_y_integral;
    staged.mcYDeadzone = config.mc_y_deadzone;
    staged.kalmanCompensateDetectionDelay = config.kalman_compensate_detection_delay;
    staged.kalmanAdditionalPredictionMs = config.kalman_additional_prediction_ms;
    staged.kalmanResetTimeoutSec = config.kalman_reset_timeout_sec;
    staged.bScopeMultiplierProfile = config.bScope_multiplier;
    {
        const int activeSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
        if (activeSlot >= 0 && activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS))
        {
            const auto& profile = config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)];
            staged.bScopeMultiplierProfile = profile.localFloat("bScope_multiplier", staged.bScopeMultiplierProfile);
            staged.snapRadius = profile.localFloat("snapRadius", config.snapRadius);
            staged.nearRadius = profile.localFloat("nearRadius", config.nearRadius);
            staged.speedCurveExponent = profile.localFloat("speedCurveExponent", config.speedCurveExponent);
            staged.snapBoostFactor = profile.localFloat("snapBoostFactor", config.snapBoostFactor);
            staged.profileMinSpeedMultiplier = profile.localFloat("minSpeedMultiplier", static_cast<float>(staged.minSpeedMultiplier));
            staged.profileMaxSpeedMultiplier = profile.localFloat("maxSpeedMultiplier", static_cast<float>(staged.maxSpeedMultiplier));
        }
        else
        {
            staged.snapRadius = config.snapRadius;
            staged.nearRadius = config.nearRadius;
            staged.speedCurveExponent = config.speedCurveExponent;
            staged.snapBoostFactor = config.snapBoostFactor;
            staged.profileMinSpeedMultiplier = static_cast<float>(staged.minSpeedMultiplier);
            staged.profileMaxSpeedMultiplier = static_cast<float>(staged.maxSpeedMultiplier);
        }
    }

    {
        std::lock_guard<std::mutex> lock(pendingConfigMutex);
        pendingConfig = staged;
    }
    pendingConfigDirty.store(true, std::memory_order_release);
}

void MouseThread::applyPendingConfigLocked()
{
    // 前置条件: 调用方持有 input_method_mutex（鼠标线程）。
    if (!pendingConfigDirty.load(std::memory_order_acquire))
        return;

    PendingConfig staged;
    {
        std::lock_guard<std::mutex> lock(pendingConfigMutex);
        if (!pendingConfig.has_value())
        {
            pendingConfigDirty.store(false, std::memory_order_release);
            return;
        }
        staged = *pendingConfig;
        pendingConfig.reset();
        pendingConfigDirty.store(false, std::memory_order_release);
    }

    // 以下写入序列与原 updateConfig 完全一致，仅执行线程改为鼠标线程。
    screen_width = screen_height = staged.resolution;
    fov_x = staged.fovX;  fov_y = staged.fovY;
    min_speed_multiplier = staged.minSpeedMultiplier;
    max_speed_multiplier = staged.maxSpeedMultiplier;
    prediction_interval = staged.predictionInterval;
    auto_shoot = staged.autoShoot;
    bScope_multiplier = staged.bScopeMultiplier;

    center_x = center_y = staged.resolution / 2.0;
    max_distance = std::hypot(staged.resolution, staged.resolution) / 2.0;

    curve_enabled = staged.curveEnabled;
    curve_intensity = staged.curveIntensity;
    perturbation_strength = staged.perturbationStrength;
    curve_max_speed = staged.curveMaxSpeed;
    curve_distance = staged.curveDistance;
    snapRadius = staged.snapRadius;
    nearRadius = staged.nearRadius;
    speedCurveExponent = staged.speedCurveExponent;
    snapBoostFactor = staged.snapBoostFactor;
    profileMinSpeedMultiplier = staged.profileMinSpeedMultiplier;
    profileMaxSpeedMultiplier = staged.profileMaxSpeedMultiplier;
    resetCurveState();
    clearCurveDebugTrail();
    targetKalman.setSettings(staged.kalmanSettings);
    targetKalman.reset();
    lastKalmanTelemetry = {};
    lastPredictionLookaheadSec = 0.0;
    lastDetectionDelaySec = 0.0;

    // 应用 mc_* 调参到 MouseController
    mc_enabled_ = staged.mcEnabled;
    {
        AxisTuning tx, ty;
        tx.tracking = staged.mcXTracking; tx.damping = staged.mcXDamping;
        tx.maxSpeed = staged.mcXMaxspeed;  tx.integral = staged.mcXIntegral;
        tx.deadzone = staged.mcXDeadzone;
        ty.tracking = staged.mcYTracking; ty.damping = staged.mcYDamping;
        ty.maxSpeed = staged.mcYMaxspeed;  ty.integral = staged.mcYIntegral;
        ty.deadzone = staged.mcYDeadzone;
        m_mc.setAxisTuningX(tx);
        m_mc.setAxisTuningY(ty);
        m_mc.setMaxStepPerFrame(staged.mcMaxStep);
        m_mc.setRetargetThreshold(staged.mcRetarget);
        m_mc.enableAdaptiveAhead(true, staged.mcAheadMin, staged.mcAheadMax);
        m_mc.enableAdaptiveDuration(true, staged.mcDurMin, staged.mcDurMax, 2500.0f);
        m_mc.tracker().setProcessNoise(staged.mcKalmanQ);
        m_mc.tracker().setMeasurementNoise(staged.mcKalmanR);
    }
}

void MouseThread::applyPendingConfig()
{
    if (!pendingConfigDirty.load(std::memory_order_acquire))
        return;
    std::lock_guard lg(input_method_mutex);
    applyPendingConfigLocked();
}

bool MouseThread::syncMouseController()
{
    // 从 PendingConfig 快照读取 mc_* 参数（由 applyPendingConfigLocked 预先填入 m_mc），
    // 不再锁 configMutex，避免与 overlay 线程的 configMutex 临界区竞争。
    // 自缓存 mc_enabled_ 由 applyPendingConfigLocked 在 input_method_mutex 下设置，
    // 此处只读返回，无需额外同步。
    return mc_enabled_;
}

MouseThread::~MouseThread()
{
    workerStop = true;
    queueCv.notify_all();
    if (moveWorker.joinable()) moveWorker.join();
}

void MouseThread::queueMove(int dx, int dy)
{
    if (dx == 0 && dy == 0)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto generation = moveGeneration.load(std::memory_order_acquire);
    std::lock_guard lg(queueMtx);

    if (!moveQueue.empty() && moveQueue.back().generation == generation)
    {
        moveQueue.back().dx = std::clamp(moveQueue.back().dx + dx, -32767, 32767);
        moveQueue.back().dy = std::clamp(moveQueue.back().dy + dy, -32767, 32767);
        moveQueue.back().queuedAt = now;
        queueCv.notify_one();
        return;
    }

    if (moveQueue.size() >= queueLimit)
        moveQueue.pop();
    moveQueue.push({ dx, dy, generation, now });
    queueCv.notify_one();
}

void MouseThread::moveWorkerLoop()
{
    try
    {
        while (!workerStop)
        {
            std::unique_lock ul(queueMtx);
            queueCv.wait(ul, [&] { return workerStop || !moveQueue.empty(); });

            while (!moveQueue.empty())
            {
                Move m = moveQueue.front();
                moveQueue.pop();
                ul.unlock();
                const auto now = std::chrono::steady_clock::now();
                const bool isCurrent = m.generation == moveGeneration.load(std::memory_order_acquire);
                const bool isFresh = now - m.queuedAt <= maxQueuedMoveAge;
                if (isCurrent && isFresh)
                {
                    if (sendMovementToDriver(m.dx, m.dy))
                        appendCurveDebugStep(m.dx, m.dy);
                }
                ul.lock();
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[鼠标] 移动工作线程崩溃: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[鼠标] 移动工作线程崩溃: 未知异常。" << std::endl;
    }
}

void MouseThread::mouseCurveMoveRelative(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    // 曲线参数由 applyPendingConfigLocked() 从 PendingConfig 快照写入成员变量，
    // 调用方（moveMousePivot/moveMouse）已持有 input_method_mutex 并调用
    // applyPendingConfigLocked()，故此处直接使用成员变量，无需重复锁 configMutex。
    curveCarryX += static_cast<double>(dx);
    curveCarryY += static_cast<double>(dy);

    const double G = std::isfinite(curve_intensity) ? std::max(0.0, curve_intensity) : 0.0;
    const double W = std::isfinite(perturbation_strength) ? std::max(0.0, perturbation_strength) : 0.0;
    const double M = std::isfinite(curve_max_speed) ? std::max(0.65, curve_max_speed) : 0.65;
    const double D = std::isfinite(curve_distance) ? std::max(1.0, curve_distance) : 1.0;

    constexpr double twoPi = 6.28318530717958647692;

    const double carryMag = std::hypot(curveCarryX, curveCarryY);
    const int maxSubsteps = std::clamp(static_cast<int>(carryMag * 0.24) + 1, 1, 5);

    for (int i = 0; i < maxSubsteps; ++i)
    {
        const double dist = std::hypot(curveCarryX, curveCarryY);
        const double velMag = std::hypot(curveVelX, curveVelY);

        if (dist < 0.20 && velMag < 0.12)
            break;

        const double normDist = std::clamp(dist / D, 0.0, 1.0);
        const double pullGain = G * (0.25 + 0.75 * normDist);
        const double noiseAmp = W * (0.15 + 0.85 * normDist);

        double pullX = 0.0;
        double pullY = 0.0;
        if (dist > 1e-8)
        {
            pullX = curveCarryX / dist * pullGain;
            pullY = curveCarryY / dist * pullGain;
        }

        curvePatternRateA = std::clamp(curvePatternRateA + curveNoiseDistribution(curveRng) * 0.004, 0.025, 0.280);
        curvePatternRateB = std::clamp(curvePatternRateB + curveNoiseDistribution(curveRng) * 0.004, 0.025, 0.280);

        const double stepTempo = 0.20 + 0.95 * normDist;
        curvePatternPhaseA += curvePatternRateA * stepTempo;
        curvePatternPhaseB += curvePatternRateB * stepTempo;
        if (curvePatternPhaseA > twoPi) curvePatternPhaseA = std::fmod(curvePatternPhaseA, twoPi);
        if (curvePatternPhaseB > twoPi) curvePatternPhaseB = std::fmod(curvePatternPhaseB, twoPi);

        const double oscAX = std::sin(curvePatternPhaseA);
        const double oscBX = std::sin(curvePatternPhaseB + 1.61803398875);
        const double oscAY = std::cos(curvePatternPhaseA * 0.79 + 0.35);
        const double oscBY = std::cos(curvePatternPhaseB * 1.17 - 0.48);

        const double patternAmp = W * (0.05 + 0.55 * normDist);
        const double patternTargetX = (oscAX + 0.58 * oscBX) * patternAmp;
        const double patternTargetY = (oscAY + 0.58 * oscBY) * patternAmp;
        const double patternBlend = 0.12 + 0.20 * normDist;
        curvePatternX = curvePatternX * (1.0 - patternBlend) + patternTargetX * patternBlend;
        curvePatternY = curvePatternY * (1.0 - patternBlend) + patternTargetY * patternBlend;

        curveNoiseX = curveNoiseX * 0.72 + curveNoiseDistribution(curveRng) * noiseAmp * 0.28;
        curveNoiseY = curveNoiseY * 0.72 + curveNoiseDistribution(curveRng) * noiseAmp * 0.28;

        const double perturbForceX = curveNoiseX + curvePatternX * 0.42;
        const double perturbForceY = curveNoiseY + curvePatternY * 0.42;

        const double drag = 0.82 + (1.0 - normDist) * 0.10;
        curveVelX = curveVelX * drag + pullX + perturbForceX;
        curveVelY = curveVelY * drag + pullY + perturbForceY;

        const double vCap = std::max(0.65, M * (0.30 + 0.70 * normDist));
        const double newVelMag = std::hypot(curveVelX, curveVelY);
        if (newVelMag > vCap)
        {
            const double clip = vCap * curveClipDistribution(curveRng);
            curveVelX = (curveVelX / newVelMag) * clip;
            curveVelY = (curveVelY / newVelMag) * clip;
        }

        curveFracX += curveVelX;
        curveFracY += curveVelY;

        int stepX = static_cast<int>(std::round(curveFracX));
        int stepY = static_cast<int>(std::round(curveFracY));
        if (stepX == 0 && stepY == 0)
            continue;

        curveFracX -= static_cast<double>(stepX);
        curveFracY -= static_cast<double>(stepY);
        curveCarryX -= static_cast<double>(stepX);
        curveCarryY -= static_cast<double>(stepY);
        queueMove(stepX, stepY);
    }

    const double carryCap = 120.0;
    const double finalCarryMag = std::hypot(curveCarryX, curveCarryY);
    if (finalCarryMag > carryCap)
    {
        const double s = carryCap / finalCarryMag;
        curveCarryX *= s;
        curveCarryY *= s;
    }
}

void MouseThread::resetCurveState()
{
    constexpr double twoPi = 6.28318530717958647692;
    std::uniform_real_distribution<double> phaseDist(0.0, twoPi);
    std::uniform_real_distribution<double> rateDist(0.04, 0.16);

    curveCarryX = 0.0;
    curveCarryY = 0.0;
    curveVelX = 0.0;
    curveVelY = 0.0;
    curveNoiseX = 0.0;
    curveNoiseY = 0.0;
    curveFracX = 0.0;
    curveFracY = 0.0;
    curvePatternX = 0.0;
    curvePatternY = 0.0;
    curvePatternPhaseA = phaseDist(curveRng);
    curvePatternPhaseB = phaseDist(curveRng);
    curvePatternRateA = rateDist(curveRng);
    curvePatternRateB = rateDist(curveRng);
}

void MouseThread::appendCurveDebugStep(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    auto delta = mouseCountsToScreenPixels(dx, dy);
    double deltaPxX = delta.first;
    double deltaPxY = delta.second;
    if (std::abs(deltaPxX) < 1e-8 && std::abs(deltaPxY) < 1e-8)
        return;
    std::lock_guard<std::mutex> lock(curveDebugTrailMutex);
    const auto now = std::chrono::steady_clock::now();
    pruneCurveDebugTrailLocked(now);

    if (curveDebugTrail.empty())
    {
        curveDebugCursorX = center_x;
        curveDebugCursorY = center_y;
        curveDebugTrail.push_back({ curveDebugCursorX, curveDebugCursorY, now });
    }

    curveDebugCursorX += deltaPxX;
    curveDebugCursorY += deltaPxY;
    curveDebugTrail.push_back({ curveDebugCursorX, curveDebugCursorY, now });

    constexpr size_t maxTrailPoints = 220;
    while (curveDebugTrail.size() > maxTrailPoints)
        curveDebugTrail.pop_front();
}

void MouseThread::pruneCurveDebugTrailLocked(const std::chrono::steady_clock::time_point& now)
{
    constexpr auto curveTrailLifetime = std::chrono::milliseconds(900);
    while (!curveDebugTrail.empty() && (now - curveDebugTrail.front().t) > curveTrailLifetime)
        curveDebugTrail.pop_front();
}

std::pair<double, double> MouseThread::mouseCountsToScreenPixels(int dx, int dy) const
{
    double deltaPxX = static_cast<double>(dx);
    double deltaPxY = static_cast<double>(dy);

    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        const Config::GameProfile* gpPtr = nullptr;

        auto activeIt = config.game_profiles.find(config.active_game);
        if (activeIt != config.game_profiles.end())
            gpPtr = &activeIt->second;
        else
        {
            auto unifiedIt = config.game_profiles.find("UNIFIED");
            if (unifiedIt != config.game_profiles.end())
                gpPtr = &unifiedIt->second;
        }

        if (gpPtr && gpPtr->sens != 0.0 && gpPtr->yaw != 0.0 && gpPtr->pitch != 0.0)
        {
            const double fovNow = std::max(1.0, fov_x);
            const double fovScale = (gpPtr->fovScaled && gpPtr->baseFOV > 1.0) ? (fovNow / gpPtr->baseFOV) : 1.0;
            const double degX = static_cast<double>(dx) * gpPtr->sens * gpPtr->yaw * fovScale;
            const double degY = static_cast<double>(dy) * gpPtr->sens * gpPtr->pitch * fovScale;

            const double degPerPxX = fov_x / std::max(1.0, screen_width);
            const double degPerPxY = fov_y / std::max(1.0, screen_height);

            if (std::abs(degPerPxX) > 1e-8 && std::abs(degPerPxY) > 1e-8)
            {
                deltaPxX = degX / degPerPxX;
                deltaPxY = degY / degPerPxY;
            }
        }
    }

    return { deltaPxX, deltaPxY };
}

void MouseThread::recordMotionCompensationStep(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    const auto delta = mouseCountsToScreenPixels(dx, dy);
    if (std::abs(delta.first) < 1e-8 && std::abs(delta.second) < 1e-8)
        return;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(motionCompensationMutex);
    pruneMotionCompensationTrailLocked(now);
    motionCompensationTrail.push_back({ delta.first, delta.second, now });

    constexpr size_t maxSamples = 512;
    while (motionCompensationTrail.size() > maxSamples)
        motionCompensationTrail.pop_front();
}

void MouseThread::pruneMotionCompensationTrailLocked(const std::chrono::steady_clock::time_point& now)
{
    constexpr auto motionTrailLifetime = std::chrono::seconds(2);
    while (!motionCompensationTrail.empty() && (now - motionCompensationTrail.front().t) > motionTrailLifetime)
        motionCompensationTrail.pop_front();
}

std::pair<double, double> MouseThread::getMotionCompensationSince(
    std::chrono::steady_clock::time_point since) const
{
    if (since.time_since_epoch().count() == 0)
        return { 0.0, 0.0 };

    double x = 0.0;
    double y = 0.0;
    std::lock_guard<std::mutex> lock(motionCompensationMutex);
    for (const auto& sample : motionCompensationTrail)
    {
        if (sample.t >= since)
        {
            x += sample.x;
            y += sample.y;
        }
    }

    return { x, y };
}

double MouseThread::currentDetectionDelaySec(double observationAgeSec) const
{
    double detectionDelaySec = 0.05;
    if (std::isfinite(observationAgeSec) && observationAgeSec >= 0.0)
    {
        detectionDelaySec = observationAgeSec;
    }
    else
    {
#ifdef USE_CUDA
        if (trt_detector)
            detectionDelaySec = trt_detector->lastInferenceTimeMs.load(std::memory_order_relaxed) * 0.001;
#else
        if (dml_detector)
            detectionDelaySec = dml_detector->lastInferenceTimeMs.load(std::memory_order_relaxed) * 0.001;
#endif
    }
    if (!std::isfinite(detectionDelaySec))
        detectionDelaySec = 0.05;
    return std::clamp(detectionDelaySec, 0.0, 0.35);
}

double MouseThread::currentPredictionLookaheadSec(double detectionDelaySec) const
{
    double lookahead = std::max(0.0, prediction_interval);
    bool compensateDetectionDelay = false;
    float additionalPredictionMs = 0.0f;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        compensateDetectionDelay = config.kalman_compensate_detection_delay;
        additionalPredictionMs = config.kalman_additional_prediction_ms;
        const int activeSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
        if (activeSlot >= 0 && activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS))
        {
            const auto& profile = config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)];
            lookahead = std::max(0.0, static_cast<double>(profile.localFloat(
                "predictionInterval", static_cast<float>(lookahead))));
        }
    }

    if (compensateDetectionDelay)
        lookahead += std::max(0.0, detectionDelaySec);
    lookahead += static_cast<double>(additionalPredictionMs) * 0.001;
    return std::clamp(lookahead, 0.0, 1.5);
}

std::pair<double, double> MouseThread::predict_target_position(
    double target_x,
    double target_y,
    std::chrono::steady_clock::time_point observationTime)
{
    auto current_time = std::chrono::steady_clock::now();
    if (observationTime.time_since_epoch().count() == 0)
        observationTime = current_time;

    double observationAgeSec = std::chrono::duration<double>(current_time - observationTime).count();
    if (!std::isfinite(observationAgeSec) || observationAgeSec < 0.0)
        observationAgeSec = 0.0;

    targetKalman.setSettings(buildKalmanSettingsFromConfig());

    if (prev_time.time_since_epoch().count() == 0 || !target_detected.load())
    {
        prev_time = observationTime;
        prev_x = target_x;
        prev_y = target_y;
        prev_velocity_x = 0.0;
        prev_velocity_y = 0.0;
        targetKalman.reset();
        const double detectionDelaySec = currentDetectionDelaySec(observationAgeSec);
        const double lookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
        lastKalmanTelemetry = targetKalman.update(target_x, target_y, currentFrameIntervalSec(), lookaheadSec);
        lastDetectionDelaySec = detectionDelaySec;
        lastPredictionLookaheadSec = lookaheadSec;
        return { target_x, target_y };
    }

    const double observationDt = std::chrono::duration<double>(observationTime - prev_time).count();
    if (!std::isfinite(observationDt) || observationDt <= 0.0)
    {
        const double detectionDelaySec = currentDetectionDelaySec(observationAgeSec);
        const double lookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
        lastDetectionDelaySec = detectionDelaySec;
        lastPredictionLookaheadSec = lookaheadSec;
        const auto predicted = targetKalman.predict(lookaheadSec);
        lastKalmanTelemetry.predicted_x = std::isfinite(predicted.first) ? predicted.first : target_x;
        lastKalmanTelemetry.predicted_y = std::isfinite(predicted.second) ? predicted.second : target_y;
        return { lastKalmanTelemetry.predicted_x, lastKalmanTelemetry.predicted_y };
    }

    const double dt = std::clamp(observationDt, 1.0 / 500.0, 0.25);
    prev_time = observationTime;
    prev_x = target_x;
    prev_y = target_y;

    const double detectionDelaySec = currentDetectionDelaySec(observationAgeSec);
    const double lookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
    lastDetectionDelaySec = detectionDelaySec;
    lastPredictionLookaheadSec = lookaheadSec;

    lastKalmanTelemetry = targetKalman.update(target_x, target_y, dt, lookaheadSec);
    prev_velocity_x = lastKalmanTelemetry.velocity_x;
    prev_velocity_y = lastKalmanTelemetry.velocity_y;

    double predictedX = lastKalmanTelemetry.predicted_x;
    double predictedY = lastKalmanTelemetry.predicted_y;
    if (!std::isfinite(predictedX)) predictedX = target_x;
    if (!std::isfinite(predictedY)) predictedY = target_y;

    return { predictedX, predictedY };
}

bool MouseThread::sendMovementToDriver(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return false;

    std::lock_guard<std::mutex> lock(inputDevicesMutex);

    if (!mouseInput || !mouseInput->move(dx, dy))
        return false;

    recordMotionCompensationStep(dx, dy);
    return true;
}

void MouseThread::moveRelative(int dx, int dy)
{
    sendMovementToDriver(dx, dy);
}

std::pair<double, double> MouseThread::calc_movement(double tx, double ty)
{
    double offx = tx - center_x;
    double offy = ty - center_y;
    double dist = std::hypot(offx, offy);
    double speed = calculate_speed_multiplier(dist);

    double degPerPxX = fov_x / screen_width;
    double degPerPxY = fov_y / screen_height;

    double mmx = offx * degPerPxX;
    double mmy = offy * degPerPxY;

    double corr = 1.0;
    double fps = static_cast<double>(captureFps.load());
    if (fps > 30.0) corr = 30.0 / fps;

    std::pair<double, double> counts_pair;
    { std::lock_guard<std::mutex> lock(configMutex); counts_pair = config.degToCounts(mmx, mmy, fov_x); }
    double move_x = counts_pair.first * speed * corr;
    double move_y = counts_pair.second * speed * corr;

    return { move_x, move_y };
}

double MouseThread::calculate_speed_multiplier(double distance)
{
    // 速度曲线参数由 applyPendingConfigLocked() 从 PendingConfig 快照写入成员变量，
    // 调用方已持有 input_method_mutex 并调用 applyPendingConfigLocked()，
    // 此处直接使用成员变量，无需重复锁 configMutex。
    const double localMinSpeedMultiplier = profileMinSpeedMultiplier > 0.0
        ? static_cast<double>(profileMinSpeedMultiplier) : min_speed_multiplier;
    const double localMaxSpeedMultiplier = profileMaxSpeedMultiplier > 0.0
        ? static_cast<double>(profileMaxSpeedMultiplier) : max_speed_multiplier;

    const double lowerSpeed = std::min(localMinSpeedMultiplier, localMaxSpeedMultiplier);
    const double upperSpeed = std::max(localMinSpeedMultiplier, localMaxSpeedMultiplier);
    const double safeDistance = std::max(0.0, distance);
    const double safeMaxDistance = std::max(1.0, max_distance);
    const float localSpeedCurveExponent = std::max(0.01f, speedCurveExponent);
    const float localSnapBoostFactor = std::max(0.0f, snapBoostFactor);

    if (nearRadius > 0.0f && safeDistance <= static_cast<double>(nearRadius))
        return std::clamp(lowerSpeed * static_cast<double>(localSnapBoostFactor), lowerSpeed, upperSpeed);

    if (snapRadius > nearRadius && safeDistance < static_cast<double>(snapRadius))
    {
        const double t = std::clamp(
            (safeDistance - static_cast<double>(nearRadius)) / (static_cast<double>(snapRadius) - static_cast<double>(nearRadius)), 0.0, 1.0);
        double curve = 1.0 - std::pow(1.0 - t, static_cast<double>(localSpeedCurveExponent));
        return std::clamp(
            lowerSpeed + (upperSpeed - lowerSpeed) * curve, lowerSpeed, upperSpeed);
    }

    double norm = std::clamp(safeDistance / safeMaxDistance, 0.0, 1.0);
    return std::clamp(
        lowerSpeed + (upperSpeed - lowerSpeed) * norm, lowerSpeed, upperSpeed);
}

bool MouseThread::check_target_in_scope(const AimbotTarget& target, double reduction_factor)
{
    const double center_target_x = target.pivotX;
    const double center_target_y = target.pivotY;

    const double reduced_w = target.w * (reduction_factor / 2.0);
    const double reduced_h = target.h * (reduction_factor / 2.0);

    double x1 = center_target_x - reduced_w;
    double x2 = center_target_x + reduced_w;
    double y1 = center_target_y - reduced_h;
    double y2 = center_target_y + reduced_h;

    return (center_x > x1 && center_x < x2 && center_y > y1 && center_y < y2);
}

void MouseThread::moveMouse(const AimbotTarget& target)
{
    std::lock_guard lg(input_method_mutex);
    // 在持锁状态下应用 UI/热键线程暂存的参数变更，避免跨线程直写本对象状态。
    applyPendingConfigLocked();

    auto predicted = predict_target_position(
        target.x + target.w / 2.0,
        target.y + target.h / 2.0);

    auto mv = calc_movement(predicted.first, predicted.second);
    queueMove(toMoveCount(mv.first), toMoveCount(mv.second));
}

void MouseThread::moveMousePivot(
    double pivotX,
    double pivotY,
    std::chrono::steady_clock::time_point observationTime)
{
    std::lock_guard lg(input_method_mutex);
    // 在持锁状态下应用 UI/热键线程暂存的参数变更，避免跨线程直写本对象状态。
    applyPendingConfigLocked();

    if (observationTime.time_since_epoch().count() != 0)
    {
        auto cameraDelta = getMotionCompensationSince(observationTime);
        pivotX -= cameraDelta.first;
        pivotY -= cameraDelta.second;
    }

    // 保留 AimKalman2D 仅用于调试绘制 (predictFuturePositions overlay)
    auto predicted = predict_target_position(pivotX, pivotY, observationTime);

    // mc_enabled 与 mc_* 调参在 syncMouseController() 的同一个 configMutex
    // 临界区内取得，既消除了原先对 config.mc_enabled 的无锁读取（数据竞争），
    // 也保证「开关 + 参数」来自同一份一致快照。
    const bool mcEnabled = syncMouseController();

    if (mcEnabled)
    {
        // ---- MouseController 移植路径: Kalman 预测 + MinJerk 轨迹 + PID 修正 ----
        double dt = currentFrameIntervalSec();
        if (!std::isfinite(dt) || dt <= 0.0) dt = 0.01;

        // 钉准星到屏幕中心 (游戏准星恒在中心), 统一以"检测分辨率像素"为坐标系
        m_mc.setMousePosition(static_cast<float>(center_x), static_cast<float>(center_y));
        // 原始 pivot 直喂 MC, 内部 Kalman 自行预测 (对应"提前预测身位")
        m_mc.update(static_cast<float>(pivotX), static_cast<float>(pivotY), static_cast<float>(dt));

        float mvx = 0.0f, mvy = 0.0f;
        m_mc.getMoveDelta(mvx, mvy);

        // 换回计数 (复用 degToCounts 换算链, 保持手感一致)
        double degX = static_cast<double>(mvx) * (fov_x / screen_width);
        double degY = static_cast<double>(mvy) * (fov_y / screen_height);
        std::pair<double, double> counts;
        {
            std::lock_guard<std::mutex> lock(configMutex);
            counts = config.degToCounts(degX, degY, fov_x);
        }
        int mx = toMoveCount(std::round(counts.first));
        int my = toMoveCount(std::round(counts.second));

        if (mx == 0 && my == 0)
            return;

        if (curve_enabled)
            mouseCurveMoveRelative(mx, my);
        else
            queueMove(mx, my);
        return;
    }

    auto mv = calc_movement(predicted.first, predicted.second);
    int mx = toMoveCount(mv.first);
    int my = toMoveCount(mv.second);

    if (mx == 0 && my == 0)
    {
        return;
    }

    if (curve_enabled)
    {
        mouseCurveMoveRelative(mx, my);
    }
    else
    {
        queueMove(mx, my);
    }
}

void MouseThread::invalidateQueuedMoves()
{
    moveGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void MouseThread::clearQueuedMoves()
{
    invalidateQueuedMoves();
    std::lock_guard<std::mutex> lock(queueMtx);
    std::queue<Move> empty;
    moveQueue.swap(empty);
    resetCurveState();
}

void MouseThread::pressMouse(const AimbotTarget& target)
{
    float localScopeMultiplier = bScope_multiplier;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        const int activeSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
        if (activeSlot >= 0 && activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS))
        {
            const auto& profile = config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)];
            localScopeMultiplier = profile.localFloat("bScope_multiplier", localScopeMultiplier);
        }
    }
    const bool bScope = check_target_in_scope(target, localScopeMultiplier);
    if (bScope && !mouse_pressed)
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        if (!mouseInput)
        {
            mouse_pressed = false;
            return;
        }

        if (!mouseInput->isOpen())
        {
            mouse_pressed = false;
            return;
        }

        if (mouseInput->leftDown())
            mouse_pressed = true;
        else if (!mouseInput->isOpen())
            mouse_pressed = false;
    }
    // 注意：bScope 判定失败时不应在此处 leftUp，否则会与 trigger 状态机
    // 的 triggerShouldFire 判定形成高频 press/release 震荡。
    // releaseMouse() 统一负责释放逻辑。
}

void MouseThread::releaseMouse()
{
    if (mouse_pressed)
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        if (!mouseInput)
        {
            mouse_pressed = false;
            return;
        }

        if (!mouseInput->isOpen())
        {
            mouse_pressed = false;
            return;
        }

        // 同上：trigger 判定"不该开火"而调用本函数时，若射击键正被物理按住，
        // 不发 leftUp，避免软件抬起压制物理按下（手动射击被强制松开）。
        if (shooting.load())
            return;

        // 无论 leftUp 是否成功，只要已尝试释放就清本地状态。
        // 若 UDP 偶发丢 ACK 但盒子实际已抬起，继续保留 true 会让上层认为仍按住，
        // 直到下一次触发才补发，表现成软件左键卡住/回补幽灵按下。
        mouse_pressed = false;
    }
}

void MouseThread::resetPrediction()
{
    clearQueuedMoves();
    prev_time = std::chrono::steady_clock::time_point();
    prev_x = 0;
    prev_y = 0;
    prev_velocity_x = 0;
    prev_velocity_y = 0;
    targetKalman.reset();
    lastKalmanTelemetry = {};
    lastPredictionLookaheadSec = 0.0;
    lastDetectionDelaySec = 0.0;
    target_detected.store(false);
    m_mc.reset(); // 重置 MouseController 内部滤波/轨迹状态
}

void MouseThread::checkAndResetPredictions()
{
    auto current_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(current_time - last_target_time).count();
    double resetTimeoutSec = 0.5;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        resetTimeoutSec = static_cast<double>(config.kalman_reset_timeout_sec);
    }
    const double timeoutSec = std::clamp(resetTimeoutSec, 0.05, 3.0);

    if (elapsed > timeoutSec && target_detected.load())
    {
        resetPrediction();
    }
}

std::vector<std::pair<double, double>> MouseThread::predictFuturePositions(double pivotX, double pivotY, int frames)
{
    std::vector<std::pair<double, double>> result;
    if (frames <= 0)
        return result;

    result.reserve(frames);

    const double frame_time = currentFrameIntervalSec();

    targetKalman.setSettings(buildKalmanSettingsFromConfig());
    if (targetKalman.initialized())
    {
        const double detectionDelaySec = (lastDetectionDelaySec > 0.0)
            ? lastDetectionDelaySec
            : currentDetectionDelaySec();
        const double baseLookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
        for (int i = 1; i <= frames; ++i)
        {
            const double t = baseLookaheadSec + frame_time * i;
            auto predicted = targetKalman.predict(t);
            if (!std::isfinite(predicted.first) || !std::isfinite(predicted.second))
                continue;
            result.push_back(predicted);
        }

        if (!result.empty())
            return result;
    }

    auto current_time = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(current_time - prev_time).count();

    if (prev_time.time_since_epoch().count() == 0 || dt > 0.5)
    {
        return result;
    }

    double vx = prev_velocity_x;
    double vy = prev_velocity_y;
    
    for (int i = 1; i <= frames; i++)
    {
        double t = frame_time * i;
        double px = pivotX + vx * t;
        double py = pivotY + vy * t;
        result.push_back({ px, py });
    }

    return result;
}

void MouseThread::storeFuturePositions(const std::vector<std::pair<double, double>>& positions)
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex);
    futurePositions = positions;
}

void MouseThread::clearFuturePositions()
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex);
    futurePositions.clear();
}

std::vector<std::pair<double, double>> MouseThread::getFuturePositions()
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex);
    return futurePositions;
}

void MouseThread::clearCurveDebugTrail()
{
    std::lock_guard<std::mutex> lock(curveDebugTrailMutex);
    curveDebugTrail.clear();
    curveDebugCursorX = center_x;
    curveDebugCursorY = center_y;
}

std::vector<std::pair<double, double>> MouseThread::getCurveDebugTrail()
{
    std::lock_guard<std::mutex> lock(curveDebugTrailMutex);
    const auto now = std::chrono::steady_clock::now();
    pruneCurveDebugTrailLocked(now);

    std::vector<std::pair<double, double>> out;
    out.reserve(curveDebugTrail.size());
    for (const auto& p : curveDebugTrail)
        out.emplace_back(p.x, p.y);
    return out;
}

void MouseThread::setMouseInput(IMouseInput* newMouseInput)
{
    // Caller owns inputDevicesMutex when replacing the shared device; keep this
    // method lock-free to preserve one lock order and avoid recursive locking.
    mouseInput = newMouseInput;
}
