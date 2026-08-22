#include "KmboxAConnection.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

#include "kmboxA.h"

bool KmboxAConnection::parsePidVid(const std::string& pidvid, unsigned short& pid, unsigned short& vid)
{
    std::string hex;
    hex.reserve(pidvid.size());

    for (unsigned char c : pidvid)
    {
        if (std::isxdigit(c))
        {
            hex.push_back(static_cast<char>(std::toupper(c)));
        }
    }

    if (hex.size() != 8)
    {
        return false;
    }

    try
    {
        // Single field format: PIDVID (PPPPVVVV).
        pid = static_cast<unsigned short>(std::stoul(hex.substr(0, 4), nullptr, 16));
        vid = static_cast<unsigned short>(std::stoul(hex.substr(4, 4), nullptr, 16));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

KmboxAConnection::KmboxAConnection(const std::string& pidvid)
    : is_open_(false)
{
    unsigned short pid = 0;
    unsigned short vid = 0;

    if (!parsePidVid(pidvid, pid, vid))
    {
        std::cerr << "[KmboxA] Invalid PIDVID format. Expected 8 hex chars (PPPPVVVV)." << std::endl;
        return;
    }

    const int ret = KM_init(vid, pid);
    is_open_ = (ret == 0);
    if (!is_open_)
    {
        std::cerr << "[KmboxA] Connection failed, ret=" << ret << " (VID=0x"
            << std::hex << std::uppercase << vid << ", PID=0x" << pid << std::dec << ")" << std::endl;
    }
}

KmboxAConnection::~KmboxAConnection()
{
    if (!is_open_) return;
    KM_close();
    is_open_ = false;
}

bool KmboxAConnection::move(int x, int y)
{
    if (!is_open_) return false;
    // KM_move 形参为 short。直接窄化会在 |delta| > 32767 时回绕成反向位移
    // （例如 32768 -> -32768，准星瞬间甩向相反方向）。此处先钳制再窄化。
    // 修复：下限统一为 -32767，与 Win32 SendInput（mouse::toMoveCount）及
    // kmboxNet 后端对齐，避免不同硬件后端在 short::min 一侧行为分叉。
    const int clampedX = std::clamp(x, -32767, 32767);
    const int clampedY = std::clamp(y, -32767, 32767);
    return KM_move(static_cast<short>(clampedX), static_cast<short>(clampedY)) == 0;
}

bool KmboxAConnection::leftDown()
{
    if (!is_open_) return false;
    return KM_left(1) == 0;
}

bool KmboxAConnection::leftUp()
{
    if (!is_open_) return false;
    return KM_left(0) == 0;
}

bool KmboxAConnection::rightDown()
{
    if (!is_open_) return false;
    return KM_right(1) == 0;
}

bool KmboxAConnection::rightUp()
{
    if (!is_open_) return false;
    return KM_right(0) == 0;
}

bool KmboxAConnection::middleDown()
{
    if (!is_open_) return false;
    return KM_middle(1) == 0;
}

bool KmboxAConnection::middleUp()
{
    if (!is_open_) return false;
    return KM_middle(0) == 0;
}

bool KmboxAConnection::wheel(int delta)
{
    if (!is_open_) return false;
    const int clamped = std::clamp(delta, -127, 127);
    const signed char wheel_delta = static_cast<signed char>(clamped);
    return KM_wheel(static_cast<unsigned char>(wheel_delta)) == 0;
}
