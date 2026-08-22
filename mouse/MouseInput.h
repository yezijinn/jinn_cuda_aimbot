#ifndef MOUSE_INPUT_H
#define MOUSE_INPUT_H

#include <memory>
#include <optional>
#include <string>

class Config;
class KmboxAConnection;
class KmboxNetConnection;
class MakcuConnection;

enum class MouseInputMethod
{
    Win32,
    KmboxNet,
    KmboxA,
    Makcu
};

std::optional<MouseInputMethod> ParseMouseInputMethod(const std::string& method);
std::string MouseInputMethodName(MouseInputMethod method);

class IMouseInput
{
public:
    virtual ~IMouseInput() = default;

    virtual const char* name() const = 0;
    virtual bool isOpen() const = 0;
    virtual bool isConnecting() const { return false; }
    virtual bool move(int dx, int dy) = 0;
    virtual bool leftDown() = 0;
    virtual bool leftUp() = 0;
    virtual bool rightDown() { return false; }
    virtual bool rightUp() { return false; }
    virtual void releaseAllButtons() {}
    virtual bool hasPhysicalButtonState() const { return false; }
    virtual bool keyPressed(const std::string& keyName) { (void)keyName; return false; }
    virtual bool aimingActive() const { return false; }
    virtual bool shootingActive() const { return false; }
    virtual bool zoomingActive() const { return false; }

    virtual KmboxNetConnection* kmboxNet() { return nullptr; }
    virtual KmboxAConnection* kmboxA() { return nullptr; }
    virtual MakcuConnection* makcu() { return nullptr; }
};

std::unique_ptr<IMouseInput> CreateMouseInputDevice(const Config& config);

#endif // MOUSE_INPUT_H
