#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>

#include "capture.h"
#include "mouse.h"
#include "mybot.h"
#include "runtime/thread_loops.h"
#include "runtime/trigger_system.h"

namespace
{
constexpr int kPredictedOnlyMoveGraceFrames = 3;
constexpr double kPredictedOnlyMoveGraceSec =
    static_cast<double>(kPredictedOnlyMoveGraceFrames) / 60.0;
constexpr int kPredictedOnlyMoveStalePadMs = 16;

double trackerFrameIntervalSec(int captureFpsValue)
{
    const double fps = std::clamp(
        static_cast<double>((captureFpsValue > 0) ? captureFpsValue : 60),
        15.0,
        500.0);
    return 1.0 / fps;
}

bool allowPredictedOnlyMove(
    int activeTrackId,
    bool hasActiveTarget,
    const LockedTargetInfo& lockInfo,
    int captureFpsValue)
{
    if (activeTrackId != lockInfo.trackId ||
        !hasActiveTarget ||
        lockInfo.missedFrames <= 0)
    {
        return false;
    }

    const double frameDtSec = trackerFrameIntervalSec(captureFpsValue);
    const double missedSec = static_cast<double>(lockInfo.missedFrames) * frameDtSec;
    return missedSec <= kPredictedOnlyMoveGraceSec + frameDtSec * 0.51;
}

int trackerStaleTimeoutMs(int captureFpsValue)
{
    const int fps = std::max(1, captureFpsValue);
    const int frameBasedMs = 2000 / fps;
    const int graceBasedMs =
        static_cast<int>(kPredictedOnlyMoveGraceSec * 1000.0 + 0.5) +
        kPredictedOnlyMoveStalePadMs;
    return std::clamp(std::max(frameBasedMs, graceBasedMs), 25, 180);
}
}

void createInputDevices();
void assignInputDevices();
void handleEasyNoRecoil(MouseThread& mouseThread)
{
    bool easyNoRecoil = false;
    int recoil_compensation = 0;
    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        const int activeSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
        const Config::MouseHotkey* profile =
            activeSlot >= 0 && activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS)
                ? &config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)] : nullptr;
        easyNoRecoil = profile ? profile->localBool("easynorecoil", config.easynorecoil) : config.easynorecoil;
        const float strength = profile
            ? profile->localFloat("easynorecoilstrength", config.easynorecoilstrength)
            : config.easynorecoilstrength;
        recoil_compensation = static_cast<int>(strength);
    }

    if (easyNoRecoil && shooting.load() && zooming.load())
    {
        mouseThread.moveRelative(0, recoil_compensation);
    }
}

void mouseThreadFunction(MouseThread& mouseThread)
{
    // 与 DetectionBuffer::version 保持同类型（uint64_t，见 detection_buffer.h 注释）。
    // 初值取 0：version 亦从 0 起，首次 set()/clear() 后变为 1，
    // `1 > 0` 成立，首帧不会被漏取；语义与原先的 int/-1 组合完全等价。
    std::uint64_t lastVersion = 0;
    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    std::chrono::steady_clock::time_point detectionTimestamp{};
    MultiTargetTracker targetTracker;
    std::optional<AimbotTarget> activeTarget;
    int activeTrackId = -1;
    bool activeTargetObserved = false;
    bool wasAiming = false;
    int appliedDetectionResolution = -1;
    bool appliedTrackerEnabled = true;
    auto lastTrackerUpdate = std::chrono::steady_clock::time_point::min();

    // Trigger system
    TriggerSystem triggerSystem;
    bool prevTriggerMasterEnabled = false;
    int prevActiveSlot = -1;

    // Dynamic Range state
    double currentEffectiveFov = -1.0; // -1 = not initialized
    bool dynamicRangeShrinking = false;
    bool dynamicRangeRestoring = false;
    std::chrono::steady_clock::time_point dynamicRangeStateStarted{};
    std::chrono::steady_clock::time_point dynamicRangeStateUntil{};
    std::chrono::steady_clock::time_point dynamicRangeCooldownUntil{};
    bool prevDynamicRangeEnabled = false;
    std::string prevDynamicTargetClasses;
    int prevDynamicShrinkScope = 320;
    int prevDynamicShrinkDurationMs = 300;
    int prevDynamicRestoreDurationMs = 300;
    std::string prevTargetingMode;
    bool classEnabledSnapshot[Config::MAX_CLASSES]{};
    auto lastKmboxNetReconnectAttempt = std::chrono::steady_clock::time_point::min();

    auto resetActiveTarget = [&]() {
        activeTarget.reset();
        activeTrackId = -1;
        activeTargetObserved = false;
        mouseThread.clearFuturePositions();
        mouseThread.resetPrediction();
    };

    auto resetDynamicRangeState = [&]() {
        dynamicRangeShrinking = false;
        dynamicRangeRestoring = false;
        dynamicRangeStateStarted = {};
        dynamicRangeStateUntil = {};
        dynamicRangeCooldownUntil = {};
        currentEffectiveFov = -1.0;
        targetTracker.setEffectiveFov(-1.0);
        g_dynamicEffectiveFov.store(-1.0);
    };

    while (!shouldExit)
    {
        bool hasNewDetection = false;
        bool hasAimObservation = false;
        int detectionResolution = 0;
        int detectionWidth = 0;
        int detectionHeight = 0;
        bool trackerEnabled = true;
        int predictionFuturePositions = 0;
        bool autoShoot = false;
        TriggerConfig targetingConfig;
        TriggerConfig shootConfig;
        TriggerConfig zoomConfig;
        Config::MouseHotkey triggerProfile;
        bool hasTriggerProfile = false;

        {
            std::lock_guard<std::mutex> cfgLock(configMutex);
            detectionResolution = config.detection_resolution;
            detectionWidth = config.detection_resolution;
            detectionHeight = config.detection_resolution;
            trackerEnabled = config.tracker_enabled;
            predictionFuturePositions = config.prediction_futurePositions;
            autoShoot = config.auto_shoot;

            const int activeSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
            if (activeSlot != prevActiveSlot)
            {
                prevActiveSlot = activeSlot;
                triggerSystem.resetAll();
            }
            const Config::MouseHotkey* profile =
                activeSlot >= 0 && activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS)
                    ? &config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)] : nullptr;
            if (profile)
            {
                trackerEnabled = profile->localBool("tracker_enabled", trackerEnabled);
                predictionFuturePositions = std::clamp(
                    profile->localInt("prediction_futurePositions", predictionFuturePositions), 1, 200);
                autoShoot = config.auto_shoot && profile->localBool("trigger_enabled_for_hotkey", true);
            }

            // Update trigger system config
            if (prevTriggerMasterEnabled != config.auto_shoot)
            {
                prevTriggerMasterEnabled = config.auto_shoot;
                triggerSystem.resetAll();
            }
            if (config.auto_shoot)
            {
                targetingConfig = config.trigger_targeting;
                shootConfig = config.trigger_shoot;
                zoomConfig = config.trigger_zoom;
                if (activeSlot >= 0 && activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS))
                {
                    const auto& profile = config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)];
                    triggerProfile = profile;
                    hasTriggerProfile = true;
                    targetingConfig.enabled = profile.localBool("trigger_enabled", targetingConfig.enabled) &&
                                              profile.localBool("trigger_enabled_for_hotkey", true);
                    targetingConfig.continuous = profile.localBool("trigger_continuous", targetingConfig.continuous);
                    targetingConfig.stop_fire_on_loss = profile.localBool("trigger_stop_fire_on_loss", targetingConfig.stop_fire_on_loss);
                    // 钳制区间与 config.cpp loadConfig 的 TriggerConfig clamp 完全对齐。
                    // localInt 用 std::stoi 解析, config.ini 手改负值可绕过 UI 校验注入,
                    // 其中 fire_duration_random_ms / cooldown_random_ms 为负会令
                    // std::uniform_int_distribution<int>(0, 负值) 触发未定义行为。
                    targetingConfig.stop_fire_delay_ms = std::clamp(
                        profile.localInt("trigger_targeting_stop_fire_delay_ms", targetingConfig.stop_fire_delay_ms), 0, 5000);
                    targetingConfig.key_delay_ms = std::clamp(
                        profile.localInt("trigger_targeting_key_delay_ms", targetingConfig.key_delay_ms), 0, 5000);
                    targetingConfig.pre_fire_delay_ms = std::clamp(
                        profile.localInt("trigger_targeting_pre_fire_delay_ms", targetingConfig.pre_fire_delay_ms), 0, 5000);
                    targetingConfig.fire_duration_ms = std::clamp(
                        profile.localInt("trigger_targeting_fire_duration_ms", targetingConfig.fire_duration_ms), 1, 10000);
                    targetingConfig.fire_duration_random_ms = std::clamp(
                        profile.localInt("trigger_targeting_fire_duration_random_ms", targetingConfig.fire_duration_random_ms), 0, 10000);
                    targetingConfig.cooldown_ms = std::clamp(
                        profile.localInt("trigger_targeting_cooldown_ms", targetingConfig.cooldown_ms), 0, 10000);
                    targetingConfig.cooldown_random_ms = std::clamp(
                        profile.localInt("trigger_targeting_cooldown_random_ms", targetingConfig.cooldown_random_ms), 0, 10000);
                }
            }

            const int activeClassSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
            int classOrderSnapshot[Config::MAX_CLASSES]{};
            NormalizedAimOffset aimOffsetSnapshot[Config::MAX_CLASSES];
            // 复用常量键名表，避免每帧为 15 个类别现拼接配置键字符串（超 SSO 必然堆分配）。
            const TargetClassConfigKeys& classKeys = targetClassConfigKeys();
            for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
            {
                classEnabledSnapshot[cls] = config.isClassEnabled(cls);
                classOrderSnapshot[cls] = cls;
            }
            if (activeClassSlot >= 0 && activeClassSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS))
            {
                // 命名为 classProfile 以避免遮蔽外层同名的 profile 指针。
                const auto& classProfile = config.mouse_hotkeys[static_cast<std::size_t>(activeClassSlot)];
                for (int cls = 0; cls < Config::FIXED_TARGET_CLASS_COUNT; ++cls)
                {
                    classEnabledSnapshot[cls] = classEnabledSnapshot[cls] &&
                        classProfile.localBool(classKeys.enabled[cls], false);
                    classOrderSnapshot[cls] = classProfile.localInt(classKeys.order[cls], cls);
                    aimOffsetSnapshot[cls].x = classProfile.localFloat(classKeys.aimOffsetX[cls], 0.5f);
                    aimOffsetSnapshot[cls].y = classProfile.localFloat(classKeys.aimOffsetY[cls], 0.5f);
                }
            }
            targetTracker.setClassConfig(classEnabledSnapshot, classOrderSnapshot, Config::FIXED_TARGET_CLASS_COUNT);
            // 在本临界区内一并注入瞄准点偏移，使 MultiTargetTracker::update()
            // 不再需要第二次获取 configMutex。
            targetTracker.setAimOffsets(aimOffsetSnapshot, Config::FIXED_TARGET_CLASS_COUNT);

            // Update dynamic range config if changed
            const bool localDynamicRangeEnabled = profile
                ? profile->localBool("dynamic_range_enabled", config.dynamic_range_enabled)
                : config.dynamic_range_enabled;
            const int localDynamicShrinkScope = profile
                ? profile->localInt("dynamic_range_shrink_scope", config.dynamic_range_shrink_scope)
                : config.dynamic_range_shrink_scope;
            const int localDynamicShrinkDurationMs = profile
                ? std::clamp(profile->localInt("dynamic_range_shrink_duration_ms", config.dynamic_range_shrink_duration_ms), 50, 2000)
                : config.dynamic_range_shrink_duration_ms;
            const int localDynamicRestoreDurationMs = profile
                ? std::clamp(profile->localInt("dynamic_range_restore_duration_ms", config.dynamic_range_cooldown_ms), 50, 2000)
                : config.dynamic_range_cooldown_ms;
            if (prevDynamicRangeEnabled != localDynamicRangeEnabled ||
                prevDynamicTargetClasses != config.dynamic_range_target_classes ||
                prevDynamicShrinkScope != localDynamicShrinkScope ||
                prevDynamicShrinkDurationMs != localDynamicShrinkDurationMs ||
                prevDynamicRestoreDurationMs != localDynamicRestoreDurationMs)
            {
                prevDynamicRangeEnabled = localDynamicRangeEnabled;
                prevDynamicTargetClasses = config.dynamic_range_target_classes;
                prevDynamicShrinkScope = localDynamicShrinkScope;
                prevDynamicShrinkDurationMs = localDynamicShrinkDurationMs;
                prevDynamicRestoreDurationMs = localDynamicRestoreDurationMs;

                targetTracker.setDynamicRangeConfig(
                    localDynamicRangeEnabled,
                    localDynamicShrinkScope,
                    localDynamicShrinkDurationMs,
                    localDynamicRestoreDurationMs,
                    config.dynamic_range_target_classes
                );
                if (!localDynamicRangeEnabled)
                {
                    resetDynamicRangeState();
                }
            }

            // Update targeting mode if changed
            const std::string localTargetingMode = profile
                ? profile->localString("targeting_mode", config.targeting_mode)
                : config.targeting_mode;
            if (prevTargetingMode != localTargetingMode)
            {
                prevTargetingMode = localTargetingMode;
                targetTracker.setTargetingMode(localTargetingMode);
            }
        }

        const bool aimingNow = aiming.load();
        if (aimingNow != wasAiming)
        {
            resetActiveTarget();
            wasAiming = aimingNow;
        }

        {
            std::unique_lock<std::mutex> lock(detectionBuffer.mutex);
            detectionBuffer.cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return detectionBuffer.version > lastVersion || shouldExit;
                }
            );

            if (shouldExit) break;

            if (detectionBuffer.version > lastVersion)
            {
                boxes = detectionBuffer.boxes;
                classes = detectionBuffer.classes;
                detectionTimestamp = detectionBuffer.frameTimestamp;
                lastVersion = detectionBuffer.version;
                hasNewDetection = true;
            }
        }

        if (input_method_changed.exchange(false))
        {
            createInputDevices();
            assignInputDevices();
            lastKmboxNetReconnectAttempt = std::chrono::steady_clock::now();
        }

        // kmboxNet 是异步建连：首次 createInputDevices 即使连接失败也会立即返回，
        // 若 SDK 命令失败把设备标记为未连接，需要由鼠标线程按冷却节奏重建输入设备，
        // 避免 UI 一直显示已连接、实际鼠标输入静默失效。
        {
            const auto kmboxNow = std::chrono::steady_clock::now();
            const bool cooldownPassed =
                lastKmboxNetReconnectAttempt == std::chrono::steady_clock::time_point::min() ||
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    kmboxNow - lastKmboxNetReconnectAttempt).count() >= 1000;
            if (cooldownPassed)
            {
                bool isKmboxNet = false;
                {
                    std::lock_guard<std::mutex> cfgLock(configMutex);
                    const auto method = ParseMouseInputMethod(config.input_method);
                    isKmboxNet = method.has_value() && *method == MouseInputMethod::KmboxNet;
                }
                if (isKmboxNet)
                {
                    std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
                    if (activeMouseInputOwner &&
                        !activeMouseInputOwner->isOpen() &&
                        !activeMouseInputOwner->isConnecting())
                    {
                        lastKmboxNetReconnectAttempt = kmboxNow;
                        input_method_changed.store(true);
                    }
                }
            }
        }

        // UI / 热键线程通过 MouseThread::updateConfig() 暂存的参数变更，
        // 必须由本线程在持有 input_method_mutex 时应用（详见 mouse.cpp 注释）。
        // moveMouse/moveMousePivot 内部也会应用一次；此处覆盖「无目标空闲」场景，
        // 保证没有瞄准动作时参数改动同样即时生效。
        mouseThread.applyPendingConfig();

        if (detection_resolution_changed.load() || detectionResolution != appliedDetectionResolution)
        {
            {
                std::lock_guard<std::mutex> cfgLock(configMutex);
                appliedDetectionResolution = config.detection_resolution;
                mouseThread.updateConfig(
                    config.detection_resolution,
                    config.fovX,
                    config.fovY,
                    config.minSpeedMultiplier,
                    config.maxSpeedMultiplier,
                    config.predictionInterval,
                    config.auto_shoot,
                    config.bScope_multiplier
                );
            }
            resetDynamicRangeState();
            targetTracker.reset();
            {
                std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                g_trackerDebugTracks.clear();
                g_trackerLockedId = -1;
            }
            resetActiveTarget();
        }

        if (trackerEnabled != appliedTrackerEnabled)
        {
            appliedTrackerEnabled = trackerEnabled;
            resetDynamicRangeState();
            targetTracker.reset();
            {
                std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                g_trackerDebugTracks.clear();
                g_trackerLockedId = -1;
            }
            resetActiveTarget();
        }

        if (hasNewDetection)
        {
            if (trackerEnabled)
            {
                // ── Dynamic Range: compute effective FOV ──
                if (prevDynamicRangeEnabled)
                {
                    int originalFovX = 121;
                    int shrinkScope = 320;
                    int shrinkDurationMs = prevDynamicShrinkDurationMs;
                    int restoreDurationMs = prevDynamicRestoreDurationMs;
                    {
                        std::lock_guard<std::mutex> cfgLock(configMutex);
                        originalFovX = config.fovX;
                        shrinkScope = prevDynamicShrinkScope;
                    }

                    int validTargetCount = 0;
                    for (std::size_t i = 0; i < boxes.size() && i < classes.size(); ++i)
                    {
                        if (boxes[i].width > 0 && boxes[i].height > 0 &&
                classes[i] >= 0 && classes[i] < Config::FIXED_TARGET_CLASS_COUNT &&
                            classEnabledSnapshot[classes[i]])
                            ++validTargetCount;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    if (validTargetCount >= 2 && !dynamicRangeShrinking &&
                        !dynamicRangeRestoring && now >= dynamicRangeCooldownUntil)
                    {
                        dynamicRangeShrinking = true;
                        dynamicRangeRestoring = false;
                        dynamicRangeStateStarted = now;
                        dynamicRangeStateUntil = now + std::chrono::milliseconds(shrinkDurationMs);
                    }
                    if (dynamicRangeShrinking && now >= dynamicRangeStateUntil)
                    {
                        dynamicRangeShrinking = false;
                        dynamicRangeRestoring = true;
                        dynamicRangeStateStarted = now;
                        dynamicRangeStateUntil = now + std::chrono::milliseconds(restoreDurationMs);
                    }

                    const double shrunkFov = originalFovX *
                        (static_cast<double>(shrinkScope) / detectionResolution);
                    if (dynamicRangeShrinking)
                    {
                        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - dynamicRangeStateStarted).count();
                        const double progress = std::clamp(
                            static_cast<double>(elapsed) / shrinkDurationMs, 0.0, 1.0);
                        currentEffectiveFov = originalFovX + (shrunkFov - originalFovX) * progress;
                    }
                    else if (dynamicRangeRestoring)
                    {
                        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - dynamicRangeStateStarted).count();
                        const double progress = std::clamp(
                            static_cast<double>(elapsed) / restoreDurationMs, 0.0, 1.0);
                        currentEffectiveFov = shrunkFov + (originalFovX - shrunkFov) * progress;
                        if (progress >= 1.0)
                        {
                            dynamicRangeRestoring = false;
                            dynamicRangeCooldownUntil = now;
                            currentEffectiveFov = -1.0;
                        }
                    }
                    else
                    {
                        currentEffectiveFov = -1.0;
                    }

                    // 同时下发基准 FOV：tracker 需要 effectiveFov/originalFovX 这一
                    // 收缩比例才能让检测过滤半径跟随动画逐帧插值，
                    // 与 Overlay 的收缩圈（同样按该比例缩放绘制半径）保持同步。
                    targetTracker.setEffectiveFov(
                        currentEffectiveFov, static_cast<double>(originalFovX));
                    g_dynamicEffectiveFov.store(currentEffectiveFov);
                }
                else if (currentEffectiveFov >= 0.0)
                {
                    currentEffectiveFov = -1.0;
                    targetTracker.setEffectiveFov(-1.0);
                    g_dynamicEffectiveFov.store(-1.0);
                }
                if (!prevDynamicRangeEnabled)
                {
                    dynamicRangeShrinking = false;
                    dynamicRangeRestoring = false;
                    dynamicRangeStateStarted = {};
                    dynamicRangeStateUntil = {};
                    dynamicRangeCooldownUntil = {};
                }

                targetTracker.update(
                    boxes,
                    classes,
                    detectionResolution,
                    detectionResolution,
                    aimingNow,
                    detectionTimestamp
                );
                lastTrackerUpdate = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    // 优化：用 swap 替代赋值，避免 getDebugTracks() 返回的临时
                    // vector 再被整体拷贝进全局容器（每帧一次堆分配 + 复制）。
                    auto tracks = targetTracker.getDebugTracks();
                    g_trackerDebugTracks.swap(tracks);
                    g_trackerLockedId = targetTracker.getLockedTrackId();
                }

                LockedTargetInfo lockInfo;
                if (targetTracker.getLockedTarget(lockInfo))
                {
                    const int previousActiveTrackId = activeTrackId;
                    const bool hadActiveTarget = activeTarget.has_value();
                    if (activeTrackId != -1 && activeTrackId != lockInfo.trackId)
                    {
                        mouseThread.resetPrediction();
                        mouseThread.clearFuturePositions();
                    }

                    activeTarget = lockInfo.target;
                    activeTrackId = lockInfo.trackId;
                    activeTargetObserved = lockInfo.observedThisFrame;
                    mouseThread.setTargetDetected(true);

                    if (lockInfo.observedThisFrame)
                    {
                        hasAimObservation = true;
                        mouseThread.setLastTargetTime(std::chrono::steady_clock::now());

                        auto futurePositions = mouseThread.predictFuturePositions(
                            activeTarget->pivotX,
                            activeTarget->pivotY,
                            predictionFuturePositions
                        );
                        mouseThread.storeFuturePositions(futurePositions);
                    }
                    else if (allowPredictedOnlyMove(
                        previousActiveTrackId,
                        hadActiveTarget,
                        lockInfo,
                        captureFps.load()))
                    {
                        hasAimObservation = true;

                        auto futurePositions = mouseThread.predictFuturePositions(
                            activeTarget->pivotX,
                            activeTarget->pivotY,
                            predictionFuturePositions
                        );
                        mouseThread.storeFuturePositions(futurePositions);
                    }
                }
                else
                {
                    resetActiveTarget();
                }
            }
            else
            {
                targetTracker.reset();
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    g_trackerDebugTracks.clear();
                    g_trackerLockedId = -1;
                }

                std::unique_ptr<AimbotTarget> selected(
                    sortTargets(
                        boxes,
                        classes,
                        detectionResolution,
                        detectionResolution));
                lastTrackerUpdate = std::chrono::steady_clock::now();

                if (selected)
                {
                    activeTarget = *selected;
                    activeTrackId = -1;
                    activeTargetObserved = true;
                    hasAimObservation = true;
                    mouseThread.setTargetDetected(true);
                    mouseThread.setLastTargetTime(std::chrono::steady_clock::now());

                    auto futurePositions = mouseThread.predictFuturePositions(
                        activeTarget->pivotX,
                        activeTarget->pivotY,
                        predictionFuturePositions
                    );
                    mouseThread.storeFuturePositions(futurePositions);
                }
                else
                {
                    resetActiveTarget();
                }
            }
        }

        if (activeTarget)
        {
            const int staleMs = trackerStaleTimeoutMs(captureFps.load());
            if (std::chrono::steady_clock::now() - lastTrackerUpdate > std::chrono::milliseconds(staleMs))
            {
                resetActiveTarget();
            }
        }

        // Build keysHeld bitmask for trigger system
        // bit 0 = targeting, bit 1 = shoot, bit 2 = zoom
        int keysHeld = 0;
        if (aimingNow)  keysHeld |= (1 << 0);
        if (shooting.load()) keysHeld |= (1 << 1);
        if (zooming.load())  keysHeld |= (1 << 2);

        // Build active target box [left, top, width, height]
        float targetBox[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float* pTargetBox = nullptr;
        if (activeTarget && activeTargetObserved)
        {
            targetBox[0] = static_cast<float>(activeTarget->x);
            targetBox[1] = static_cast<float>(activeTarget->y);
            targetBox[2] = static_cast<float>(activeTarget->w);
            targetBox[3] = static_cast<float>(activeTarget->h);
            pTargetBox = targetBox;
        }

        // Run trigger state machine
        bool triggerShouldFire = false;
        if (autoShoot)
        {
            TriggerConfig activeTargetTriggerConfig = targetingConfig;
            if (activeTarget &&
                activeTarget->classId >= 0 &&
                activeTarget->classId < Config::FIXED_TARGET_CLASS_COUNT &&
                hasTriggerProfile)
            {
                const TargetClassConfigKeys& classKeys = targetClassConfigKeys();
                const int cls = activeTarget->classId;
                activeTargetTriggerConfig.zone_offset_x = std::clamp(
                    triggerProfile.localFloat(classKeys.triggerZoneOffsetX[cls], 0.1f), 0.0f, 1.0f);
                activeTargetTriggerConfig.zone_offset_y = std::clamp(
                    triggerProfile.localFloat(classKeys.triggerZoneOffsetY[cls], 0.1f), 0.0f, 1.0f);
                activeTargetTriggerConfig.zone_size_x = std::clamp(
                    triggerProfile.localFloat(classKeys.triggerZoneSizeX[cls], 0.8f), 0.01f, 1.0f);
                activeTargetTriggerConfig.zone_size_y = std::clamp(
                    triggerProfile.localFloat(classKeys.triggerZoneSizeY[cls], 0.8f), 0.01f, 1.0f);
            }
            triggerSystem.updateConfig(
                activeTargetTriggerConfig,
                shootConfig,
                zoomConfig);
            triggerShouldFire = triggerSystem.update(
                pTargetBox, keysHeld, detectionWidth, detectionHeight);
        }

        // ── Mouse movement (targeting only) ──
        if (aimingNow)
        {
            if (activeTarget && hasAimObservation)
            {
                mouseThread.moveMousePivot(activeTarget->pivotX, activeTarget->pivotY, detectionTimestamp);
            }
            else
            {
                if (!activeTarget || !activeTargetObserved)
                {
                    mouseThread.clearQueuedMoves();
                }
            }
        }
        else
        {
            mouseThread.clearQueuedMoves();
        }

        // ── Trigger fire control ──
        if (autoShoot)
        {
            // triggerShouldFire 由 trigger 状态机决定。但若当前无锁定目标
            // （activeTarget 为 nullopt），即便状态机处于 Firing 也不应继续按住：
            // ①无目标自然没有 press 的对象；②若上一帧已 press，本帧不 release 则
            //   软件按下状态会卡滞到 triggerShouldFire 翻回 false 才释放，期间硬件
            //   按键持续发出，表现为"目标消失后还在开火"。
            //   根因：状态机在 stop_fire_on_loss=false 时，pTargetBox=nullptr 使
            //   calculateTriggerZone 直接 return false → inZone=false，但 Firing
            //   分支不因 inZone=false 退出，update 仍返回 true。
            //   修复：triggerShouldFire && !activeTarget 视为应释放，与目标丢失语义一致。
            if (triggerShouldFire && activeTarget.has_value())
            {
                mouseThread.pressMouse(*activeTarget);
            }
            else
            {
                mouseThread.releaseMouse();
            }
        }
        else
        {
            mouseThread.releaseMouse();
        }

        handleEasyNoRecoil(mouseThread);

        mouseThread.checkAndResetPredictions();
    }
}
