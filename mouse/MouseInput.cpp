#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include "MouseInput.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#include "KmboxAConnection.h"
#include "KmboxNetConnection.h"
#include "Makcu.h"
#include "config.h"

namespace
{
// kmboxNet SDK (mouse/kmbox_net/kmboxNet.cpp) 把套接字、收发缓冲 tx/rx、
// 目标地址 addrSrv 与命令序号 indexpts 全部实现为**文件作用域全局变量**，且内部
// 没有任何互斥保护。因此"建立连接 / 关闭连接"这一对生命周期操作在进程内必须整体
// 串行化，否则两次连接尝试会互相踩踏（见 KmboxNetMouseInput 构造函数注释）。
std::mutex& KmboxNetLifecycleMutex()
{
    static std::mutex instance;
    return instance;
}

// 仅有生命周期锁还不够：两个连接线程谁先抢到锁由调度器决定。若后发起的请求
// B 先拿到锁、完成握手并对外宣称"已连接"，随后被放弃的旧线程 A 才拿到锁去
// kmNet_init（覆盖全局 sockClientfd）与 kmNet_close（关闭的其实是 B 的套接字），
// 故障现象与不加锁时完全一致。
// 因此给每次连接请求分配单调递增的代际号：持锁后先比对代际，发现自己已被更新的
// 请求取代就立即返回——连 kmNet_init 都不会执行，绝不触碰 SDK 的任何全局状态。
std::atomic<std::uint64_t>& KmboxNetRequestGeneration()
{
    static std::atomic<std::uint64_t> instance{ 0 };
    return instance;
}

bool logicalButtonPressed(
    const std::string& keyName,
    bool shootingActive,
    bool zoomingActive,
    bool aimingActive)
{
    if (keyName == "LeftMouseButton")
        return shootingActive;
    if (keyName == "RightMouseButton")
        return zoomingActive;
    if (keyName == "X2MouseButton")
        return aimingActive;
    return false;
}

bool sendWin32Move(int dx, int dy)
{
    INPUT input{ 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    // dx/dy are relative deltas. MOUSEEVENTF_VIRTUALDESK is only for
    // absolute coordinates and can make relative test movement ineffective.
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool sendWin32Click(DWORD flag)
{
    INPUT input{ 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

class Win32MouseInput final : public IMouseInput
{
public:
    const char* name() const override { return "WIN32"; }
    bool isOpen() const override { return true; }
    bool move(int dx, int dy) override { return sendWin32Move(dx, dy); }
    bool leftDown() override { return sendWin32Click(MOUSEEVENTF_LEFTDOWN); }
    bool leftUp() override { return sendWin32Click(MOUSEEVENTF_LEFTUP); }
    bool rightDown() override { return sendWin32Click(MOUSEEVENTF_RIGHTDOWN); }
    bool rightUp() override { return sendWin32Click(MOUSEEVENTF_RIGHTUP); }
};

class KmboxNetMouseInput final : public IMouseInput
{
public:
    KmboxNetMouseInput(const std::string& ip, const std::string& port, const std::string& uuid)
        : state_(std::make_shared<State>())
    {
        state_->connecting.store(true);
        // 代际号必须在构造线程（鼠标线程）上自增，才能与用户点击顺序一致。
        const std::uint64_t generation =
            KmboxNetRequestGeneration().fetch_add(1, std::memory_order_acq_rel) + 1;
        std::thread([state = state_, ip, port, uuid, generation] {
            // 【第 28 轮修复·异常逃逸】本 lambda 是全工程唯一 detach 的线程，
            // 且未走 mybot.cpp 的 StartThreadGuarded 包裹。
            // 其内部 std::make_unique<KmboxNetConnection>（构造函数会做套接字创建、
            // 字符串解析、可能抛 std::bad_alloc / std::system_error）以及
            // std::lock_guard 取锁失败，任一抛出都会让异常逃出线程入口，
            // 直接触发 std::terminate 杀死整个进程（无日志、无栈展开）。
            // 修法：整体包 try/catch(...)，并在所有出口保证 connecting 复位，
            // 否则 UI 会永久停留在"连接中"。捕获后仅放弃本次连接，主程序继续运行。
            try
            {
            // 整个"建连 -> 交接 / 丢弃销毁"过程必须持有进程级生命周期锁。
            //
            // 缺少该锁时的真实故障序列（用户连点两次"保存并重连"即可复现，
            // 盒子不可达时 kmNet_init 会阻塞握手超时 1000ms，窗口被极大放大）：
            //   1. 线程 A 正在 kmNet_init 中（尚未交接 device）；
            //   2. 鼠标线程析构本对象：closed=true，device 仍为空，析构瞬间返回；
            //   3. 鼠标线程立刻构造新的 KmboxNetMouseInput，派生线程 B；
            //   4. A、B 同时 kmNet_init：全局 sockClientfd 被覆盖 -> 前一个套接字
            //      句柄泄漏；tx/addrSrv 并发写入 -> 状态撕裂；
            //   5. A 随后发现 closed==true，销毁自己的 KmboxNetConnection ->
            //      ~KmboxNetConnection 调 kmNet_close() 关闭的却是 **B 刚建立的**
            //      那个全局套接字。
            //   最终现象：UI 显示"kmboxNet已连接"，但鼠标完全不动，且无任何报错。
            //
            // 持锁后 A 的建连与丢弃销毁会在 B 开始 kmNet_init 之前整体完成，
            // 两次尝试严格串行，句柄不泄漏也不会被交叉关闭。
            // 注意：析构函数**不**参与此锁，因此设备切换不会被阻塞——
            // createInputDevices() 中"旧对象析构"与"新对象构造"本就在同一线程上
            // 顺序执行，二者之间不存在并发窗口。
            std::lock_guard<std::mutex> lifecycle(KmboxNetLifecycleMutex());

            // 已经存在更新的连接请求：本次请求作废。必须在 kmNet_init 之前返回，
            // 否则仍会覆盖/关闭新连接持有的全局套接字。
            if (KmboxNetRequestGeneration().load(std::memory_order_acquire) != generation)
            {
                state->connecting.store(false);
                return;
            }

            auto device = std::make_unique<KmboxNetConnection>(ip, port, uuid);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                // 若输入设备已在连接完成前被销毁/切换，丢弃连接结果，
                // device 析构会正确关闭连接，避免资源泄漏。
                if (state->closed.load(std::memory_order_relaxed))
                {
                    state->connecting.store(false);
                    return;
                }
                state->device = std::move(device);
            }
            state->connecting.store(false);
            }
            catch (const std::exception& e)
            {
                state->connecting.store(false);
                std::cerr << "[kmboxNet] 连接线程异常，已放弃本次连接: " << e.what() << std::endl;
            }
            catch (...)
            {
                state->connecting.store(false);
                std::cerr << "[kmboxNet] 连接线程发生未知异常，已放弃本次连接。" << std::endl;
            }
            }).detach();
    }

    ~KmboxNetMouseInput() override
    {
        if (!state_)
            return;

        // 先从 State 中原子化摘除设备，再在生命周期锁内销毁。这样调用方从此刻起
        // 不会再拿到旧设备；同时析构期的 kmNet_close 不会与异步建连中的 kmNet_init
        // 交叉操作 SDK 文件作用域全局套接字、收发缓冲和目标地址。
        std::unique_ptr<KmboxNetConnection> deviceToClose;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            // closed 必须在同一临界区内、且先于 device 交接置位。否则连接线程可能
            // 恰好在窗口内回填刚建立的连接，使无人持有的设备继续保持打开。
            state_->closed.store(true, std::memory_order_relaxed);
            deviceToClose = std::move(state_->device);
            state_->connecting.store(false);
        }

        // 连接线程已用同一把锁覆盖“建连 -> 交接 / 丢弃销毁”全过程；析构若不参与，
        // 并发设备切换时旧对象的 kmNet_close 仍可能关闭新对象刚初始化的全局连接。
        // 注意不能在持有 state_->mutex 时等待此锁，否则连接线程在交接阶段会死锁。
        std::lock_guard<std::mutex> lifecycle(KmboxNetLifecycleMutex());
        deviceToClose.reset();
    }

    const char* name() const override { return "KMBOX_NET"; }
    bool isOpen() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->isOpen();
    }
    bool isConnecting() const override
    {
        return state_->connecting.load(std::memory_order_acquire);
    }
    bool move(int dx, int dy) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        return state_->device->move(dx, dy);
    }
    bool leftDown() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        return state_->device->leftDown();
    }
    bool leftUp() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        return state_->device->leftUp();
    }
    bool rightDown() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        return state_->device->rightDown();
    }
    bool rightUp() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        return state_->device->rightUp();
    }
    void releaseAllButtons() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        // 断线不代表盒子不再需要释放：如果此前已下发过 down（例如丢 ACK 后才把
        // is_open_ 标记为 false），上层主动释放按键时仍应尝试 flush 所有释放包。
        // KmboxNetConnection::releaseAllButtons() 内部会自行处理不可用 socket。
        if (state_->device)
            state_->device->releaseAllButtons();
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        KmboxNetConnection* device = state_->device.get();
        if (!device || !device->isOpen())
            return false;

        if (keyName == "LeftMouseButton" && device->monitorMouseLeft() == 1)
            return true;
        if (keyName == "RightMouseButton" && device->monitorMouseRight() == 1)
            return true;
        if (keyName == "MiddleMouseButton" && device->monitorMouseMiddle() == 1)
            return true;
        if (keyName == "X1MouseButton" && device->monitorMouseSide1() == 1)
            return true;
        if (keyName == "X2MouseButton" && device->monitorMouseSide2() == 1)
            return true;

        // 逻辑按键与物理监控同源：device->shooting_active/zooming_active/aiming_active
        // 无写入点（仅构造置 false），恒 false 会让 logicalButtonPressed 分支永远失效。
        // 改用 monitor 镜像后与上方物理分支、aimingActive()/shootingActive()/zoomingActive()
        // 完全一致（侧键2=瞄准、左键=射击、右键=缩放），消除与 Makcu 的行为差异。
        return logicalButtonPressed(
            keyName,
            device->monitorMouseLeft() == 1,
            device->monitorMouseRight() == 1,
            device->monitorMouseSide2() == 1);
    }
    // 原实现直接读 device->aiming_active/shooting_active/zooming_active —— 这三个
    // 原子量在 KmboxNetConnection 中仅构造时置 false、全项目无任何写入点，因此
    // isAimingActiveFromDevices()/isShootingActiveFromDevices()/isZoomingActiveFromDevices()
    // 对 kmboxNet 恒返回 false，与 Makcu（回调写入 *_active）行为不一致。
    // kmboxNet 的物理按键状态唯一来源是 kmNet 监控镜像（monitorMouseXxx），此处改为
    // 直接查询物理监控，语义与 keyPressed() 的物理分支一致：侧键2=瞄准、左键=射击、
    // 右键=缩放。device 未连接时 monitorMouseXxx 返回 -1，==1 恒为 false，行为不变。
    bool aimingActive() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->monitorMouseSide2() == 1;
    }
    bool shootingActive() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->monitorMouseLeft() == 1;
    }
    bool zoomingActive() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->monitorMouseRight() == 1;
    }
    KmboxNetConnection* kmboxNet() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device.get();
    }

private:
    struct State
    {
        mutable std::mutex mutex;
        std::unique_ptr<KmboxNetConnection> device;
        std::atomic<bool> connecting{ false };
        std::atomic<bool> closed{ false };
    };

    std::shared_ptr<State> state_;
};

class KmboxAMouseInput final : public IMouseInput
{
public:
    explicit KmboxAMouseInput(const std::string& pidvid)
        : device_(std::make_unique<KmboxAConnection>(pidvid))
    {
    }

    const char* name() const override { return "KMBOX_A"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        return device_->move(dx, dy);
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        return device_->leftDown();
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        return device_->leftUp();
    }
    bool rightDown() override
    {
        if (!isOpen())
            return false;
        return device_->rightDown();
    }
    bool rightUp() override
    {
        if (!isOpen())
            return false;
        return device_->rightUp();
    }
    KmboxAConnection* kmboxA() override { return device_.get(); }

private:
    std::unique_ptr<KmboxAConnection> device_;
};

class MakcuMouseInput final : public IMouseInput
{
public:
    MakcuMouseInput(const std::string& port, unsigned int baudrate)
        : device_(std::make_unique<MakcuConnection>(port, baudrate))
    {
    }

    const char* name() const override { return "MAKCU"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        return device_->move(dx, dy);
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        return device_->press(0);
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        return device_->release(0);
    }
    bool rightDown() override
    {
        if (!isOpen())
            return false;
        return device_->press(1);
    }
    bool rightUp() override
    {
        if (!isOpen())
            return false;
        return device_->release(1);
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return device_ && device_->aiming_active; }
    bool shootingActive() const override { return device_ && device_->shooting_active; }
    bool zoomingActive() const override { return device_ && device_->zooming_active; }
    MakcuConnection* makcu() override { return device_.get(); }

private:
    std::unique_ptr<MakcuConnection> device_;
};
}

std::optional<MouseInputMethod> ParseMouseInputMethod(const std::string& method)
{
    if (method == "WIN32")
        return MouseInputMethod::Win32;
    if (method == "KMBOX_NET")
        return MouseInputMethod::KmboxNet;
    if (method == "KMBOX_A")
        return MouseInputMethod::KmboxA;
    if (method == "MAKCU")
        return MouseInputMethod::Makcu;
    return std::nullopt;
}

std::string MouseInputMethodName(MouseInputMethod method)
{
    switch (method)
    {
    case MouseInputMethod::KmboxNet: return "KMBOX_NET";
    case MouseInputMethod::KmboxA: return "KMBOX_A";
    case MouseInputMethod::Makcu: return "MAKCU";
    case MouseInputMethod::Win32:
    default:
        return "WIN32";
    }
}

std::unique_ptr<IMouseInput> CreateMouseInputDevice(const Config& config)
{
    const MouseInputMethod method = ParseMouseInputMethod(config.input_method).value_or(MouseInputMethod::Win32);
    switch (method)
    {
    case MouseInputMethod::KmboxNet:
        return std::make_unique<KmboxNetMouseInput>(config.kmbox_net_ip, config.kmbox_net_port, config.kmbox_net_uuid);
    case MouseInputMethod::KmboxA:
        return std::make_unique<KmboxAMouseInput>(config.kmbox_a_pidvid);
    case MouseInputMethod::Makcu:
        return std::make_unique<MakcuMouseInput>(config.makcu_port, static_cast<unsigned int>(config.makcu_baudrate));
    case MouseInputMethod::Win32:
    default:
        return std::make_unique<Win32MouseInput>();
    }
}
