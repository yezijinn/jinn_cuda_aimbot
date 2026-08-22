#include "TargetPublisher.h"
#include <cmath>

// ============================================================================
// TargetPublisher.cpp
//
// 无锁正确性论证 (单写单读):
//   快照打包: v = (f2u(y) << 32) | f2u(x), 单个 std::atomic<uint64_t>。
//   生产者: 构造 v (两次 memcpy + 移位, 均为非原子局部计算) ->
//           m_snap.store(v, release) -> m_published.store(true, release)。
//           release 保证 v 的构造先于快照可见, 快照先于 published 置位可见。
//   消费者: m_published.load(acquire) 为 true 时, 经 release-acquire 配对保证
//           m_snap.load(acquire) 读到的是完整最新快照 (64 位原子读无撕裂)。
//   新鲜度: m_read 记录上次消费的 v; m_everConsumed 为 true 且 v == m_read 时
//   无新数据返回 false。m_read / m_everConsumed 仅消费者线程读写, 生产者
//   从不触碰, 无数据竞争 (标准无 UB)。
//
//   注: 若生产者连续发布两帧坐标完全相同的快照, 第二帧消费时 v == m_read
//       判为"无新数据" (对鼠标控制无影响: 坐标相同 = 目标无变化)。
//   (第九轮) m_published 初始 false 区分"从未发布"与"已发布但无新数据":
//   初版 m_snap 初始 0 = (0.0f,0.0f) 打包值, 与 m_read 初始 0 相同, 首帧
//   发布 (0,0) 被恒判无新数据且无法自愈 (目标在屏幕原点时永远无法追踪)。
//   修复: m_everConsumed 使首次消费不做 v==m_read 比对, 任何坐标首帧均可
//   消费; m_published 使初始态 (m_snap=0 但从未发布) 不被误消费。
// ============================================================================

TargetPublisher::TargetPublisher()
    : m_snap(0)
    , m_published(false)
    , m_read(0)
    , m_everConsumed(false)
{
    // m_snap 初始 0 (即 +0.0f/+0.0f 打包); m_published=false 保证首次
    // tryConsume 判"从未发布"返回 false; m_everConsumed=false 保证首个
    // 有效帧 (即使恰为 (0,0), 其打包值 0 与 m_read 初始 0 相同) 不被
    // v==m_read 比对误判为"无新数据"。
}

void TargetPublisher::publish(float x, float y)
{
    const uint64_t v = (static_cast<uint64_t>(f2u(y)) << 32) | f2u(x);
    m_snap.store(v, std::memory_order_release);
    m_published.store(true, std::memory_order_release);
}

void TargetPublisher::publishInvalid()
{
    // 无效帧标记: x = y = NaN 位模式 (0x7FC00000, 标准 quiet NaN)
    const uint64_t v = (static_cast<uint64_t>(0x7FC00000u) << 32) | 0x7FC00000u;
    m_snap.store(v, std::memory_order_release);
    m_published.store(true, std::memory_order_release);
}

bool TargetPublisher::tryConsume(float& x, float& y) const
{
    // (第九轮) 从未发布过任何帧 (含无效帧) -> 无数据。
    // 必须先查 m_published 再读快照: 若直接以 m_snap==m_read 判定,
    // 初始 m_snap=0 与 m_read=0 相同, 初始态会被误消费为 (0,0) 有效帧。
    if (!m_published.load(std::memory_order_acquire))
        return false;

    const uint64_t v = m_snap.load(std::memory_order_acquire);

    // (第九轮) 无新数据判定仅在已消费过之后生效: 首次消费跳过比对,
    // 否则初始 m_read=0 恰为 (0,0) 打包值, 首帧 (0,0) 被误判无新数据
    // 而永久丢失 (目标持续停在屏幕原点时永远无法追踪)。
    if (m_everConsumed && v == m_read)
        return false;

    m_read = v;           // 标记已消费 (即使无效也消费, 避免重复返回同一无效帧)
    m_everConsumed = true;

    const uint32_t xb = static_cast<uint32_t>(v & 0xFFFFFFFFu);
    const float    xf = u2f(xb);
    const float    yf = u2f(static_cast<uint32_t>(v >> 32));

    // (第九轮) 同时校验 x 与 y 的有限性: 初版仅查 x, publish(有限x, NaN)
    // 会返回 true 且 y=NaN 透传 (独立接入层应自防御)。任何非有限坐标
    // (丢帧标记 / 异常注入) 一律按无效帧消费处理。
    if (!std::isfinite(xf) || !std::isfinite(yf))
        return false;

    x = xf;
    y = yf;
    return true;
}
