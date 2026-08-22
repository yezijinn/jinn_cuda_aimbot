#ifndef MAKCU_CONNECTION_H
#define MAKCU_CONNECTION_H

#include <string>
#include <atomic>
#include <mutex>

#include "../modules/makcu/include/makcu.h"

class MakcuConnection
{
public:
    MakcuConnection(const std::string& port, unsigned int baud_rate);
    ~MakcuConnection();

    bool isOpen() const;

    bool click(int button);
    bool press(int button);
    bool release(int button);
    bool move(int x, int y);

    // 这三个标志由 MAKCU SDK 的按键监听线程写入 (onButtonCallback)，
    // 由鼠标线程 / overlay 线程读取，必须是原子类型，否则构成数据竞争 (UB)。
    // 读取方 MouseInput.cpp 使用隐式转换 (operator bool)，语义与原先一致。
    std::atomic<bool> aiming_active;
    std::atomic<bool> shooting_active;
    std::atomic<bool> zooming_active;

private:
    void onButtonCallback(makcu::MouseButton button, bool pressed);

private:
    makcu::Device device_;
    std::atomic<bool> is_open_;
    std::mutex write_mutex_;
};

#endif // MAKCU_CONNECTION_H
