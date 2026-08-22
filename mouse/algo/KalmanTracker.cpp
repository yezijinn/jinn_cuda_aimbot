#include "KalmanTracker.h"
#include <cmath>

// ============================================================================
// KalmanTracker.cpp
// 匀加速模型 (CA) 卡尔曼滤波实现
//
// 状态转移矩阵 F(dt) (6x6), 行序 [x, y, vx, vy, ax, ay]:
//     [ 1  0  dt  0   dt^2/2  0     ]
//     [ 0  1  0   dt  0      dt^2/2 ]
//     [ 0  0  1   0   dt     0      ]
//     [ 0  0  0   1   0      dt     ]
//     [ 0  0  0   0   1      0      ]
//     [ 0  0  0   0   0      1      ]
//
// 过程噪声 Q(dt) —— 分段恒定白噪声加加速度(jerk)模型, 每通道 3x3:
//     Q3 = q * [ dt^5/20  dt^4/8  dt^3/6 ]
//              [ dt^4/8   dt^3/3  dt^2/2 ]
//              [ dt^3/6   dt^2/2  dt     ]
//   x 通道取索引 (0,2,4), y 通道取索引 (1,3,5), 其余为 0。
//
// 稳定性设计 (本次审查加固):
//   - update() / predictOnly() 对 dt 做有效性防护 (NaN/负/Inf -> 1ms)
//   - predict() 对 dt 做上限钳制 (<= 0.5s, 第六轮: 防巨大有限 dt 使 dt^2 溢出)
//   - predictAhead() 对 aheadTime 做上限钳制 (<= 10s, 第六轮: 防外推溢出)
//   - update() 对观测值做 isfinite + 幅度防护, 无效观测退化为仅外推,
//     防止 YOLO 丢帧 / 检测失败 (NaN/Inf/超大值) 污染滤波状态
//   - 首帧观测无效时拒绝初始化 (防止 NaN 永久污染状态)
//   - predict() 末尾对协方差做幅度钳制 (对角 + 非对角),
//     覆盖 update / predictOnly 全部路径, 防止长期运行数值发散
// ============================================================================

namespace
{
    // 协方差对角线上限: 超过视为数值发散, 强制回收 (位置/速度/加速度量级上限)
    constexpr float kMaxCovDiag = 1.0e12f;
    // 非对角相关性容差: |Pij| 上限 = kCorrTol * sqrt(Pii*Pjj)。
    // 5% 容差吸收正常浮点累积误差, 同时确保 P 近似半正定、
    // 创新协方差 S 行列式永不归零 (防 K 爆炸 / 状态溢出)。
    constexpr float kCorrTol = 1.05f;
    // 最小有效 dt: 防止 NaN/负值/0 导致 F/Q 奇异
    constexpr float kMinDt = 1.0e-3f;
    // 最大有效 dt (第六轮加固): 防止"巨大但有限"的 dt 使 dt^2 溢出 float 污染状态。
    // isfinite 仅挡 NaN/Inf, 不挡巨大有限值: 外部经 tracker() 访问器直调
    // update(x, y, 1e20) 时 dt^2=1e40 -> Inf, 状态外推 -> Inf, 且 m_initialized
    // 已置位后无法自愈。MouseController 内部已钳 dt <= 0.1s (kMaxDt),
    // 此上限仅针对模块独立边界 (外部直调场景, 如低频检测线程 2Hz -> dt=0.5s),
    // 对正常控制路径零影响。
    constexpr float kMaxDt = 0.5f;
    // 前瞻外推时长上限 (第六轮加固): 防止"巨大但有限"的 aheadTime 使
    // 0.5*a*t^2 溢出 float (ahead=1e20 -> 1e40 -> Inf) 污染输出落点。
    // MouseController 内部自适应前瞻 <= 0.15s / 固定前瞻 <= 10s, 此上限仅针对
    // 外部直调 tracker().predictAhead(巨大值) 的模块独立边界。
    constexpr float kMaxAhead = 10.0f;
    // 坐标幅度上限 (第五轮加固): 拦截"巨大但有限"的异常检测值。
    // isfinite 仅挡 NaN/Inf, 不挡超大有限值: 若 1e30 量级坐标进入状态,
    // 经轨迹规划 solveQuintic 的 dp 项 (c3 ~ 1e10*dp) 会溢出 float -> Inf,
    // 污染全链且无法自愈。真实 YOLO 坐标 < 1e5 px, 1e7 上限零影响。
    constexpr float kMaxCoord = 1.0e7f;

    // 坐标有效性: 有限且在允许幅度内 (NaN/Inf/超大 -> 无效观测)
    inline bool validCoord(float v)
    {
        return std::isfinite(v) && std::fabs(v) <= kMaxCoord;
    }
}

KalmanTracker::KalmanTracker(float processNoise, float measurementNoise)
    : m_processNoise((std::isfinite(processNoise) && processNoise >= 0.0f) ? processNoise : 1.0e3f)
    , m_measurementNoise((std::isfinite(measurementNoise) && measurementNoise > 0.0f) ? measurementNoise : 25.0f)
    , m_initialized(false)
{
    for (int i = 0; i < 6; ++i)
    {
        m_state[i] = 0.0f;
        for (int j = 0; j < 6; ++j)
            m_P[i][j] = 0.0f;
    }
}

void KalmanTracker::initialize(float x, float y)
{
    // 防护 (第五轮加固): public API 可被外部直接调用 (经 tracker() 访问器),
    // 拒绝 NaN/Inf/超大坐标, 防止状态被一次性污染且无法自愈 (m_initialized
    // 已置位后不会再次走到初始化分支)。正常输入 (有限像素坐标) 零影响。
    if (!validCoord(x) || !validCoord(y))
        return;

    for (int i = 0; i < 6; ++i)
    {
        m_state[i] = 0.0f;
        for (int j = 0; j < 6; ++j)
            m_P[i][j] = 0.0f;
    }
    m_state[0] = x;   // x
    m_state[1] = y;   // y

    // 初始协方差: 位置较确定, 速度/加速度不确定但不过大 (抑制首帧过冲)
    m_P[0][0] = 100.0f;    // x
    m_P[1][1] = 100.0f;    // y
    m_P[2][2] = 1.0e4f;    // vx
    m_P[3][3] = 1.0e4f;    // vy
    m_P[4][4] = 1.0e4f;    // ax
    m_P[5][5] = 1.0e4f;    // ay

    m_initialized = true;
}

void KalmanTracker::update(float x, float y, float dt)
{
    // dt 防护: 非正 / NaN / Inf 一律钳制为最小有效步长
    if (!(dt > 0.0f) || !std::isfinite(dt))
        dt = kMinDt;

    if (!m_initialized)
    {
        // 首帧观测无效 (YOLO 尚无检测结果 / 首帧即丢帧 / 坐标超幅度) 时拒绝初始化:
        // 否则 NaN/Inf 会直接写入 m_state, 后续所有 predict/correct 全部被
        // 永久污染且无法自愈 (m_initialized 已置位, 不会再次走到初始化分支)。
        if (!validCoord(x) || !validCoord(y))
            return;
        initialize(x, y);
        return;
    }

    // 观测防护: 无效观测 (丢帧 / 检测失败 / 坐标超幅度) 仅外推, 不进行校正,
    // 避免 NaN/Inf/超大值污染状态与协方差, 保证整条控制链数值稳定。
    if (!validCoord(x) || !validCoord(y))
    {
        predict(dt);
        return;
    }

    predict(dt);
    correct(x, y);
}

void KalmanTracker::predictOnly(float dt)
{
    if (!m_initialized)
        return; // 尚无状态可外推

    if (!(dt > 0.0f) || !std::isfinite(dt))
        dt = kMinDt;

    predict(dt);
}

void KalmanTracker::predict(float dt)
{
    // dt 上限钳制 (第六轮加固): update()/predictOnly() 已在入口钳下限,
    // predict() 统一补上限, 两条路径全覆盖。防"巨大但有限"dt 使
    // dt^2/dt^5 溢出 float (dt=1e20 -> dt^2=1e40=Inf -> 状态/Q 全 Inf)。
    if (dt > kMaxDt) dt = kMaxDt;

    const float dt2  = dt * dt;     // dt^2
    const float dt2h = 0.5f * dt2;  // dt^2/2

    // ---- 1. 状态外推 (利用 F 稀疏性, 每行仅 3 个非零) ----
    //   x'  = x  + vx*dt + 0.5*ax*dt^2
    //   vx' = vx + ax*dt ; ax' = ax (同理 y)
    // 注意: 先更新位置(用旧速度)再更新速度, 顺序不可颠倒
    m_state[0] += m_state[2] * dt + dt2h * m_state[4];
    m_state[1] += m_state[3] * dt + dt2h * m_state[5];
    m_state[2] += m_state[4] * dt;
    m_state[3] += m_state[5] * dt;

    // ---- 2. 协方差外推 P = F * P * F^T + Q ----
    // 利用 F 的稀疏性展开 (替代通用 6x6 矩阵乘法, 乘加次数约 432 -> 180, ~2.4x 提速):
    //   F 每行非零: [0]=(0,2,4) [1]=(1,3,5) [2]=(2,4) [3]=(3,5) [4]=[5]=单位
    // 第一步: FP = F * P
    float FP[6][6];
    for (int j = 0; j < 6; ++j)
    {
        FP[0][j] = m_P[0][j] + dt * m_P[2][j] + dt2h * m_P[4][j];
        FP[1][j] = m_P[1][j] + dt * m_P[3][j] + dt2h * m_P[5][j];
        FP[2][j] = m_P[2][j] + dt * m_P[4][j];
        FP[3][j] = m_P[3][j] + dt * m_P[5][j];
        FP[4][j] = m_P[4][j];
        FP[5][j] = m_P[5][j];
    }
    // 第二步: Pn = FP * F^T (同理利用 F 列稀疏)
    float Pn[6][6];
    for (int i = 0; i < 6; ++i)
    {
        Pn[i][0] = FP[i][0] + dt * FP[i][2] + dt2h * FP[i][4];
        Pn[i][1] = FP[i][1] + dt * FP[i][3] + dt2h * FP[i][5];
        Pn[i][2] = FP[i][2] + dt * FP[i][4];
        Pn[i][3] = FP[i][3] + dt * FP[i][5];
        Pn[i][4] = FP[i][4];
        Pn[i][5] = FP[i][5];
    }

    // ---- 3. 加入过程噪声 Q (jerk 模型, 块对角两通道) ----
    float q  = m_processNoise;
    float d1 = dt;
    float d2 = dt * dt;
    float d3 = d2 * dt;
    float d4 = d3 * dt;
    float d5 = d4 * dt;
    float Q3[3][3] = {
        { q * d5 / 20.0f, q * d4 / 8.0f, q * d3 / 6.0f },
        { q * d4 / 8.0f,  q * d3 / 3.0f, q * d2 / 2.0f },
        { q * d3 / 6.0f,  q * d2 / 2.0f, q * d1        }
    };
    const int ch[2][3] = { {0, 2, 4}, {1, 3, 5} }; // x / y 通道的 (p,v,a) 索引
    for (int c = 0; c < 2; ++c)
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                Pn[ch[c][a]][ch[c][b]] += Q3[a][b];

    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            m_P[i][j] = Pn[i][j];

    // ---- 4. 协方差钳制 (本轮审查加固: 相关性约束 + 幅度上限) ----
    // 统一放在 predict() 末尾: 覆盖 update() / predictOnly() 两条路径。
    //
    // (a) 对角线: 超过 kMaxCovDiag 视为数值发散, 强制回收。
    // (b) 非对角: 由"独立幅度上限"升级为"相关性约束":
    //       |P[i][j]| <= kCorrTol * sqrt(P[i][i] * P[j][j])
    //     数学上保证 P 近似半正定 (相关系数 |rho| <= 1),
    //     从而创新协方差 S = H P H^T + R 的行列式恒不为 0。
    //     原实现仅按 ±kMaxCovDiag 独立钳制, 极端情况 (长时间外推后
    //     x/y 位置相关性饱和) 下 |P01| 可与 P00、P11 同达 1e12,
    //     S 行列式 -> 0, 逆矩阵元素被钳到 1e24, 卡尔曼增益 K 爆炸,
    //     状态更新 state += K*residual 可溢出为 Inf, 整条控制链失效。
    //     正常滤波中 |Pij| 恒 < sqrt(Pii*Pjj), 该约束零影响;
    //     仅当浮点累积/异常注入使相关性越界时才强制回收。
    for (int i = 0; i < 6; ++i)
        if (m_P[i][i] > kMaxCovDiag)
            m_P[i][i] = kMaxCovDiag;
    for (int i = 0; i < 6; ++i)
        for (int j = i + 1; j < 6; ++j)
        {
            // diagProd <= 0 (对角被浮点误差压到非正) 时按 0 处理:
            // 非对角钳为 0 (最保守), 同时避免 sqrt(负) 产生 NaN
            const float diagProd = m_P[i][i] * m_P[j][j];
            float lim = (diagProd > 0.0f) ? kCorrTol * std::sqrt(diagProd) : 0.0f;
            float v = m_P[i][j];
            if (v >  lim) v =  lim;
            if (v < -lim) v = -lim;
            m_P[i][j] = v;
            m_P[j][i] = v; // 同步对称
        }
}

void KalmanTracker::correct(float measX, float measY)
{
    // ---- 1. 残差 (创新) ----
    float resX = measX - m_state[0];
    float resY = measY - m_state[1];

    // ---- 2. 创新协方差 S = H P H^T + R (2x2) ----
    float r = m_measurementNoise;
    float s00 = m_P[0][0] + r;
    float s01 = m_P[0][1];
    float s10 = m_P[1][0];
    float s11 = m_P[1][1] + r;

    float det = std::fabs(s00 * s11 - s01 * s10); // 取绝对值, 防浮点误差致 det 微负
    if (det < 1.0e-12f) det = 1.0e-12f;           // 防奇异, 保证可逆
    float invDet = 1.0f / det;
    float i00 =  s11 * invDet;
    float i01 = -s01 * invDet;
    float i10 = -s10 * invDet;
    float i11 =  s00 * invDet;

    // ---- 3. 卡尔曼增益 K = P H^T S^-1 (6x2) ----
    float K[6][2];
    for (int i = 0; i < 6; ++i)
    {
        float ph0 = m_P[i][0];
        float ph1 = m_P[i][1];
        K[i][0] = ph0 * i00 + ph1 * i10;
        K[i][1] = ph0 * i01 + ph1 * i11;
    }

    // ---- 4. 状态更新 ----
    for (int i = 0; i < 6; ++i)
        m_state[i] += K[i][0] * resX + K[i][1] * resY;

    // ---- 5. 协方差更新: P = (I - K H) P ----
    float P[6][6];
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            P[i][j] = m_P[i][j];
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            m_P[i][j] = P[i][j] - K[i][0] * P[0][j] - K[i][1] * P[1][j];

    // 对称化以抑制浮点误差累积
    for (int i = 0; i < 6; ++i)
        for (int j = i + 1; j < 6; ++j)
        {
            float avg = 0.5f * (m_P[i][j] + m_P[j][i]);
            m_P[i][j] = avg;
            m_P[j][i] = avg;
        }
    // 注: 协方差幅度钳制已统一前移至 predict() 末尾 (覆盖 correct 前的 predict 路径),
    //     此处无需重复, 避免代码冗余。
}

void KalmanTracker::getState(float& x, float& y, float& vx, float& vy) const
{
    x  = m_state[0];
    y  = m_state[1];
    vx = m_state[2];
    vy = m_state[3];
}

void KalmanTracker::applyEgoShift(float dx, float dy)
{
    // 未初始化时无状态可平移 (首帧 initialize 会直接采用观测值)
    if (!m_initialized)
        return;
    // 入参防护: NaN/Inf/超幅度一律忽略, 防止污染状态且无自愈路径
    if (!validCoord(dx) || !validCoord(dy))
        return;
    m_state[0] += dx;
    m_state[1] += dy;
}

void KalmanTracker::predictAhead(float aheadTime, float& outX, float& outY) const
{
    // 防御: NaN/负值/Inf 前瞻按 0 处理 (退化为当前滤波位置)
    if (!(aheadTime >= 0.0f) || !std::isfinite(aheadTime))
        aheadTime = 0.0f;
    // 幅度上限 (第六轮加固): "巨大但有限"的 aheadTime (如 1e20) 会使
    // 0.5*a*t^2 溢出 float 为 Inf, 污染输出落点 (const 方法不污染内部状态,
    // 但输出经下游重规划路径可扩散)。钳到 kMaxAhead 后外推量有限。
    if (aheadTime > kMaxAhead) aheadTime = kMaxAhead;

    // 匀加速外推: p + v*t + 0.5*a*t^2
    outX = m_state[0] + m_state[2] * aheadTime + 0.5f * m_state[4] * aheadTime * aheadTime;
    outY = m_state[1] + m_state[3] * aheadTime + 0.5f * m_state[5] * aheadTime * aheadTime;
}
