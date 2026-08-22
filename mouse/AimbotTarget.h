#ifndef AIMBOTTARGET_H
#define AIMBOTTARGET_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <string>
#include <chrono>
#include <unordered_map>
#include "config.h"

struct NormalizedAimOffset
{
    float x = 0.5f;
    float y = 0.5f;
};

// 目标类别相关配置键名的常量缓存表。
// 热路径（sortTargets / tracker 配置快照 / trigger zone / debug 预览）每帧需要
// 读取 15 个类别 × 8 个键，若每帧用 "class_" + std::to_string(i) + "_xxx" 现拼接，
// 字符串长度超过 SSO 阈值必然触发堆分配（15×8 次/帧 @ 数百 FPS）。
// 该表在首次访问时构造一次，之后按 const std::string& 直接绑定，热路径零分配。
struct TargetClassConfigKeys
{
    std::string enabled[Config::FIXED_TARGET_CLASS_COUNT];
    std::string order[Config::FIXED_TARGET_CLASS_COUNT];
    std::string aimOffsetX[Config::FIXED_TARGET_CLASS_COUNT];
    std::string aimOffsetY[Config::FIXED_TARGET_CLASS_COUNT];
    std::string triggerZoneOffsetX[Config::FIXED_TARGET_CLASS_COUNT];
    std::string triggerZoneOffsetY[Config::FIXED_TARGET_CLASS_COUNT];
    std::string triggerZoneSizeX[Config::FIXED_TARGET_CLASS_COUNT];
    std::string triggerZoneSizeY[Config::FIXED_TARGET_CLASS_COUNT];

    TargetClassConfigKeys();
};

const TargetClassConfigKeys& targetClassConfigKeys();

class AimbotTarget
{
public:
    AimbotTarget();
    int x, y, w, h;
    int classId;

    double pivotX;
    double pivotY;
    NormalizedAimOffset aimOffset;

    AimbotTarget(int x, int y, int w, int h, int classId, double pivotX = 0.0, double pivotY = 0.0,
        NormalizedAimOffset aimOffset = {});
};

AimbotTarget* sortTargets(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight
);

struct LockedTargetInfo
{
    int trackId = -1;
    bool observedThisFrame = false;
    int missedFrames = 0;
    AimbotTarget target;
};

struct TrackDebugInfo
{
    int trackId = -1;
    int classId = -1;
    cv::Rect box;
    double pivotX = 0.0;
    double pivotY = 0.0;
    double velocityX = 0.0;
    double velocityY = 0.0;
    std::chrono::steady_clock::time_point lastUpdate{};
    bool observedThisFrame = false;
    int missedFrames = 0;
    bool isLocked = false;
};

class MultiTargetTracker
{
public:
    void reset();
    void update(
        const std::vector<cv::Rect>& boxes,
        const std::vector<int>& classes,
        int screenWidth,
        int screenHeight,
        bool keepCurrentLock,
        std::chrono::steady_clock::time_point observationTime = {}
    );
    bool getLockedTarget(LockedTargetInfo& out) const;
    int getLockedTrackId() const { return lockedTrackId_; }
    std::vector<TrackDebugInfo> getDebugTracks() const;

    // Dynamic Range support
    void setDynamicRangeConfig(
        bool enabled,
        int shrinkScope,
        int shrinkDurationMs,
        int cooldownMs,
        const std::string& targetClasses
    );
    // effectiveFov: 当前生效的水平 FOV（单位：度）；<= 0 表示动态收缩未激活。
    // baseFov:      原始（未收缩）水平 FOV（单位：度）。
    //   传入 > 0 时，update() 会按 effectiveFov / baseFov 这一无量纲收缩比例
    //   对检测过滤半径做逐帧插值，使过滤范围与 Overlay 绘制的收缩圈严格同步；
    //   省略或传入 <= 0 时退回旧行为（固定使用 shrinkScope / 2），
    //   既有调用点无需改动即可编译，且行为逐位不变。
    void setEffectiveFov(double effectiveFov, double baseFov = -1.0);
    void setTargetingMode(const std::string& mode);
    void setClassConfig(const bool* enabled, const int* order, int count);
    // 由鼠标线程在既有 configMutex 临界区内注入，避免 update() 内部二次加锁。
    // 内部会对非有限值/越界值做钳制，调用方可直接传入配置原始值。
    void setAimOffsets(const NormalizedAimOffset* offsets, int count);
    // 轨迹数硬上限（运行时可调，默认 kDefaultMaxTracks）。
    // 详见 AimbotTarget.cpp::enforceMaxTracks()：匈牙利匹配 O(n^3)、代价矩阵 O(n^2)，
    // n = max(轨迹数, 检测数)；检测数已由后处理 maxDetections 约束，
    // 但轨迹数跨帧累积、仅由漏检宽限帧回收，需硬上限防止极端负载下 CPU 尖峰。
    void setMaxTracks(std::size_t n);

private:
    struct TrackState
    {
        int id = -1;
        cv::Rect2f box;
        cv::Point2f velocity = { 0.0f, 0.0f };
        int classId = -1;
        int hits = 0;
        int missed = 0;
        bool observedThisFrame = false;
        double pivotX = 0.0;
        double pivotY = 0.0;
        NormalizedAimOffset aimOffset;
        std::chrono::steady_clock::time_point lastUpdate;
    };

    struct DetectionCandidate
    {
        cv::Rect2f box;
        int classId = -1;
        double pivotX = 0.0;
        double pivotY = 0.0;
        NormalizedAimOffset aimOffset;
    };

    static float iou(const cv::Rect2f& a, const cv::Rect2f& b);
    int findTrackIndexById(int id) const;
    int chooseBestTrack(int screenWidth, int screenHeight) const;
    int allowedMissedFrames(const TrackState& t) const;
    void pruneDeadTracks();
    // 轨迹数硬上限防御：保持 tracks_.size() <= maxTracks_ 不变式，
    // 从而每帧匈牙利匹配与代价矩阵规模有界。正常负载（轨迹数 <= 上限）零行为变化。
    void enforceMaxTracks();

    std::vector<TrackState> tracks_;
    int nextId_ = 1;
    int lockedTrackId_ = -1;
    int maxMissedFrames_ = 6;
    // 轨迹数硬上限（第 31 轮新增）。默认 64：远超正常目标数（<= ~30），留 2x 余量；
    // 同时把每帧匈牙利 O(n^3) 与代价矩阵 O(n^2) 的 n 上限锁在 max(64, 检测数<=100)。
    std::size_t maxTracks_ = 64;
    double frameDtMeanSec_ = 1.0 / 60.0;
    std::chrono::steady_clock::time_point lastTrackerFrameTime_{};

    // Dynamic Range state
    bool dynamicRangeEnabled_ = false;
    int dynamicRangeShrinkScope_ = 320;
    int dynamicRangeShrinkDurationMs_ = 300;
    int dynamicRangeCooldownMs_ = 300;
    std::chrono::steady_clock::time_point dynamicRangeStateUntil_{};
    std::chrono::steady_clock::time_point dynamicRangeCooldownUntil_{};
    bool dynamicRangeShrinking_ = false;
    std::vector<int> enabledClasses_;
    bool classEnabled_[Config::MAX_CLASSES]{};
    int classOrder_[Config::MAX_CLASSES]{};
    NormalizedAimOffset aimOffsets_[Config::MAX_CLASSES]{};
    double effectiveFov_ = -1.0; // -1 means disabled/use original FOV
    // 原始（未收缩）水平 FOV，单位与 effectiveFov_ 一致（度）。
    // <= 0 表示调用方未提供基准，此时 update() 退回固定 shrinkScope/2 的旧行为。
    double baseFov_ = -1.0;
    std::string targetingMode_ = "closest_center"; // "closest_center" or "largest_box"

    // 跨帧复用的热路径工作缓冲：仅在容量不足时才重新分配，
    // 稳态运行下 update() 不再产生任何堆分配。
    std::vector<DetectionCandidate> detBuffer_;
    std::vector<int> detAssigned_;
    std::vector<int> trackAssigned_;
    std::vector<double> matchCost_;      // 行主序展平的代价矩阵（matrixSize × matrixSize）
    std::vector<int> matchAssignment_;   // 匈牙利匹配结果
};

#endif // AIMBOTTARGET_H
