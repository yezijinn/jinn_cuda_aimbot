#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

// ============================================================================
// TargetPublisher.h
// 无锁 SPSC (单生产者-单消费者) 目标坐标快照
//
// 用途: 为 MouseController 提供线程安全的 YOLO 检测结果接入层。
//       真实集成时, TensorRT 推理线程作为生产者调用 publish(), 鼠标控制
//       线程 (100Hz) 作为消费者调用 tryConsume() 取最新快照后喂给
//       MouseController::update()。两个角色必须各自固定单线程。
//
// 线程模型:
//   生产者 (检测线程): publish(x, y) / publishInvalid()
//   消费者 (控制线程): tryConsume(x, y)
//   两者可并发执行, 内部无锁; 多生产者 / 多消费者并发调用是未定义行为。
//
// 无锁设计 —— 单 64 位原子快照 + 已发布标志 (初版双缓冲 + 槽位翻转方案在
// 多线程压力测试下暴露缺陷: 生产者连续两次发布翻转回原槽时, 消费者读旧槽
// 会与生产者写该槽并发 -> 数据竞争; 故改用本方案):
//   - 两个 float (x, y) 的位模式打包进一个 std::atomic<uint64_t>,
//     x 占低 32 位, y 占高 32 位;
//   - 64 位原子 load/store 保证单次读取必然是某次完整写入 (无撕裂,
//     C++ 内存模型标准行为, 非 UB);
//   - 生产者 store(release) 保证数据先于快照可见;
//     消费者 load(acquire) 保证读到完整最新快照;
//   - 无效帧用 x/y = NaN 位模式标记 (消费方判 isfinite 返回 false);
//   - (第九轮加固) m_published 原子标志解决"首帧 (0,0) 丢失"缺陷:
//     初版 m_snap 初始化为 0 (恰为 (0.0f, 0.0f) 的打包值), m_read 同为 0,
//     生产者若首个有效帧发布 (0,0) (目标在屏幕原点), v == m_read 恒判
//     "无新数据" -> 该帧永久丢失, 且目标持续停在原点时永远无法追踪。
//     修复分两层: 生产者发布时先 store 快照 (release) 再置位 m_published
//     (release), 消费者先查 m_published (acquire) 区分"从未发布"与"已发布";
//     同时 m_everConsumed (仅消费者访问) 与 m_read 解耦: 首次消费不做
//     v == m_read 比对 (初始 m_read=0 恰为 (0,0) 打包值, 直接比对会误判),
//     任何坐标 (含 0) 的首帧均可消费, 且无位模式哨兵碰撞风险;
//   - (第九轮加固) 消费端同时校验 x 与 y 的 isfinite: 初版仅查 x,
//     publish(有限x, NaN) 会返回 true 且 y=NaN 透传 (MouseController 有兜底,
//     但 TargetPublisher 作为独立接入层应自防御, 按无效帧处理);
//   - m_read / m_everConsumed 仅由消费者读写, 生产者不触碰, 无竞争。
//
// 丢帧语义: 检测线程发现 YOLO 丢帧/检测失败时调用 publishInvalid(),
//   消费者 tryConsume() 返回 false 且该无效快照即视为已消费 (不重复),
//   控制线程据此退化为仅外推 (tracker.predictOnly), 与
//   MouseController::update 的无效观测语义一致。
//
// 无新数据语义: 自上次消费后快照未变化 (生产频率 <= 消费频率, 或生产者
//   发布了与上次完全相同的坐标), tryConsume() 返回 false, 消费者保持
//   上一帧结果继续运行。
// ============================================================================

class TargetPublisher
{
public:
    TargetPublisher();

    // 生产者: 发布一帧有效检测结果 (YOLO 目标坐标, 像素)
    // 内部不校验坐标有效性, 由消费者 (MouseController 已有 isfinite +
    // 幅度防护) 兜底; 若需拦截可先自行 isfinite 检查再发布。
    void publish(float x, float y);

    // 生产者: 发布一帧无效检测 (丢帧/检测失败), 消费方将得到 false
    void publishInvalid();

    // 消费者: 取最新快照。
    //   返回 true  : 有新数据且有效, x/y 写入最新坐标
    //   返回 false : 无新数据 (帧率不匹配) 或最新快照无效 (丢帧)
    // 无论返回值, 本次调用后该快照即视为已消费。
    bool tryConsume(float& x, float& y) const;

private:
    static uint32_t f2u(float f)  // float 位模式 -> uint32 (memcpy, 严格无 UB)
    {
        uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        return u;
    }
    static float u2f(uint32_t u)  // uint32 位模式 -> float
    {
        float f;
        std::memcpy(&f, &u, sizeof f);
        return f;
    }

private:
    std::atomic<uint64_t> m_snap;     // 打包快照: 低 32 位 = x, 高 32 位 = y (float 位模式)
    std::atomic<bool>     m_published; // (第九轮) 是否已发布过任一帧 (含无效帧);
                                       // 解决首帧 (0,0) 快照与初始 m_read=0 冲突导致
                                       // 的目标坐标永久丢失缺陷, 并区分"从未发布"与
                                       // "已发布但无新数据"两种语义
    mutable uint64_t      m_read;     // 上次消费的快照值 (仅消费者线程访问, 非原子)
    mutable bool          m_everConsumed; // (第九轮) 是否已消费过任一帧 (仅消费者线程
                                          // 访问): 首次消费跳过 v==m_read 比对, 避免
                                          // 初始 m_read=0 恰为 (0,0) 打包值的误判,
                                          // 且无位模式哨兵碰撞风险
};
