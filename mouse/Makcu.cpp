#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <iostream>

#include "Makcu.h"
#include "mybot.h"

namespace
{
// 将 IMouseInput 层使用的按钮序号映射到 MAKCU SDK 枚举。
// 序号取值与 makcu::MouseButton 的底层值一一对应
// (0=LEFT, 1=RIGHT, 2=MIDDLE, 3=SIDE1, 4=SIDE2)，此处显式转换以避免
// 未来枚举调整导致的静默错位；未知序号回退为 LEFT，与历史行为保持一致。
makcu::MouseButton ToMakcuButton(int button)
{
    switch (button)
    {
    case 1:  return makcu::MouseButton::RIGHT;
    case 2:  return makcu::MouseButton::MIDDLE;
    case 3:  return makcu::MouseButton::SIDE1;
    case 4:  return makcu::MouseButton::SIDE2;
    case 0:
    default: return makcu::MouseButton::LEFT;
    }
}
}

MakcuConnection::MakcuConnection(const std::string& port, unsigned int baud_rate)
    : is_open_(false)
    , aiming_active(false)
    , shooting_active(false)
    , zooming_active(false)
{
    try
    {
        device_.setMouseButtonCallback([this](makcu::MouseButton button, bool pressed) {
            onButtonCallback(button, pressed);
        });

        device_.enableButtonMonitoring(true);

        if (device_.connect(port))
        {
            device_.enableHighPerformanceMode(true);

            constexpr unsigned int sdkHighSpeedBaud = 4000000;
            if (baud_rate > 0 && baud_rate != sdkHighSpeedBaud)
            {
                std::cout << "[Makcu] Ignoring configured baud rate " << baud_rate
                    << "; the MAKCU SDK connection is already running at "
                    << sdkHighSpeedBaud << " baud." << std::endl;
            }

            is_open_ = true;
            std::cout << "[Makcu] Connected! PORT: " << port << std::endl;
        }
        else
        {
            std::cerr << "[Makcu] Unable to connect to the port: " << port << std::endl;
        }
    }
    catch (const makcu::MakcuException& e)
    {
        std::cerr << "[Makcu] Error: " << e.what() << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Makcu] Error: " << e.what() << std::endl;
    }
}

MakcuConnection::~MakcuConnection()
{
    try
    {
        device_.disconnect();
    }
    catch (...)
    {
    }
    is_open_ = false;
}

bool MakcuConnection::isOpen() const
{
    return is_open_ && device_.isConnected();
}

bool MakcuConnection::move(int x, int y)
{
    if (!is_open_)
        return false;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        return device_.mouseMove(x, y);
    }
    catch (...)
    {
        is_open_ = false;
        return false;
    }
}

bool MakcuConnection::click(int button)
{
    if (!is_open_)
        return false;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        return device_.click(ToMakcuButton(button));
    }
    catch (...)
    {
        is_open_ = false;
        return false;
    }
}

bool MakcuConnection::press(int button)
{
    if (!is_open_)
        return false;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        return device_.mouseDown(ToMakcuButton(button));
    }
    catch (...)
    {
        is_open_ = false;
        return false;
    }
}

bool MakcuConnection::release(int button)
{
    if (!is_open_)
        return false;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        return device_.mouseUp(ToMakcuButton(button));
    }
    catch (...)
    {
        is_open_ = false;
        return false;
    }
}

void MakcuConnection::onButtonCallback(makcu::MouseButton button, bool pressed)
{
    switch (button)
    {
    case makcu::MouseButton::LEFT:
        // LMB = shooting
        shooting_active = pressed;
        shooting.store(pressed);
        break;

    case makcu::MouseButton::RIGHT:
        // RMB = zooming
        zooming_active = pressed;
        zooming.store(pressed);
        break;

    case makcu::MouseButton::MIDDLE:
        // MMB - not used for now
        break;

    case makcu::MouseButton::SIDE1:
        // Mouse4 (side button 1) - not used
        break;

    case makcu::MouseButton::SIDE2:
        // Mouse5 (side button 2) = aiming
        aiming_active = pressed;
        aiming.store(pressed);
        break;
    }
}
