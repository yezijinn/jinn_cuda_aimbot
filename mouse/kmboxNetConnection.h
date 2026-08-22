#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <thread>

#include "kmbox_net/kmboxNet.h"

class KmboxNetConnection
{
public:
    KmboxNetConnection(const std::string& ip, const std::string& port, const std::string& uuid);
    ~KmboxNetConnection();

    // 本类独占 kmboxNet SDK 的全局套接字（析构会调用 kmNet_close）。
    // 一旦被拷贝，两个副本都会在析构时关闭同一条连接，第二次 kmNet_close
    // 将作用在已失效/已被复用的句柄上。显式禁用拷贝与移动，让误用在编译期暴露。
    KmboxNetConnection(const KmboxNetConnection&) = delete;
    KmboxNetConnection& operator=(const KmboxNetConnection&) = delete;
    KmboxNetConnection(KmboxNetConnection&&) = delete;
    KmboxNetConnection& operator=(KmboxNetConnection&&) = delete;

    void monitorThread();
    bool isOpen() const { return is_open_; }

    bool move(int x, int y);
    bool moveAuto(int x, int y, int ms);
    bool moveBezier(int x, int y, int ms, int x1, int y1, int x2, int y2);
    bool leftDown();
    bool leftUp();
    bool rightDown();
    bool rightUp();
    bool side1Down();
    bool side1Up();
    bool side2Down();
    bool side2Up();
    bool middleDown();
    bool middleUp();
    bool wheel(int wheel);
    bool mouseAll(int button, int x, int y, int wheel);
    void releaseAllButtons();

    void keyDown(int vkey);
    void keyUp(int vkey);

    void monitor(short port);
    int monitorMouseLeft();
    int monitorMouseRight();
    int monitorMouseMiddle();
    int monitorMouseSide1();
    int monitorMouseSide2();
    int monitorKeyboard(short vkey);
    bool monitorReady() const;

    void maskMouseLeft(bool enable);
    void maskMouseRight(bool enable);
    void maskMouseMiddle(bool enable);
    void maskMouseSide1(bool enable);
    void maskMouseSide2(bool enable);
    void maskMouseX(bool enable);
    void maskMouseY(bool enable);
    void maskMouseWheel(bool enable);
    void maskKeyboard(short vkey);
    void unmaskKeyboard(short vkey);
    void unmaskAll();

    void reboot();
    void setConfig(const std::string& ip, unsigned short port);
    void debug(short port, char enable);

    void lcdColor(unsigned short rgb565);
    void lcdPictureBottom(unsigned char* buff_128_80);
    void lcdPicture(unsigned char* buff_128_160);

    std::atomic<bool> aiming_active{ false };
    std::atomic<bool> shooting_active{ false };
    std::atomic<bool> zooming_active{ false };

private:
    mutable std::mutex command_mutex_;
    std::atomic<bool> is_open_{ false };
    std::atomic<int> command_failures_{ 0 };
    bool monitor_ = false;
    std::thread monitor_thread_;
    std::atomic<bool> monitor_running_{ false };
    std::string ip_, port_, uuid_;

    void MarkDisconnectedLocked(int ret);
};
