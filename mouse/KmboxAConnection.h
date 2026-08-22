#pragma once

#include <string>

class KmboxAConnection
{
public:
    explicit KmboxAConnection(const std::string& pidvid);
    ~KmboxAConnection();

    // 本类独占 kmboxA 的 HID 设备句柄（析构会调用 KM_close）。一旦被拷贝，
    // 两个副本都会在析构时 KM_close 同一设备，第二次将作用在已释放的句柄上。
    // 显式禁用拷贝与移动，让误用在编译期暴露。
    KmboxAConnection(const KmboxAConnection&) = delete;
    KmboxAConnection& operator=(const KmboxAConnection&) = delete;
    KmboxAConnection(KmboxAConnection&&) = delete;
    KmboxAConnection& operator=(KmboxAConnection&&) = delete;

    bool isOpen() const { return is_open_; }

    bool move(int x, int y);
    bool leftDown();
    bool leftUp();
    bool rightDown();
    bool rightUp();
    bool middleDown();
    bool middleUp();
    bool wheel(int delta);

private:
    static bool parsePidVid(const std::string& pidvid, unsigned short& pid, unsigned short& vid);

    bool is_open_;
};
