#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "KmboxNetConnection.h"
#include "kmbox_net/HidTable.h"

namespace
{

using std::uint32_t;

struct PacketRecord
{
    cmd_head_t head{};
    uint32_t length = 0;
    soft_mouse_t mouse{};
    soft_keyboard_t keyboard{};
    std::vector<unsigned char> payload;
};

class KmboxNetMock
{
public:
    KmboxNetMock()
    {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(1, 1), &wsaData) != 0)
            throw std::runtime_error("WSAStartup failed");
        wsa_started_ = true;

        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ == INVALID_SOCKET)
            throw std::runtime_error("mock socket create failed");

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bindAddr.sin_port = 0;
        if (bind(socket_, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR)
            throw std::runtime_error("mock socket bind failed");

        sockaddr_in sockName{};
        int sockNameLen = sizeof(sockName);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&sockName), &sockNameLen) == SOCKET_ERROR)
            throw std::runtime_error("mock getsockname failed");
        port_ = ntohs(sockName.sin_port);

        DWORD timeout = 1000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), static_cast<int>(sizeof(timeout)));
    }

    ~KmboxNetMock()
    {
        Stop();
        if (socket_ != INVALID_SOCKET)
            closesocket(socket_);
        if (wsa_started_)
            WSACleanup();
    }

    unsigned short Port() const { return port_; }

    void Start()
    {
        stop_ = false;
        thread_ = std::thread([this]() { ReadLoop(); });
    }

    void Stop()
    {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        packets_.clear();
        cursor_ = 0;
    }

    size_t Count()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return packets_.size();
    }

    PacketRecord Last()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (packets_.empty())
            throw std::runtime_error("mock has no packets");
        return packets_.back();
    }

    PacketRecord TakeNext()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cursor_ >= packets_.size())
            throw std::runtime_error("mock has no more packets");
        return packets_[cursor_++];
    }

    void SetDropNextAck(bool drop)
    {
        drop_next_ = drop;
    }

private:
    void ReadLoop()
    {
        client_tx packet{};
        sockaddr_in from{};
        int fromLen = sizeof(from);

        while (!stop_.load(std::memory_order_acquire))
        {
            std::memset(&packet, 0, sizeof(packet));
            fromLen = sizeof(from);
            const int received = recvfrom(
                socket_,
                reinterpret_cast<char*>(&packet),
                static_cast<int>(sizeof(packet)),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &fromLen);
            if (received == SOCKET_ERROR)
                continue;
            if (received < static_cast<int>(sizeof(cmd_head_t)))
                continue;

            PacketRecord record;
            record.head = packet.head;
            record.length = static_cast<uint32_t>(received);
            record.mouse = packet.cmd_mouse;
            record.keyboard = packet.cmd_keyboard;
            record.payload.assign(
                reinterpret_cast<const unsigned char*>(&packet),
                reinterpret_cast<const unsigned char*>(&packet) + received);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                packets_.push_back(record);
            }

            if (drop_next_.exchange(false, std::memory_order_acq_rel))
                continue;

            const cmd_head_t ack = packet.head;
            sendto(
                socket_,
                reinterpret_cast<const char*>(&ack),
                static_cast<int>(sizeof(ack)),
                0,
                reinterpret_cast<sockaddr*>(&from),
                fromLen);
        }
    }

    SOCKET socket_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    bool wsa_started_ = false;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> drop_next_{ false };
    std::thread thread_;
    std::mutex mutex_;
    std::vector<PacketRecord> packets_;
    size_t cursor_ = 0;
};

int Fail(const char* message)
{
    std::cerr << "[kmboxNet_protocol_test] FAIL: " << message << std::endl;
    return 1;
}

bool CheckMouse(const PacketRecord& p, uint32_t expectedCmd, int button, int x, int y, int wheel)
{
    if (p.head.cmd != expectedCmd)
    {
        std::cerr << "  cmd=0x" << std::hex << p.head.cmd << " expected=0x" << expectedCmd << std::dec << std::endl;
        return false;
    }
    if (p.mouse.button != button || p.mouse.x != x || p.mouse.y != y || p.mouse.wheel != wheel)
    {
        std::cerr << "  mouse button=" << p.mouse.button << " x=" << p.mouse.x
                  << " y=" << p.mouse.y << " wheel=" << p.mouse.wheel << std::endl;
        return false;
    }
    if (p.length != sizeof(cmd_head_t) + sizeof(soft_mouse_t))
    {
        std::cerr << "  mouse length=" << p.length << std::endl;
        return false;
    }
    return true;
}

bool CheckKeyboard(const PacketRecord& p, unsigned char ctrl, std::initializer_list<unsigned char> keys)
{
    if (p.head.cmd != cmd_keyboard_all)
    {
        std::cerr << "  keyboard cmd=0x" << std::hex << p.head.cmd << std::dec << std::endl;
        return false;
    }
    if (p.keyboard.ctrl != ctrl)
    {
        std::cerr << "  ctrl=0x" << std::hex << static_cast<unsigned>(p.keyboard.ctrl)
                  << " expected=0x" << static_cast<unsigned>(ctrl) << std::dec << std::endl;
        return false;
    }

    for (unsigned char key : keys)
    {
        bool present = false;
        for (unsigned char value : p.keyboard.button)
        {
            if (value == key)
            {
                present = true;
                break;
            }
        }
        if (!present)
        {
            std::cerr << "  missing keyboard key=0x" << std::hex << static_cast<unsigned>(key) << std::dec << std::endl;
            return false;
        }
    }
    if (p.length != sizeof(cmd_head_t) + sizeof(soft_keyboard_t))
    {
        std::cerr << "  keyboard length=" << p.length << std::endl;
        return false;
    }
    return true;
}

bool CheckMask(const PacketRecord& p, uint32_t expectedRand)
{
    if (p.head.cmd != cmd_mask_mouse && p.head.cmd != cmd_unmask_all)
    {
        std::cerr << "  mask cmd=0x" << std::hex << p.head.cmd << std::dec << std::endl;
        return false;
    }
    if (p.head.rand != expectedRand)
    {
        std::cerr << "  mask rand=0x" << std::hex << p.head.rand
                  << " expected=0x" << expectedRand << std::dec << std::endl;
        return false;
    }
    return true;
}

bool WaitUntil(std::function<bool()> predicate, int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        Sleep(5);
    }
    return predicate();
}

} // namespace

int RunMain()
{
    KmboxNetMock mock;
    mock.Start();

    const std::string mockPort = std::to_string(mock.Port());
    std::unique_ptr<KmboxNetConnection> conn;
    try
    {
        conn = std::make_unique<KmboxNetConnection>("127.0.0.1", mockPort.c_str(), "01020304");
    }
    catch (const std::exception& e)
    {
        return Fail(e.what());
    }

    if (!conn || !conn->isOpen())
        return Fail("connection did not open against mock box");

    // Constructor should have performed connect, monitor enable and a full release pass.
    if (mock.Count() < 7)
        return Fail("connection setup did not flush monitored/release commands");
    mock.Clear();

    if (!conn->leftDown())
        return Fail("leftDown returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_left, 0x01, 0, 0, 0))
        return Fail("leftDown packet mismatch");

    if (!conn->move(100, -200))
        return Fail("move returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_move, 0x01, 100, -200, 0))
        return Fail("move while left-down must preserve held button state");

    if (!conn->rightDown())
        return Fail("rightDown returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_right, 0x03, 0, 0, 0))
        return Fail("mixed left+right down packet mismatch");

    if (!conn->wheel(3))
        return Fail("wheel returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_wheel, 0x03, 0, 0, 3))
        return Fail("wheel while buttons held must not clear button mask");

    if (!conn->side1Down())
        return Fail("side1Down returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_right, 0x0B, 0, 0, 0))
        return Fail("mixed left+right+side1 down packet mismatch");

    if (!conn->side2Down())
        return Fail("side2Down returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_right, 0x1B, 0, 0, 0))
        return Fail("mixed left+right+side1+side2 down packet mismatch");

    if (!conn->move(5, 6))
        return Fail("move returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_move, 0x1B, 5, 6, 0))
        return Fail("move while side buttons held must preserve full button mask");

    if (!conn->side1Up())
        return Fail("side1Up returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_right, 0x13, 0, 0, 0))
        return Fail("side1Up packet mismatch");

    if (!conn->leftUp())
        return Fail("leftUp returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_left, 0x12, 0, 0, 0))
        return Fail("leftUp packet mismatch");

    if (!conn->rightUp())
        return Fail("rightUp returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_right, 0x10, 0, 0, 0))
        return Fail("rightUp while side2 held packet mismatch");

    if (!conn->side2Up())
        return Fail("side2Up returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_right, 0x00, 0, 0, 0))
        return Fail("side2Up packet mismatch");

    if (!conn->mouseAll(0x03, 7, 8, 9))
        return Fail("mouseAll returned false");
    if (!CheckMouse(mock.Last(), cmd_mouse_wheel, 0x03, 7, 8, 9))
        return Fail("mouseAll packet mismatch");

    conn->keyDown(KEY_A);
    if (!CheckKeyboard(mock.Last(), 0x00, { KEY_A }))
        return Fail("keyDown A packet mismatch");

    conn->keyDown(KEY_LEFTCONTROL);
    if (!CheckKeyboard(mock.Last(), 0x01, { KEY_A }))
        return Fail("keyDown LCtrl packet mismatch");

    conn->keyDown(KEY_B);
    if (!CheckKeyboard(mock.Last(), 0x01, { KEY_A, KEY_B }))
        return Fail("multi key packet mismatch");

    // KEY_VOLUME_UP has bit 0x80 set. In the original signed char board state,
    // keyUp could not compare the stored negative byte against the caller's int.
    conn->keyDown(KEY_VOLUME_UP);
    if (!CheckKeyboard(mock.Last(), 0x01, { KEY_A, KEY_B, KEY_VOLUME_UP }))
        return Fail("high-byte keyDown packet mismatch");

    conn->keyUp(KEY_VOLUME_UP);
    if (!CheckKeyboard(mock.Last(), 0x01, { KEY_A, KEY_B }))
        return Fail("high-byte keyUp packet mismatch: the key must be removed from the queue");

    conn->keyUp(KEY_B);
    if (!CheckKeyboard(mock.Last(), 0x01, { KEY_A }))
        return Fail("keyUp B packet mismatch");

    conn->keyUp(KEY_A);
    if (!CheckKeyboard(mock.Last(), 0x01, {}))
        return Fail("keyUp A packet mismatch");

    conn->keyUp(KEY_LEFTCONTROL);
    if (!CheckKeyboard(mock.Last(), 0x00, {}))
        return Fail("keyUp LCtrl packet mismatch");

    mock.Clear();
    conn->releaseAllButtons();
    {
    const PacketRecord all = mock.TakeNext();
        if (all.head.cmd != cmd_mouse_wheel || all.mouse.button != 0 ||
            all.mouse.x != 0 || all.mouse.y != 0 || all.mouse.wheel != 0)
            return Fail("releaseAll all-zero mouse packet mismatch");
    }
    {
    const PacketRecord left = mock.TakeNext();
        if (left.head.cmd != cmd_mouse_left || left.mouse.button != 0)
            return Fail("releaseAll left release packet mismatch");
    }
    {
    const PacketRecord right = mock.TakeNext();
        if (right.head.cmd != cmd_mouse_right || right.mouse.button != 0)
            return Fail("releaseAll right release packet mismatch");
    }
    {
    const PacketRecord middle = mock.TakeNext();
        if (middle.head.cmd != cmd_mouse_middle || middle.mouse.button != 0)
            return Fail("releaseAll middle release packet mismatch");
    }
    {
    const PacketRecord side1 = mock.TakeNext();
        if (side1.head.cmd != cmd_mouse_right || side1.mouse.button != 0)
            return Fail("releaseAll side1 release packet mismatch");
    }
    {
    const PacketRecord side2 = mock.TakeNext();
        if (side2.head.cmd != cmd_mouse_right || side2.mouse.button != 0)
            return Fail("releaseAll side2 release packet mismatch");
    }
    {
    const PacketRecord unmask = mock.TakeNext();
        if (unmask.head.cmd != cmd_unmask_all)
            return Fail("releaseAll unmask packet mismatch");
    }

    mock.Clear();
    conn->maskMouseLeft(true);
    if (!CheckMask(mock.Last(), 0x01))
        return Fail("maskMouseLeft(true) mask bitmap mismatch");
    conn->maskMouseRight(true);
    if (!CheckMask(mock.Last(), 0x03))
        return Fail("maskMouseRight(true) mask bitmap mismatch");
    conn->maskMouseMiddle(true);
    if (!CheckMask(mock.Last(), 0x07))
        return Fail("maskMouseMiddle(true) mask bitmap mismatch");
    conn->maskMouseSide1(true);
    if (!CheckMask(mock.Last(), 0x0F))
        return Fail("maskMouseSide1(true) mask bitmap mismatch");
    conn->maskMouseSide2(true);
    if (!CheckMask(mock.Last(), 0x1F))
        return Fail("maskMouseSide2(true) mask bitmap mismatch");
    conn->maskMouseX(true);
    if (!CheckMask(mock.Last(), 0x3F))
        return Fail("maskMouseX(true) mask bitmap mismatch");
    conn->maskMouseY(true);
    if (!CheckMask(mock.Last(), 0x7F))
        return Fail("maskMouseY(true) mask bitmap mismatch");
    conn->maskMouseWheel(true);
    if (!CheckMask(mock.Last(), 0xFF))
        return Fail("maskMouseWheel(true) mask bitmap mismatch");
    conn->maskKeyboard(KEY_A);
    if (!CheckMask(mock.Last(), 0x4FF))
        return Fail("maskKeyboard(KEY_A) mask bitmap mismatch");
    conn->unmaskKeyboard(KEY_A);
    if (!CheckMask(mock.Last(), 0x4FF))
        return Fail("unmaskKeyboard(KEY_A) mask bitmap mismatch");
    conn->unmaskAll();
    if (!CheckMask(mock.Last(), 0x00))
        return Fail("unmaskAll mask bitmap mismatch");

    mock.Clear();
    mock.SetDropNextAck(true);
    conn->maskMouseLeft(true);
    if (!conn->isOpen())
        return Fail("single dropped ack must not permanently mark kmboxNet as disconnected");

    mock.SetDropNextAck(true);
    conn->maskMouseLeft(true);
    if (!conn->isOpen())
        return Fail("two dropped ack must still keep the transient-failure window open");

    mock.SetDropNextAck(true);
    conn->maskMouseLeft(true);
    if (conn->isOpen())
        return Fail("three consecutive dropped ack must mark kmboxNet as disconnected");

    conn->releaseAllButtons();
    if (!WaitUntil([&] { return mock.Count() >= 7; }))
        return Fail("releaseAllButtons after disconnect did not attempt release commands");

    return 0;
}

int main()
{
    try
    {
        return RunMain();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[kmboxNet_protocol_test] unhandled exception: " << e.what() << std::endl;
        return 2;
    }
}
