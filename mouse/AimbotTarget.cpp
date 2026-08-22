#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <cmath>
#include <limits>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <sstream>
#include <opencv2/opencv.hpp>

#include "mybot.h"
#include "AimbotTarget.h"
#include "config.h"

namespace
{
constexpr double kMatchBaseGateMin = 24.0;
constexpr double kMatchBaseGateDiagMultiplier = 1.15;
constexpr double kMatchBaseGateConstant = 10.0;
constexpr double kMatchSpeedGateBase = 1.8;
constexpr double kMatchSpeedGateMissedMultiplier = 0.35;
constexpr double kMatchMissGateMin = 14.0;
constexpr double kMatchMissGateDiagMultiplier = 0.18;

constexpr double kMatchRelaxedMultiplier = 1.6;
constexpr double kMatchMissPenaltyMultiplier = 0.025;
constexpr double kMatchHitBonusMultiplier = 0.01;
constexpr double kMatchLockedBonus = 0.10;
constexpr double kMatchOverlapWeight = 0.30;

// 轨迹数硬上限默认值（见 MultiTargetTracker::maxTracks_）。
// 真实 FPS 场景同屏目标数通常 <= ~30，64 既给出 2x 余量又保证每帧匹配代价有界。
constexpr std::size_t kDefaultMaxTracks = 64;

// 归一化瞄准点偏移必须落在 [0,1] 内。
// config.ini 手改 NaN/Inf/越界值可绕过 normalizeHotkeyClassAimOffsets 的 clamp
// (std::clamp 对 NaN 恒返回 NaN), 非有限 pivot 会令 MC 拒绝移动或 tracker 得分失真。
float clampAimOffset(float value, float fallback)
{
    if (!std::isfinite(value))
        return fallback;
    return std::clamp(value, 0.0f, 1.0f);
}
} // namespace

TargetClassConfigKeys::TargetClassConfigKeys()
{
    for (int i = 0; i < Config::FIXED_TARGET_CLASS_COUNT; ++i)
    {
        const std::string idx = std::to_string(i);
        enabled[i] = "class_enabled_" + idx;
        order[i] = "class_order_" + idx;
        aimOffsetX[i] = "class_" + idx + "_aim_offset_x";
        aimOffsetY[i] = "class_" + idx + "_aim_offset_y";
        triggerZoneOffsetX[i] = "class_" + idx + "_trigger_zone_offset_x";
        triggerZoneOffsetY[i] = "class_" + idx + "_trigger_zone_offset_y";
        triggerZoneSizeX[i] = "class_" + idx + "_trigger_zone_size_x";
        triggerZoneSizeY[i] = "class_" + idx + "_trigger_zone_size_y";
    }
}

const TargetClassConfigKeys& targetClassConfigKeys()
{
    // C++11 起函数内静态局部变量初始化线程安全（magic statics）。
    static const TargetClassConfigKeys keys;
    return keys;
}

AimbotTarget::AimbotTarget()
    : x(0), y(0), w(0), h(0), classId(0), pivotX(0.0), pivotY(0.0), aimOffset()
{
}

AimbotTarget::AimbotTarget(
    int x_, int y_, int w_, int h_, int cls, double px, double py, NormalizedAimOffset aimOffset_)
    : x(x_), y(y_), w(w_), h(h_), classId(cls), pivotX(px), pivotY(py), aimOffset(aimOffset_)
{
}

AimbotTarget* sortTargets(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight)
{
    const size_t inputCount = std::min(boxes.size(), classes.size());
    if (inputCount == 0)
    {
        return nullptr;
    }

    cv::Point center(screenWidth / 2, screenHeight / 2);
    NormalizedAimOffset aimOffsets[Config::MAX_CLASSES];
    bool classEnabled[Config::MAX_CLASSES] = { false };
    int classOrder[Config::MAX_CLASSES] = {};
    std::string targetingMode = "closest_center";
    {
        const TargetClassConfigKeys& keys = targetClassConfigKeys();
        std::lock_guard<std::mutex> lock(configMutex);
        const int activeSlot = active_mouse_hotkey_slot.load(std::memory_order_relaxed);
        const Config::MouseHotkey* profile =
            activeSlot >= 0 && activeSlot < static_cast<int>(Config::MAX_MOUSE_HOTKEYS)
                ? &config.mouse_hotkeys[static_cast<std::size_t>(activeSlot)] : nullptr;
        for (int i = 0; i < Config::FIXED_TARGET_CLASS_COUNT; ++i)
        {
            classEnabled[i] = config.isClassEnabled(i);
            classOrder[i] = i;
            if (profile)
            {
                aimOffsets[i].x = clampAimOffset(profile->localFloat(keys.aimOffsetX[i], 0.5f), 0.5f);
                aimOffsets[i].y = clampAimOffset(profile->localFloat(keys.aimOffsetY[i], 0.5f), 0.5f);
                classEnabled[i] = classEnabled[i] && profile->localBool(keys.enabled[i], false);
                classOrder[i] = profile->localInt(keys.order[i], i);
            }
        }
        // 瞄准模式必须在无激活鼠标热键槽位时也回退到全局配置，
        // 否则键盘热键瞄准 / 未绑定鼠标热键场景下会被硬编码为 closest_center，
        // 与 tracker 路径（mouse_thread_loop 使用 config.targeting_mode 回退）行为不一致。
        targetingMode = profile
            ? profile->localString("targeting_mode", config.targeting_mode)
            : config.targeting_mode;
    }

    // 统计启用的类别数量，判断是否为多类别模式
    // 收集所有符合条件的候选目标
    struct Candidate
    {
        size_t idx;
        double distSq;
    };
    // 候选缓冲跨帧复用：sortTargets 是 tracker 关闭时的每帧后备选择路径，
    // 原实现每次调用都构造并 reserve 一个 std::vector，稳态下即一次堆分配 + 释放。
    // 该函数仅由鼠标线程调用，thread_local 既消除分配又天然无数据竞争。
    static thread_local std::vector<Candidate> candidates;
    candidates.clear();
    candidates.reserve(inputCount);

    for (size_t i = 0; i < inputCount; i++)
    {
        if (boxes[i].width <= 0 || boxes[i].height <= 0)
            continue;

        const int cls = classes[i];
        if (cls < 0 || cls >= Config::FIXED_TARGET_CLASS_COUNT)
            continue;

        // 全局类别过滤：只瞄准启用的类别
        if (!classEnabled[cls])
            continue;

        const NormalizedAimOffset aimOffset = aimOffsets[cls];
        const int pivotY = static_cast<int>(boxes[i].y + boxes[i].height * aimOffset.y);
        const int pivotX = static_cast<int>(boxes[i].x + boxes[i].width * aimOffset.x);
        // 原用 std::pow(x, 2) 求平方：pow 是通用超越函数调用，
        // 在「每帧 × 每候选 × 2 次」的热路径上远慢于一次乘法，且无精度收益。
        const double dx = static_cast<double>(pivotX - center.x);
        const double dy = static_cast<double>(pivotY - center.y);
        const double distSq = dx * dx + dy * dy;

        candidates.push_back({ i, distSq });
    }

    if (candidates.empty())
        return nullptr;

    // 字符串比较从内层循环提到循环外：原实现每次迭代做 2 次 std::string 比较。
    const bool preferLargestBox = (targetingMode == "largest_box");

    auto scoreOf = [&](size_t candIdx) -> double
        {
            return preferLargestBox
                ? -static_cast<double>(boxes[candidates[candIdx].idx].area())
                : candidates[candIdx].distSq;
        };

    size_t bestIdx = 0;
    for (size_t i = 1; i < candidates.size(); ++i)
    {
        const int candidateOrder = classOrder[classes[candidates[i].idx]];
        const int bestOrder = classOrder[classes[candidates[bestIdx].idx]];
        const bool preferCandidateClass = candidateOrder < bestOrder;
        // 同优先级判定必须比较 class_order 而非 class id。
        // 原实现用「类别 id 相等」作为可比较条件：一旦两个不同类别被配置成
        // 相同的 class_order_N（profile 中缺失部分 order 键时会回退默认值 i，
        // 与拖拽排序写入的序号撞车），二者既不满足 order 严格更小、
        // 也不满足同类别，后出现者永远无法取代先出现者
        // —— 表现为「明明有更近/更大的同优先级目标却始终瞄另一个」。
        // tracker 路径 chooseBestTrack() 用的正是 candidateOrder == bestOrder，
        // 此处对齐后两条选择路径在同一份配置下行为一致。
        // 默认配置（class_order_i == i，各类别序号互异）下判定结果逐位不变。
        const bool sameOrder = (candidateOrder == bestOrder);
        if (preferCandidateClass || (sameOrder && scoreOf(i) < scoreOf(bestIdx)))
            bestIdx = i;
    }

    const size_t nearestIdx = candidates[bestIdx].idx;
    int finalY = boxes[nearestIdx].y;
    int finalX = boxes[nearestIdx].x;
    int finalW = boxes[nearestIdx].width;
    int finalH = boxes[nearestIdx].height;
    int finalClass = classes[nearestIdx];

    const NormalizedAimOffset aimOffset = aimOffsets[finalClass];
    const double pivotX = finalX + finalW * aimOffset.x;
    const double pivotY = finalY + finalH * aimOffset.y;

    return new AimbotTarget(finalX, finalY, finalW, finalH, finalClass, pivotX, pivotY, aimOffset);
}

float MultiTargetTracker::iou(const cv::Rect2f& a, const cv::Rect2f& b)
{
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width, b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float w = std::max(0.0f, x2 - x1);
    const float h = std::max(0.0f, y2 - y1);
    const float inter = w * h;
    const float ua = a.width * a.height + b.width * b.height - inter;
    if (ua <= 1e-6f) return 0.0f;
    return inter / ua;
}

namespace
{
// 匈牙利算法（JV/匈牙利 O(n^3) 变体）的跨帧复用工作区。
// 原实现每帧、且每行迭代都要新建 minv/used 向量，
// n=matrixSize 时每帧堆分配次数为 O(n)（外加 4 个固定向量），
// 在数百 FPS 的鼠标线程热路径上是可测量的分配与缓存压力。
// tracker 仅由鼠标线程调用，此处使用 thread_local 保证无数据竞争。
struct HungarianScratch
{
    std::vector<double> u;
    std::vector<double> v;
    std::vector<double> minv;
    std::vector<int> p;
    std::vector<int> way;
    std::vector<char> used;
};

HungarianScratch& hungarianScratch()
{
    static thread_local HungarianScratch scratch;
    return scratch;
}

// costs: 行主序展平的 n×m 代价矩阵，行跨度为 stride（stride >= m）。
// assignment: 输出，长度 n，元素为匹配到的列下标，-1 表示未分配。
void hungarianMinimize(const double* costs, int n, int m, int stride, std::vector<int>& assignment)
{
    assignment.assign(static_cast<size_t>(std::max(n, 0)), -1);
    if (n <= 0 || m <= 0 || costs == nullptr || stride < m)
        return;

    HungarianScratch& s = hungarianScratch();
    s.u.assign(static_cast<size_t>(n) + 1, 0.0);
    s.v.assign(static_cast<size_t>(m) + 1, 0.0);
    s.p.assign(static_cast<size_t>(m) + 1, 0);
    s.way.assign(static_cast<size_t>(m) + 1, 0);
    s.minv.resize(static_cast<size_t>(m) + 1);
    s.used.resize(static_cast<size_t>(m) + 1);

    double* const u = s.u.data();
    double* const v = s.v.data();
    int* const p = s.p.data();
    int* const way = s.way.data();
    double* const minv = s.minv.data();
    char* const used = s.used.data();

    for (int i = 1; i <= n; ++i)
    {
        p[0] = i;
        int j0 = 0;
        std::fill_n(minv, m + 1, std::numeric_limits<double>::infinity());
        std::fill_n(used, m + 1, static_cast<char>(0));

        do
        {
            used[j0] = 1;
            const int i0 = p[j0];
            double delta = std::numeric_limits<double>::infinity();
            int j1 = 0;
            const double* const row = costs + static_cast<size_t>(i0 - 1) * static_cast<size_t>(stride);

            for (int j = 1; j <= m; ++j)
            {
                if (used[j])
                    continue;

                const double cur = row[j - 1] - u[i0] - v[j];
                if (cur < minv[j])
                {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta)
                {
                    delta = minv[j];
                    j1 = j;
                }
            }

            for (int j = 0; j <= m; ++j)
            {
                if (used[j])
                {
                    u[p[j]] += delta;
                    v[j] -= delta;
                }
                else
                {
                    minv[j] -= delta;
                }
            }

            j0 = j1;
        } while (p[j0] != 0);

        do
        {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    for (int j = 1; j <= m; ++j)
    {
        if (p[j] > 0)
            assignment[static_cast<size_t>(p[j] - 1)] = j - 1;
    }
}
}

int MultiTargetTracker::findTrackIndexById(int id) const
{
    for (size_t i = 0; i < tracks_.size(); ++i)
    {
        if (tracks_[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

int MultiTargetTracker::allowedMissedFrames(const TrackState& t) const
{
    // Keep the locked target alive longer to survive short occlusion/fast motion bursts.
    const int lockedBonus = (t.id == lockedTrackId_) ? 8 : 0;
    return maxMissedFrames_ + lockedBonus;
}

void MultiTargetTracker::pruneDeadTracks()
{
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(), [&](const TrackState& t) {
            return t.missed > allowedMissedFrames(t);
            }),
        tracks_.end());
}

void MultiTargetTracker::setMaxTracks(std::size_t n)
{
    // 下限 1 保证至少保留一个目标；上限 4096 防止误配把匹配矩阵放大到不可接受。
    constexpr std::size_t kMaxAllowed = 4096;
    maxTracks_ = (n < 1) ? 1 : (n > kMaxAllowed ? kMaxAllowed : n);
}

void MultiTargetTracker::enforceMaxTracks()
{
    // 不变式：每帧 update() 末尾调用本函数后，tracks_.size() <= maxTracks_。
    // 由于新轨迹在 pruneDeadTracks() 之后才追加（update 内 matching 阶段已结束），
    // 下一帧进入 matching 时的轨迹数 <= maxTracks_，匈牙利 O(n^3) 与代价矩阵 O(n^2)
    // 的 n 规模被永久锁定，杜绝极端检测噪声/负载下的 CPU 灾难性尖峰。
    // 正常负载（轨迹数 <= 上限）下本函数直接返回，零行为变化。
    if (tracks_.size() <= maxTracks_)
        return;

    // 保留「漏检最少、命中最多」的最可信轨迹，丢弃最差者。
    // partial_sort 仅把前 maxTracks_ 个最优者归位到 [begin, begin+maxTracks_)，
    // 避免全排序开销；随后 resize 截断尾部最差轨迹。
    std::partial_sort(
        tracks_.begin(),
        tracks_.begin() + static_cast<int>(maxTracks_),
        tracks_.end(),
        [](const TrackState& a, const TrackState& b) {
            if (a.missed != b.missed)
                return a.missed < b.missed;
            return a.hits > b.hits;
        });
    tracks_.resize(maxTracks_);

    // 被截断的最差轨迹可能恰为锁定目标；若其已被丢弃则回退到未锁定。
    if (findTrackIndexById(lockedTrackId_) < 0)
        lockedTrackId_ = -1;
}

int MultiTargetTracker::chooseBestTrack(int screenWidth, int screenHeight) const
{
    if (tracks_.empty())
        return -1;

    const double cx = screenWidth * 0.5;
    const double cy = screenHeight * 0.5;
    const bool largestBox = (targetingMode_ == "largest_box");

    // 使用当前运行时快照，避免在目标选择中读取全局配置导致 profile 设置失效。
    int enabledCount = 0;
    for (int i = 0; i < Config::FIXED_TARGET_CLASS_COUNT; ++i)
        if (classEnabled_[i]) enabledCount++;
    if (enabledCount == 0)
        return -1;
    int bestIdx = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (size_t i = 0; i < tracks_.size(); ++i)
    {
        const auto& t = tracks_[i];
        if (t.missed > allowedMissedFrames(t))
            continue;
        if (t.classId < 0 || t.classId >= Config::FIXED_TARGET_CLASS_COUNT || !classEnabled_[t.classId])
            continue;

        const double dx = t.pivotX - cx;
        const double dy = t.pivotY - cy;
        const double distSq = dx * dx + dy * dy;
        const double hitBonus = std::min(5, t.hits) * 4.0;
        const double missPenalty = t.missed * 50.0;

        double score;
        if (largestBox)
        {
            // screenWidth/screenHeight 为 int，直接相乘存在溢出与除零风险，
            // 改为 double 计算并保证分母 >= 1，避免 Inf/NaN 污染排序。
            const double screenArea = std::max(1.0,
                static_cast<double>(screenWidth) * static_cast<double>(screenHeight));
            const double boxArea = static_cast<double>(t.box.width) * static_cast<double>(t.box.height);
            const double areaBonus = -(boxArea / screenArea) * 5000.0;
            score = std::sqrt(distSq) * 0.3 + missPenalty - hitBonus + areaBonus;
        }
        else
        {
            score = std::sqrt(distSq) + missPenalty - hitBonus;
        }

        const int candidateOrder = classOrder_[t.classId];
        const int bestOrder = bestIdx >= 0 ? classOrder_[tracks_[bestIdx].classId] : std::numeric_limits<int>::max();
        if (bestIdx < 0 || candidateOrder < bestOrder ||
            (candidateOrder == bestOrder && score < bestScore))
        {
            bestScore = score;
            bestIdx = static_cast<int>(i);
        }
    }

    return bestIdx;
}

void MultiTargetTracker::setDynamicRangeConfig(
    bool enabled,
    int shrinkScope,
    int shrinkDurationMs,
    int cooldownMs,
    const std::string& targetClasses)
{
    dynamicRangeEnabled_ = enabled;
    dynamicRangeShrinkScope_ = shrinkScope;
    dynamicRangeShrinkDurationMs_ = std::clamp(shrinkDurationMs, 50, 2000);
    dynamicRangeCooldownMs_ = std::clamp(cooldownMs, 50, 2000);

    // Parse enabled classes
    enabledClasses_.clear();
    if (!targetClasses.empty())
    {
        std::stringstream ss(targetClasses);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            while (!token.empty() && token.front() == ' ') token.erase(token.begin());
            while (!token.empty() && token.back() == ' ') token.pop_back();
            if (!token.empty())
            {
                try { enabledClasses_.push_back(std::stoi(token)); }
                catch (...) {}
            }
        }
    }

}

void MultiTargetTracker::setEffectiveFov(double effectiveFov, double baseFov)
{
    effectiveFov_ = effectiveFov;
    baseFov_ = baseFov;
}

void MultiTargetTracker::setTargetingMode(const std::string& mode)
{
    targetingMode_ = mode;
}

void MultiTargetTracker::setClassConfig(const bool* enabled, const int* order, int count)
{
    const int safeCount = std::clamp(count, 0, Config::MAX_CLASSES);
    for (int i = 0; i < Config::MAX_CLASSES; ++i)
    {
        classEnabled_[i] = i < safeCount && enabled != nullptr ? enabled[i] : false;
        classOrder_[i] = i < safeCount && order != nullptr ? order[i] : i;
    }
}

void MultiTargetTracker::setAimOffsets(const NormalizedAimOffset* offsets, int count)
{
    const int safeCount = std::clamp(count, 0, Config::MAX_CLASSES);
    for (int i = 0; i < Config::MAX_CLASSES; ++i)
    {
        if (i < safeCount && offsets != nullptr)
        {
            aimOffsets_[i].x = clampAimOffset(offsets[i].x, 0.5f);
            aimOffsets_[i].y = clampAimOffset(offsets[i].y, 0.5f);
        }
        else
        {
            aimOffsets_[i].x = 0.5f;
            aimOffsets_[i].y = 0.5f;
        }
    }
}

void MultiTargetTracker::reset()
{
    tracks_.clear();
    nextId_ = 1;
    lockedTrackId_ = -1;
    frameDtMeanSec_ = 1.0 / 60.0;
    lastTrackerFrameTime_ = {};
}

void MultiTargetTracker::update(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight,
    bool keepCurrentLock,
    std::chrono::steady_clock::time_point observationTime)
{
    auto now = (observationTime.time_since_epoch().count() != 0)
        ? observationTime
        : std::chrono::steady_clock::now();
    if (lastTrackerFrameTime_.time_since_epoch().count() != 0)
    {
        double frameDt = std::chrono::duration<double>(now - lastTrackerFrameTime_).count();
        if (!std::isfinite(frameDt) || frameDt <= 0.0)
        {
            const double fallbackDt = std::clamp(frameDtMeanSec_, 1.0 / 500.0, 0.25);
            now = lastTrackerFrameTime_ +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(fallbackDt));
            frameDt = fallbackDt;
        }
        frameDt = std::clamp(frameDt, 1.0 / 500.0, 0.25);
        frameDtMeanSec_ = frameDtMeanSec_ * 0.88 + frameDt * 0.12;
    }
    lastTrackerFrameTime_ = now;

    // 类别放行表与瞄准点偏移均由鼠标线程在其既有 configMutex 临界区内
    // 通过 setClassConfig()/setAimOffsets() 注入，此处直接使用成员快照：
    // 1) 消除本函数原有的第二次 configMutex 获取（每帧一次锁竞争）；
    // 2) 与 chooseBestTrack() 使用同一张 classEnabled_ 表，
    //    修复"update 用全局表放行、chooseBestTrack 用 profile 表淘汰"导致的
    //    航迹被建立却永不可选、白白消耗匹配预算的不一致问题。
    const NormalizedAimOffset* const aimOffsets = aimOffsets_;

    for (auto& t : tracks_)
        t.observedThisFrame = false;

    const size_t inputCount = std::min(boxes.size(), classes.size());

    // Determine if we should filter detections by FOV (dynamic range)
    bool filterByFov = dynamicRangeEnabled_ && effectiveFov_ > 0.0;
    double halfFovPixelsSq = 0.0;
    double centerX = screenWidth * 0.5;
    double centerY = screenHeight * 0.5;
    if (filterByFov)
    {
        // 收缩半径（像素）。
        // 原实现固定取 shrinkScope/2，与 effectiveFov_ 完全无关：
        //   鼠标线程每帧把「插值中的 effectiveFov」通过 setEffectiveFov() 送进来，
        //   本函数却始终按最终收缩尺寸 shrinkScope 过滤，导致
        //   ①收缩/恢复动画期间过滤半径瞬间跳变（非平滑，与 Overlay 收缩圈不同步）；
        //   ②effectiveFov_ 仅被当作「是否收缩」的开关，其数值形同虚设。
        // 修复：当调用方提供了基准 FOV（baseFov_ > 0）时，用无量纲收缩比例
        //   ratio = clamp(effectiveFov_ / baseFov_, 0, 1) 缩放屏幕半宽，
        //   使过滤半径与 effectiveFov_ 的逐帧插值严格成正比、并与绘制圈一致；
        //   未提供基准时退回旧的 shrinkScope/2（保持既有调用点行为不变）。
        double half;
        if (baseFov_ > 0.0 && std::isfinite(baseFov_) && std::isfinite(effectiveFov_) &&
            screenWidth > 0 && screenHeight > 0)
        {
            const double ratio = std::clamp(effectiveFov_ / baseFov_, 0.0, 1.0);
            half = std::min(centerX, centerY) * ratio;
        }
        else
        {
            half = dynamicRangeShrinkScope_ * 0.5;
        }
        halfFovPixelsSq = half * half;
    }

    std::vector<DetectionCandidate>& dets = detBuffer_;
    dets.clear();
    dets.reserve(inputCount);
    for (size_t i = 0; i < inputCount; ++i)
    {
        const int cls = classes[i];
        const cv::Rect& b = boxes[i];
        if (b.width <= 0 || b.height <= 0)
            continue;
        if (cls < 0 || cls >= Config::FIXED_TARGET_CLASS_COUNT)
            continue;

        // 类别过滤：使用与 chooseBestTrack 一致的运行时快照表
        if (!classEnabled_[cls])
            continue;

        // Dynamic range 额外过滤（保留原有逻辑，但基于全局已过滤的类别）
        if (dynamicRangeEnabled_ && !enabledClasses_.empty())
        {
            bool classAllowed = false;
            for (int ec : enabledClasses_)
            {
                if (cls == ec) { classAllowed = true; break; }
            }
            if (!classAllowed)
                continue;
        }

        // FOV filtering (dynamic range) - use squared distance
        if (filterByFov)
        {
            double detCx = b.x + b.width * 0.5;
            double detCy = b.y + b.height * 0.5;
            double dx = detCx - centerX;
            double dy = detCy - centerY;
            if (dx * dx + dy * dy > halfFovPixelsSq)
                continue;
        }

        DetectionCandidate d;
        d.box = cv::Rect2f(static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.width), static_cast<float>(b.height));
        d.classId = cls;
        d.aimOffset = aimOffsets[cls];
        d.pivotX = b.x + b.width * d.aimOffset.x;
        d.pivotY = b.y + b.height * d.aimOffset.y;
        dets.push_back(d);
    }

    std::vector<int>& detAssigned = detAssigned_;
    std::vector<int>& trackAssigned = trackAssigned_;
    detAssigned.assign(dets.size(), -1);
    trackAssigned.assign(tracks_.size(), -1);
    constexpr double kUnassignedCost = 2.0;
    constexpr double kInvalidCost = 1000000.0;

    auto computeMatchScore = [&](const TrackState& t, const DetectionCandidate& d, bool relaxedForLocked) -> double
        {
            // 跨类别不允许关联，直接判定为不可匹配。
            // 原实现在此之后仍保留 classSwappedWithinTarget / classPenalty 两个
            // 恒为 false / 0.0 的死变量，会误导后续维护者认为存在跨类别接管分支，
            // 且 MSVC 在 /W4 下可能产生"赋值后未修改"的噪声告警，此处一并移除。
            if (d.classId != t.classId)
            {
                return std::numeric_limits<double>::infinity();
            }

            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                1e-4, 0.25
            );

            const float predCx = t.box.x + t.box.width * 0.5f + t.velocity.x * static_cast<float>(dt);
            const float predCy = t.box.y + t.box.height * 0.5f + t.velocity.y * static_cast<float>(dt);
            cv::Rect2f predBox(predCx - t.box.width * 0.5f, predCy - t.box.height * 0.5f, t.box.width, t.box.height);

            const double detCx = d.box.x + d.box.width * 0.5;
            const double detCy = d.box.y + d.box.height * 0.5;
            const double dxBox = detCx - predCx;
            const double dyBox = detCy - predCy;
            const double boxDistSq = dxBox * dxBox + dyBox * dyBox;

            const double predPivotX = t.pivotX + t.velocity.x * dt;
            const double predPivotY = t.pivotY + t.velocity.y * dt;
            const double dxPivot = d.pivotX - predPivotX;
            const double dyPivot = d.pivotY - predPivotY;
            const double pivotDistSq = dxPivot * dxPivot + dyPivot * dyPivot;

            const double diag = std::hypot(static_cast<double>(t.box.width), static_cast<double>(t.box.height));
            const double speed = std::hypot(t.velocity.x, t.velocity.y);
            const double baseGate = std::max(kMatchBaseGateMin, diag * kMatchBaseGateDiagMultiplier + kMatchBaseGateConstant);
            const double speedGate = speed * dt * (kMatchSpeedGateBase + t.missed * kMatchSpeedGateMissedMultiplier);
            const double missGate = t.missed * std::max(kMatchMissGateMin, diag * kMatchMissGateDiagMultiplier);
            double maxDist = baseGate + speedGate + missGate;
            if (relaxedForLocked)
                maxDist *= kMatchRelaxedMultiplier;

            const double maxDistSq = maxDist * maxDist;
            const double distSq = std::min(boxDistSq, pivotDistSq);
            if (distSq > maxDistSq)
                return std::numeric_limits<double>::infinity();

            const double dist = std::sqrt(distSq);
            const double overlap = iou(predBox, d.box);
            const double missPenalty = t.missed * kMatchMissPenaltyMultiplier;
            const double hitBonus = std::min(6, t.hits) * kMatchHitBonusMultiplier;
            const double lockedBonus = (t.id == lockedTrackId_) ? kMatchLockedBonus : 0.0;
            return (dist / maxDist) + (1.0 - overlap) * kMatchOverlapWeight + missPenalty - hitBonus - lockedBonus;
        };

    if (!tracks_.empty() && !dets.empty())
    {
        // 代价矩阵改为跨帧复用的行主序一维缓冲：
        // 原 vector<vector<double>> 每帧需要 matrixSize+1 次堆分配，
        // 且行数据在堆上离散分布，匈牙利内层循环的缓存局部性差。
        const size_t matrixSize = std::max(tracks_.size(), dets.size());
        const size_t cellCount = matrixSize * matrixSize;
        if (matchCost_.size() < cellCount)
            matchCost_.resize(cellCount);
        std::fill_n(matchCost_.data(), cellCount, kUnassignedCost);

        for (size_t ti = 0; ti < tracks_.size(); ++ti)
        {
            const auto& t = tracks_[ti];
            double* const row = matchCost_.data() + ti * matrixSize;
            for (size_t di = 0; di < dets.size(); ++di)
            {
                const bool relaxedForLocked = (t.id == lockedTrackId_);
                const double score = computeMatchScore(t, dets[di], relaxedForLocked);
                row[di] = std::isfinite(score) ? score : kInvalidCost;
            }
        }

        hungarianMinimize(
            matchCost_.data(),
            static_cast<int>(matrixSize),
            static_cast<int>(matrixSize),
            static_cast<int>(matrixSize),
            matchAssignment_);

        const std::vector<int>& assignment = matchAssignment_;
        for (size_t ti = 0; ti < tracks_.size(); ++ti)
        {
            if (ti >= assignment.size())
                continue;

            const int di = assignment[ti];
            if (di < 0 || di >= static_cast<int>(dets.size()))
                continue;

            const double assignedCost = matchCost_[ti * matrixSize + static_cast<size_t>(di)];
            if (!std::isfinite(assignedCost) ||
                assignedCost >= kInvalidCost ||
                assignedCost >= kUnassignedCost)
            {
                continue;
            }

            trackAssigned[ti] = di;
            detAssigned[di] = static_cast<int>(ti);
        }
    }

    for (size_t ti = 0; ti < tracks_.size(); ++ti)
    {
        auto& t = tracks_[ti];
        const int di = trackAssigned[ti];

        if (di >= 0)
        {
            const auto& d = dets[di];
            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                1e-4, 0.2
            );

            const float oldX = static_cast<float>(t.pivotX);
            const float oldY = static_cast<float>(t.pivotY);
            const float newX = static_cast<float>(d.pivotX);
            const float newY = static_cast<float>(d.pivotY);
            const cv::Point2f rawVel(
                static_cast<float>((newX - oldX) / dt),
                static_cast<float>((newY - oldY) / dt)
            );

            cv::Point2f clampedRawVel = rawVel;
            const double rawSpeed = std::hypot(clampedRawVel.x, clampedRawVel.y);
            const double maxReasonableSpeed = std::max(screenWidth, screenHeight) * 3.5;
            if (rawSpeed > maxReasonableSpeed && rawSpeed > 1e-4)
            {
                const float scale = static_cast<float>(maxReasonableSpeed / rawSpeed);
                clampedRawVel *= scale;
            }

            const float blend = (t.id == lockedTrackId_) ? 0.45f : 0.35f;
            t.velocity = t.velocity * (1.0f - blend) + clampedRawVel * blend;
            t.box = d.box;
            t.pivotX = d.pivotX;
            t.pivotY = d.pivotY;
            t.aimOffset = d.aimOffset;
            t.classId = d.classId;
            t.hits += 1;
            t.missed = 0;
            t.observedThisFrame = true;
            t.lastUpdate = now;
        }
        else
        {
            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                0.0, 0.2
            );
            t.box.x += t.velocity.x * static_cast<float>(dt);
            t.box.y += t.velocity.y * static_cast<float>(dt);
            t.pivotX += t.velocity.x * dt;
            t.pivotY += t.velocity.y * dt;
            const float decay = (t.id == lockedTrackId_) ? 0.90f : 0.84f;
            t.velocity *= decay;
            t.missed += 1;
            t.observedThisFrame = false;
            t.lastUpdate = now;
        }
    }

    for (size_t di = 0; di < dets.size(); ++di)
    {
        if (detAssigned[di] != -1)
            continue;

        const auto& d = dets[di];
        TrackState t;
        t.id = nextId_++;
        t.box = d.box;
        t.classId = d.classId;
        t.hits = 1;
        t.missed = 0;
        t.observedThisFrame = true;
        t.pivotX = d.pivotX;
        t.pivotY = d.pivotY;
        t.aimOffset = d.aimOffset;
        t.lastUpdate = now;
        tracks_.push_back(t);
    }

    pruneDeadTracks();

    // 第 31 轮：轨迹数硬上限防御，保证 tracks_.size() <= maxTracks_ 不变式
    // （详见 enforceMaxTracks() 注释），从而每帧匈牙利匹配规模有界。
    enforceMaxTracks();

    if (findTrackIndexById(lockedTrackId_) < 0)
        lockedTrackId_ = -1;

    if (!keepCurrentLock)
    {
        const int bestIdx = chooseBestTrack(screenWidth, screenHeight);
        lockedTrackId_ = (bestIdx >= 0) ? tracks_[bestIdx].id : -1;
        return;
    }

    if (lockedTrackId_ == -1)
    {
        const int bestIdx = chooseBestTrack(screenWidth, screenHeight);
        lockedTrackId_ = (bestIdx >= 0) ? tracks_[bestIdx].id : -1;
    }
}

bool MultiTargetTracker::getLockedTarget(LockedTargetInfo& out) const
{
    const int idx = findTrackIndexById(lockedTrackId_);
    if (idx < 0)
        return false;

    const auto& t = tracks_[idx];
    if (t.missed > allowedMissedFrames(t))
        return false;

    out.trackId = t.id;
    out.observedThisFrame = t.observedThisFrame;
    out.missedFrames = t.missed;
    out.target = AimbotTarget(
        static_cast<int>(std::lround(t.box.x)),
        static_cast<int>(std::lround(t.box.y)),
        static_cast<int>(std::lround(t.box.width)),
        static_cast<int>(std::lround(t.box.height)),
        t.classId,
        t.pivotX,
        t.pivotY,
        t.aimOffset
    );
    return true;
}

std::vector<TrackDebugInfo> MultiTargetTracker::getDebugTracks() const
{
    std::vector<TrackDebugInfo> out;
    out.reserve(tracks_.size());

    for (const auto& t : tracks_)
    {
        if (t.missed > allowedMissedFrames(t))
            continue;

        TrackDebugInfo d;
        d.trackId = t.id;
        d.classId = t.classId;
        d.box = cv::Rect(
            static_cast<int>(std::lround(t.box.x)),
            static_cast<int>(std::lround(t.box.y)),
            static_cast<int>(std::lround(t.box.width)),
            static_cast<int>(std::lround(t.box.height))
        );
        d.pivotX = t.pivotX;
        d.pivotY = t.pivotY;
        d.velocityX = t.velocity.x;
        d.velocityY = t.velocity.y;
        d.lastUpdate = t.lastUpdate;
        d.observedThisFrame = t.observedThisFrame;
        d.missedFrames = t.missed;
        d.isLocked = (t.id == lockedTrackId_);
        out.push_back(d);
    }

    return out;
}
