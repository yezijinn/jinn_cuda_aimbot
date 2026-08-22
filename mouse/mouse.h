#ifndef MOUSE_H
#define MOUSE_H

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <mutex>
#include <atomic>
#include <chrono>
#include <utility>
#include <cstdint>
#include <vector>
#include <queue>
#include <thread>
#include <condition_variable>
#include <deque>
#include <optional>
#include <random>

#include "AimbotTarget.h"
#include "MouseInput.h"
#include "aim_kalman.h"
#include "algo/MouseController.h"

class MouseThread
{
private:
    double screen_width;
    double screen_height;
    double prediction_interval;
    double fov_x;
    double fov_y;
    double max_distance;
    double min_speed_multiplier;
    double max_speed_multiplier;
    double center_x;
    double center_y;
    bool   auto_shoot;
    float  bScope_multiplier;

    double prev_x, prev_y;
    double prev_velocity_x, prev_velocity_y;
    std::chrono::time_point<std::chrono::steady_clock> prev_time;
    std::chrono::steady_clock::time_point last_target_time;
    std::atomic<bool> target_detected{ false };
    std::atomic<bool> mouse_pressed{ false };

    IMouseInput* mouseInput;

    bool sendMovementToDriver(int dx, int dy);

    struct Move
    {
        int dx;
        int dy;
        std::uint64_t generation;
        std::chrono::steady_clock::time_point queuedAt;
    };

    std::queue<Move>              moveQueue;
    std::mutex                    queueMtx;
    std::atomic<std::uint64_t>    moveGeneration{0};
    std::condition_variable       queueCv;
    const size_t                  queueLimit = 5;
    static constexpr std::chrono::milliseconds maxQueuedMoveAge{ 40 };
    std::thread                   moveWorker;
    std::atomic<bool>             workerStop{ false };

    std::vector<std::pair<double, double>> futurePositions;
    std::mutex                    futurePositionsMutex;
    aim::AimKalman2D              targetKalman;
    aim::AimKalmanTelemetry       lastKalmanTelemetry;

    // MouseController 移植模块 (Kalman 预测 + MinJerk 轨迹 + PID 修正)
    MouseController               m_mc;
    bool                          mc_enabled_ = false;
    double                        lastPredictionLookaheadSec = 0.0;
    double                        lastDetectionDelaySec = 0.0;

    void moveWorkerLoop();
    void queueMove(int dx, int dy);

    // 将 config 中的 mc_* 开关同步到 m_mc (阶段开关 / 前瞻 / 时长)。
    // 内部自行获取 configMutex（锁序: input_method_mutex -> configMutex，
    // 与 calc_movement / calculate_speed_multiplier 一致），
    // 返回同一临界区内读到的 config.mc_enabled 快照，避免调用方再次无锁读取。
    bool syncMouseController();

    // ---- 参数热更新暂存区 ----
    // updateConfig() 可能来自 overlay 渲染线程 / keyboard 监听线程 / 鼠标线程，
    // 而其写入的几何量、曲线状态、targetKalman、m_mc 均由鼠标线程在
    // input_method_mutex 保护下于热路径读写。调用方进入 updateConfig 时已持有
    // configMutex，若在此再获取 input_method_mutex 会与鼠标线程
    // (input_method_mutex -> configMutex) 形成 ABBA 死锁。
    // 因此改为「暂存快照 + 鼠标线程择机应用」。
    struct PendingConfig
    {
        int    resolution = 0;
        int    fovX = 0;
        int    fovY = 0;
        double minSpeedMultiplier = 0.0;
        double maxSpeedMultiplier = 0.0;
        double predictionInterval = 0.0;
        bool   autoShoot = false;
        float  bScopeMultiplier = 0.0f;
        bool   curveEnabled = true;
        double curveIntensity = 0.0;
        double perturbationStrength = 0.0;
        double curveMaxSpeed = 0.0;
        double curveDistance = 0.0;
        aim::AimKalmanSettings kalmanSettings{};
        // MouseController 调参快照 (对应 syncMouseController 的 configMutex 临界区)
        bool   mcEnabled = false;
        float  mcMaxStep = 0.0f;
        float  mcRetarget = 0.0f;
        float  mcAheadMin = 0.0f, mcAheadMax = 0.0f;
        float  mcDurMin = 0.0f, mcDurMax = 0.0f;
        float  mcKalmanQ = 0.0f, mcKalmanR = 0.0f;
        float  mcXTracking = 0.0f, mcXDamping = 0.0f, mcXMaxspeed = 0.0f, mcXIntegral = 0.0f, mcXDeadzone = 0.0f;
        float  mcYTracking = 0.0f, mcYDamping = 0.0f, mcYMaxspeed = 0.0f, mcYIntegral = 0.0f, mcYDeadzone = 0.0f;
        // 预测参数快照
        bool   kalmanCompensateDetectionDelay = false;
        float  kalmanAdditionalPredictionMs = 0.0f;
        float  kalmanResetTimeoutSec = 0.5f;
        // 射击参数快照
        float  bScopeMultiplierProfile = 0.0f;
        // 速度曲线参数快照 (profile 覆盖)
        float  snapRadius = 0.0f;
        float  nearRadius = 0.0f;
        float  speedCurveExponent = 1.0f;
        float  snapBoostFactor = 1.0f;
        float  profileMinSpeedMultiplier = 0.0f;
        float  profileMaxSpeedMultiplier = 0.0f;
    };

    std::mutex                   pendingConfigMutex;
    std::optional<PendingConfig> pendingConfig;
    std::atomic<bool>            pendingConfigDirty{ false };

    // 应用暂存参数。调用方必须已持有 input_method_mutex。
    void applyPendingConfigLocked();

    bool   curve_enabled = false;
    double curve_intensity, perturbation_strength, curve_max_speed, curve_distance;
    float  snapRadius = 0.0f;
    float  nearRadius = 0.0f;
    float  speedCurveExponent = 1.0f;
    float  snapBoostFactor = 1.0f;
    float  profileMinSpeedMultiplier = 0.0f;
    float  profileMaxSpeedMultiplier = 0.0f;
    void   mouseCurveMoveRelative(int dx, int dy);
    void   resetCurveState();
    void   appendCurveDebugStep(int dx, int dy);
    void   pruneCurveDebugTrailLocked(const std::chrono::steady_clock::time_point& now);
    std::pair<double, double> mouseCountsToScreenPixels(int dx, int dy) const;

    struct CurveDebugPoint
    {
        double x = 0.0;
        double y = 0.0;
        std::chrono::steady_clock::time_point t{};
    };

    // Persistent curve state to avoid per-frame "reset" feel.
    double curveCarryX = 0.0;
    double curveCarryY = 0.0;
    double curveVelX = 0.0;
    double curveVelY = 0.0;
    double curveNoiseX = 0.0;
    double curveNoiseY = 0.0;
    double curveFracX = 0.0;
    double curveFracY = 0.0;
    double curvePatternX = 0.0;
    double curvePatternY = 0.0;
    double curvePatternPhaseA = 0.0;
    double curvePatternPhaseB = 0.0;
    double curvePatternRateA = 0.0;
    double curvePatternRateB = 0.0;
    std::mt19937 curveRng{ std::random_device{}() };
    std::uniform_real_distribution<double> curveNoiseDistribution{ -1.0, 1.0 };
    std::uniform_real_distribution<double> curveClipDistribution{ 0.55, 1.0 };
    
    std::deque<CurveDebugPoint> curveDebugTrail;
    std::mutex                             curveDebugTrailMutex;
    double                                 curveDebugCursorX = 0.0;
    double                                 curveDebugCursorY = 0.0;

    struct MotionCompensationSample
    {
        double x = 0.0;
        double y = 0.0;
        std::chrono::steady_clock::time_point t{};
    };

    mutable std::mutex motionCompensationMutex;
    std::deque<MotionCompensationSample> motionCompensationTrail;
    void recordMotionCompensationStep(int dx, int dy);
    void pruneMotionCompensationTrailLocked(const std::chrono::steady_clock::time_point& now);

    std::pair<double, double> calc_movement(double target_x, double target_y);
    double calculate_speed_multiplier(double distance);
    double currentDetectionDelaySec(double observationAgeSec = -1.0) const;
    double currentPredictionLookaheadSec(double detectionDelaySec) const;

public:
    std::mutex input_method_mutex;

    MouseThread(
        int  resolution,
        int  fovX,
        int  fovY,
        double minSpeedMultiplier,
        double maxSpeedMultiplier,
        double predictionInterval,
        bool auto_shoot,
        float bScope_multiplier,
        IMouseInput* mouseInputDevice = nullptr
    );
    ~MouseThread();

    void updateConfig(
        int resolution,
        int fovX,
        int fovY,
        double minSpeedMultiplier,
        double maxSpeedMultiplier,
        double predictionInterval,
        bool auto_shoot,
        float bScope_multiplier
    );

    // 由鼠标线程在空闲轮次调用，确保无瞄准目标时参数变更也能及时生效。
    void applyPendingConfig();

    void moveMousePivot(
        double pivotX,
        double pivotY,
        std::chrono::steady_clock::time_point observationTime = {});
    void moveRelative(int dx, int dy);
    void clearQueuedMoves();
    std::pair<double, double> predict_target_position(
        double target_x,
        double target_y,
        std::chrono::steady_clock::time_point observationTime = {});
    void moveMouse(const AimbotTarget& target);
    void pressMouse(const AimbotTarget& target);
    void releaseMouse();
    void resetPrediction();
    void checkAndResetPredictions();
    void invalidateQueuedMoves();
    bool check_target_in_scope(const AimbotTarget& target, double reduction_factor);

    std::vector<std::pair<double, double>> predictFuturePositions(double pivotX, double pivotY, int frames);
    void storeFuturePositions(const std::vector<std::pair<double, double>>& positions);
    void clearFuturePositions();
    std::vector<std::pair<double, double>> getFuturePositions();
    void clearCurveDebugTrail();
    std::vector<std::pair<double, double>> getCurveDebugTrail();
    std::pair<double, double> getMotionCompensationSince(
        std::chrono::steady_clock::time_point since) const;

    void setMouseInput(IMouseInput* newMouseInput);

    void setTargetDetected(bool detected) { target_detected.store(detected); }
    void setLastTargetTime(const std::chrono::steady_clock::time_point& t) { last_target_time = t; }
};

#endif // MOUSE_H
