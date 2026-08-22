#pragma once

#include <cmath>

// ============================================================================
// KalmanTracker.h
// 六维状态卡尔曼目标跟踪器 (Constant Acceleration, CA 匀加速模型)
//
// 状态向量: [x, y, vx, vy, ax, ay]
//   x, y  : 目标位置 (像素)
//   vx, vy: 目标速度 (像素/秒)
//   ax, ay: 目标加速度 (像素/秒^2)
//
// 相比 CV 匀速模型, CA 模型额外估计加速度, 对"变向 / 圆周 / 急启停"
// 等有机动加速度的目标预测更准, 高速变向场景滞后更小。
//
// 运动模型: 匀加速模型
//   x'  = x + vx*dt + 0.5*ax*dt^2
//   vx' = vx + ax*dt
//   ax' = ax
//   (y 通道同理)
// 观测模型: 仅观测位置 (x, y)
//
// 设计要点:
//   - 支持动态 dt (每次调用可传入不同时间步长)
//   - 过程噪声采用"分段恒定白噪声加加速度(jerk)模型", 支持目标高速机动
//   - 过程噪声 / 测量噪声均可在线调整
//   - 全部使用栈上固定数组 + 固定尺寸矩阵乘法, 无动态内存分配
// ============================================================================

class KalmanTracker
{
public:
    // 构造函数
    //   processNoise     : 过程噪声强度 q (jerk 噪声谱密度), 越大跟踪越快但越不平滑
    //   measurementNoise : 测量噪声方差 r (YOLO 检测坐标噪声), 越大越信任预测
    explicit KalmanTracker(float processNoise = 1.0e3f,
                           float measurementNoise = 25.0f);

    // 以首个观测点初始化状态 (位置设为观测值, 速度/加速度置零)
    // 防护: 观测为 NaN/Inf 或幅度超 1e7 px (异常检测值) 时拒绝初始化, 保持未初始化状态。
    void initialize(float x, float y);

    // 完整一步: 先按 dt 外推, 再用新观测 (x, y) 校正
    // 健壮性: dt 非正/NaN 时钳制为 1ms; 观测 (x, y) 为 NaN/Inf
    //         (YOLO 丢帧/检测失败) 或幅度超 1e7 px 时自动退化为仅外推,
    //         不污染状态。
    void update(float x, float y, float dt);

    // 仅外推一步 (不校正): 用于检测无效 / 丢帧时保持目标运动估计。
    // 内部状态与协方差同步推进, 下一次 update() 可无缝衔接。
    void predictOnly(float dt);

    // 读取当前滤波后的位置与速度
    void getState(float& x, float& y, float& vx, float& vy) const;

    // 自运动补偿 (ego-motion compensation)
    //   FPS 场景中准星被锁在屏幕中心, 鼠标输出转动镜头, 会让"静止的目标"在
    //   屏幕上反向移动。若不补偿, 滤波器估计到的是"目标 - 准星"的相对速度,
    //   前瞻外推等于自我引用 (准星越快, 预测点越被甩到目标后方),
    //   会造成距目标仍远却提前停摆 -> 反向 -> 狩猎振荡, 高增益下发散。
    //   本方法应在 predict 之前调用, 把状态位置平移 (dx, dy) =
    //   "本帧镜头运动引起的目标屏幕位移", 使新息只反映目标自身运动,
    //   从而速度/加速度收敛到目标真实运动, 前瞻预测恢复正确语义。
    //   仅平移位置, 不改速度/加速度与协方差; 未初始化或非法入参时忽略。
    void applyEgoShift(float dx, float dy);

    // 在"当前已滤波状态"基础上, 按匀加速模型向前外推 aheadTime 秒, 得预测落点
    // 不修改内部协方差, 仅用于前瞻瞄准。
    // 健壮性: aheadTime 非负有限且 <= 10s, 否则按 0 处理 (无前瞻) /
    //         钳到 10s (防巨大有限值外推溢出 float)。
    void predictAhead(float aheadTime, float& outX, float& outY) const;

    // 在线调整噪声参数 (本轮审查加固: 非法输入防污染, 与 PID setter 防护对齐)
    //   q : 过程噪声, 须有限且 >= 0, 否则忽略 (保留旧值)
    //   r : 测量噪声, 须有限且 >  0, 否则忽略 (r 为 NaN/负值会经 correct()
    //       直接污染 S/det/K, 使状态永久失效, 故必须拦截)
    void setProcessNoise(float q)
    {
        if (std::isfinite(q) && q >= 0.0f) m_processNoise = q;
    }
    void setMeasurementNoise(float r)
    {
        if (std::isfinite(r) && r > 0.0f) m_measurementNoise = r;
    }

    bool isInitialized() const { return m_initialized; }

private:
    void predict(float dt);            // 状态 + 协方差外推
    void correct(float measX, float measY); // 观测校正

private:
    float m_state[6];       // 状态 [x, y, vx, vy, ax, ay]
    float m_P[6][6];        // 状态协方差矩阵
    float m_processNoise;   // 过程噪声强度 q (jerk)
    float m_measurementNoise;// 测量噪声方差 r
    bool  m_initialized;    // 是否已用首个观测初始化
};
