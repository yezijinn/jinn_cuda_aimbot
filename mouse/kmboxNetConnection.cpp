#include <algorithm>
#include <iostream>
#include <thread>
#include <chrono>

#include "kmbox_net/kmboxNet.h"
#include "KmboxNetConnection.h"

// ============================================================================
// 加锁约定（务必遵守，勿再引入未加锁的命令包装）
//
// kmboxNet SDK 把套接字 sockClientfd、收发缓冲 tx/rx、软件鼠标状态 softmouse
// 与命令序号 tx.head.indexpts 全部实现为**文件作用域全局变量**，且内部零互斥
// （kmboxNet.cpp 中 grep "mutex" 无命中）。因此：
//
//   1) 所有会真正发出网络命令的包装函数，都必须持有 command_mutex_。
//      两个线程并发进入时会交叉改写同一份 tx 与 indexpts，导致：
//        - 命令内容错配（鼠标位移包被写成 LCD 命令头，反之亦然）；
//        - 应答比对 rx.head.indexpts != tx.head.indexpts 恒不成立，
//          SendCommandAndAwaitAck 每次都空等满 300ms 超时，
//          1000Hz 鼠标线程被拖成秒级卡顿。
//
//   2) monitorMouseXxx / monitorKeyboard 是**例外**：它们只读取
//      kmboxNet.cpp 中的原子镜像变量（不发网络包），本身已线程安全。
//      这些函数位于 1000Hz 瞄准热路径上，若给它们加 command_mutex_，
//      会让热路径与可阻塞 300ms 的网络命令互相排队，反而制造卡顿。
//      故保持无锁，仅此一类。
//
// 说明：当前所有外部调用点都额外持有 inputDevicesMutex，故上述竞争尚未实际发生；
// 此处补齐属于纵深防御 —— 一旦将来新增一个不持 inputDevicesMutex 的调用点，
// 缺锁就会立刻退化为真实的数据竞争，且症状（偶发 300ms 卡顿）极难定位。
// ============================================================================

KmboxNetConnection::KmboxNetConnection(const std::string& ip, const std::string& port, const std::string& uuid)
    : ip_(ip), port_(port), uuid_(uuid)
{
    int ret = kmNet_init((char*)ip.c_str(), (char*)port.c_str(), (char*)uuid.c_str());
    is_open_ = (ret == 0);
    if (!is_open_)
    {
        std::cerr << "[KmboxNet] Connection failed, ret=" << ret << std::endl;
        return;
    }

    aiming_active = false;
    shooting_active = false;
    zooming_active = false;

    // 监控链路是按键判定（keyboard_listener → keyPressed → kmNet_monitor_mouse_*）的
    // 唯一数据源，且项目明确不回退 Win32：监控未就绪时鼠标热键会整体失效。
    // kmNet_monitor 现在会等到监听线程就绪（bind 成功）才返回 success，因此这里
    // 对失败做有界重试，把"盒子刚开机/网络抖动"导致的偶发监控失败降到最低。
    // 每次失败间隔递增（100ms/200ms），最多 3 次；总耗时上限约 300ms(ACK) + 500ms(就绪)。
    constexpr int kMonitorRetryCount = 3;
    int monitorResult = -1;
    for (int attempt = 0; attempt < kMonitorRetryCount && monitorResult != 0; ++attempt)
    {
        monitorResult = kmNet_monitor(10000);
        if (monitorResult != 0)
        {
            if (attempt + 1 < kMonitorRetryCount)
                Sleep(100 * (attempt + 1));
        }
    }
    if (monitorResult != 0)
        std::cerr << "[KmboxNet] Monitor start failed after " << kMonitorRetryCount
                  << " attempts, ret=" << monitorResult << std::endl;

    // 连接/重连成功后清理盒子侧软件按键、键盘掩码与 unmask 残留，避免上次会话
    // 的按住状态延续到新会话。这里不能持 command_mutex_ 调用，releaseAllButtons()
    // 内部会自行加锁。
    if (is_open_.load())
        releaseAllButtons();
}

// 说明：该函数目前在全项目中无任何调用点（monitor_running_ 恒为 false，
// 循环体不会执行），属历史遗留的可选监听线程入口。为保持接口兼容而完整保留，
// 仅按本文件的加锁约定给其中的网络命令补上 command_mutex_。
void KmboxNetConnection::monitorThread()
{
    try
    {
        int ret = 0;
        {
            std::lock_guard<std::mutex> lock(command_mutex_);
            ret = kmNet_monitor(10000);
        }
        if (ret != 0)
        {
            std::cerr << "[KmboxNet] Monitor start failed, ret=" << ret << std::endl;
            return;
        }

        while (monitor_running_)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    catch (const std::exception& e)
    {
        std::cerr << "[KmboxNet] Monitor thread crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[KmboxNet] Monitor thread crashed: unknown exception." << std::endl;
    }
}

KmboxNetConnection::~KmboxNetConnection()
{
    // 即使连接已标记为关闭（例如 reboot 后），仍需要回收本地监听线程与 SDK 全局资源；
    // SDK 内部对无效句柄和 WSA 引用计数有兜底，这里统一走完整清理。
    // releaseAllButtons() 自带 command_mutex_，此处不可再包一层（std::mutex 不可重入）。
    releaseAllButtons();

    // 关闭监听与连接同样是网络命令，必须与其它命令互斥。
    std::lock_guard<std::mutex> lock(command_mutex_);
    // 即使 sockClientfd 已失效，kmNet_monitor(0) 也会尽量回收监听线程。
    (void)kmNet_monitor(0);
    (void)kmNet_close();
    is_open_.store(false);
}

bool KmboxNetConnection::move(int x, int y)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    // kmNet_mouse_move 形参为 short；越界窄化会回绕成反向位移，先行钳制。
    // 上限 -32767 与点击层 mouse::toMoveCount() 和 KmboxAConnection::move() 对齐，
    // 避免不同后端在 short::min (-32768) 这一侧行为分叉造成手工模拟时难以排查。
    const int clampedX = std::clamp(x, -32767, 32767);
    const int clampedY = std::clamp(y, -32767, 32767);
    const int ret = kmNet_mouse_move(static_cast<short>(clampedX), static_cast<short>(clampedY));
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::moveAuto(int x, int y, int ms)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_move_auto(x, y, ms);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::moveBezier(int x, int y, int ms, int x1, int y1, int x2, int y2)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_move_beizer(x, y, ms, x1, y1, x2, y2);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::leftDown()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_left(1);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::leftUp()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_left(0);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::rightDown()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_right(1);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::rightUp()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_right(0);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::side1Down()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_side1(1);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::side1Up()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_side1(0);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::side2Down()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_side2(1);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::side2Up()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_side2(0);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

void KmboxNetConnection::releaseAllButtons()
{
    std::lock_guard<std::mutex> lock(command_mutex_);

    // mouseAll(0, 0, 0, 0) clears the device's complete software button mask
    // in one acknowledged packet; individual releases can leave stale state
    // behind when a preceding UDP response was lost.
    const int ret = kmNet_mouse_all(0, 0, 0, 0);
    MarkDisconnectedLocked(ret);
    MarkDisconnectedLocked(kmNet_mouse_left(0));
    MarkDisconnectedLocked(kmNet_mouse_right(0));
    MarkDisconnectedLocked(kmNet_mouse_middle(0));
    MarkDisconnectedLocked(kmNet_mouse_side1(0));
    MarkDisconnectedLocked(kmNet_mouse_side2(0));
    MarkDisconnectedLocked(kmNet_unmask_all());
}

bool KmboxNetConnection::middleDown()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_middle(1);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::middleUp()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_middle(0);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::wheel(int wheel)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_wheel(wheel);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

bool KmboxNetConnection::mouseAll(int button, int x, int y, int wheel)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load())
        return false;
    const int ret = kmNet_mouse_all(button, x, y, wheel);
    MarkDisconnectedLocked(ret);
    return ret == 0;
}

void KmboxNetConnection::keyDown(int vkey)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_keydown(vkey);
    MarkDisconnectedLocked(ret);
}

void KmboxNetConnection::keyUp(int vkey)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_keyup(vkey);
    MarkDisconnectedLocked(ret);
}

void KmboxNetConnection::monitor(short port)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_monitor(port);
    MarkDisconnectedLocked(ret);
}

// ---------------------------------------------------------------------------
// 以下 monitorXxx 只读取 SDK 内的原子镜像，不产生网络往返，
// 且处于 1000Hz 瞄准热路径上：**刻意不加 command_mutex_**（见文件头说明）。
// ---------------------------------------------------------------------------
int KmboxNetConnection::monitorMouseLeft()
{
    if (!is_open_.load()) return -1;
    return kmNet_monitor_mouse_left();
}

int KmboxNetConnection::monitorMouseRight()
{
    if (!is_open_.load()) return -1;
    return kmNet_monitor_mouse_right();
}

int KmboxNetConnection::monitorMouseMiddle()
{
    if (!is_open_.load()) return -1;
    return kmNet_monitor_mouse_middle();
}

int KmboxNetConnection::monitorMouseSide1()
{
    if (!is_open_.load()) return -1;
    return kmNet_monitor_mouse_side1();
}

int KmboxNetConnection::monitorMouseSide2()
{
    if (!is_open_.load()) return -1;
    return kmNet_monitor_mouse_side2();
}

int KmboxNetConnection::monitorKeyboard(short vkey)
{
    if (!is_open_.load()) return -1;
    return kmNet_monitor_keyboard(vkey);
}

// ---------------------------------------------------------------------------
// 遮罩类命令：全部为网络往返，必须加锁。
// ---------------------------------------------------------------------------
void KmboxNetConnection::maskMouseLeft(bool enable)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_mask_mouse_left(enable ? 1 : 0);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::maskMouseRight(bool enable)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_mask_mouse_right(enable ? 1 : 0);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::maskMouseMiddle(bool enable)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_mask_mouse_middle(enable ? 1 : 0);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::maskMouseSide1(bool enable)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_mask_mouse_side1(enable ? 1 : 0);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::maskMouseSide2(bool enable)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_mask_mouse_side2(enable ? 1 : 0);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::maskMouseX(bool enable)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_mask_mouse_x(enable ? 1 : 0);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::maskMouseY(bool enable)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_mask_mouse_y(enable ? 1 : 0);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::maskMouseWheel(bool enable)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_mask_mouse_wheel(enable ? 1 : 0);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::maskKeyboard(short vkey)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_mask_keyboard(vkey);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::unmaskKeyboard(short vkey)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_unmask_keyboard(vkey);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::unmaskAll()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_unmask_all();
    MarkDisconnectedLocked(ret);
}

void KmboxNetConnection::reboot()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    // kmNet_reboot 内部会 CleanupClientSocket()（关闭全局 sockClientfd 并配对
    // WSACleanup），盒子随即重启，连接必然失效。原实现不更新 is_open_，导致 UI
    // 仍显示"已连接"、后续所有命令以 err_creat_socket 静默失败。
    // 置 false 后 device->isOpen() 返回 false：UI 正确显示"未连接"并禁用相关按钮，
    // 键盘判定走"设备未连接"路径（不调用 keyPressed，也不走物理监控）。
    // 重启前先停监控：kmNet_reboot() 会 CleanupClientSocket()，但不会回收监听线程。
    // 若不先 kmNet_monitor(0)，旧 ThreadListenProcess 会在 reboot 后继续持有全局
    // sockMonitorfd，下次重连创建新监听线程时可能发生旧线程/新线程句柄复用竞态。
    (void)kmNet_monitor(0);
    kmNet_reboot();
    is_open_.store(false);
}

void KmboxNetConnection::setConfig(const std::string& ip, unsigned short port)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_setconfig((char*)ip.c_str(), port);
    MarkDisconnectedLocked(ret);
}

void KmboxNetConnection::debug(short port, char enable)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_debug(port, enable);
    MarkDisconnectedLocked(ret);
}

void KmboxNetConnection::lcdColor(unsigned short rgb565)
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_lcd_color(rgb565);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::lcdPictureBottom(unsigned char* buff_128_80)
{
    if (buff_128_80 == nullptr) return;
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_lcd_picture_bottom(buff_128_80);
    MarkDisconnectedLocked(ret);
}
void KmboxNetConnection::lcdPicture(unsigned char* buff_128_160)
{
    if (buff_128_160 == nullptr) return;
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (!is_open_.load()) return;
    const int ret = kmNet_lcd_picture(buff_128_160);
    MarkDisconnectedLocked(ret);
}

void KmboxNetConnection::MarkDisconnectedLocked(int ret)
{
    if (ret == 0)
    {
        command_failures_.store(0, std::memory_order_relaxed);
        return;
    }
    if (!is_open_.load())
        return;
    const int failures = command_failures_.fetch_add(1, std::memory_order_acq_rel) + 1;
    // UDP 偶发丢 ACK 不应立即把盒子标为断开；只有连续多次命令失败才认定连接失效。
    // 任一命令成功都会清零计数，恢复后无需手动重连。
    if (failures >= 3)
    {
        is_open_.store(false);
        command_failures_.store(0, std::memory_order_relaxed);
        std::cerr << "[KmboxNet] command failed, ret=" << ret
                  << "，连续失败 " << failures << " 次，连接已标记为断开。" << std::endl;
    }
    else
    {
        std::cerr << "[KmboxNet] command failed, ret=" << ret
                  << "，连续失败 " << failures << " 次，暂不断开。" << std::endl;
    }
}
