#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "KmboxNetConnection.h"

namespace
{

int Fail(const std::string& message)
{
    std::cerr << "[kmboxNet_live_smoke_test] FAIL: " << message << std::endl;
    return 1;
}

bool NeedValue(int argc, char** argv, int i, const char* name, std::string& out)
{
    if (i + 1 >= argc)
    {
        std::cerr << "[kmboxNet_live_smoke_test] missing value for " << name << std::endl;
        return false;
    }
    out = argv[i + 1];
    return true;
}

void PrintUsage(const char* program)
{
    std::cout << "Usage: " << program
              << " [--ip 192.168.2.188] [--port 8808] --uuid UUID\n"
              << "Default parameters are the kmboxNet box defaults.\n"
              << "Supply the UUID shown by your device; do not commit a device-specific UUID.\n"
              << "Keep all physical mouse buttons released during the test.\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::string ip = "192.168.2.188";
    std::string port = "8808";
    std::string uuid;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--ip")
        {
            if (!NeedValue(argc, argv, i, "--ip", ip))
                return Fail("invalid --ip");
            ++i;
        }
        else if (arg == "--port")
        {
            if (!NeedValue(argc, argv, i, "--port", port))
                return Fail("invalid --port");
            ++i;
        }
        else if (arg == "--uuid")
        {
            if (!NeedValue(argc, argv, i, "--uuid", uuid))
                return Fail("invalid --uuid");
            ++i;
        }
        else if (arg == "--help" || arg == "-h")
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else
        {
            return Fail("unknown argument: " + arg);
        }
    }

    if (uuid.empty())
        return Fail("missing --uuid; use the UUID shown by the device");

    try
    {
        KmboxNetConnection conn(ip, port, uuid);
        if (!conn.isOpen())
            return Fail("connection init failed");

        std::cout << "[kmboxNet_live_smoke_test] connected: "
                  << ip << ":" << port << " uuid=" << uuid << std::endl;

        const int left = conn.monitorMouseLeft();
        const int right = conn.monitorMouseRight();
        const int middle = conn.monitorMouseMiddle();
        const int side1 = conn.monitorMouseSide1();
        const int side2 = conn.monitorMouseSide2();

        std::cout << "[kmboxNet_live_smoke_test] monitor states left=" << left
                  << " right=" << right << " middle=" << middle
                  << " side1=" << side1 << " side2=" << side2 << std::endl;

        if (left == -1 || right == -1 || middle == -1 || side1 == -1 || side2 == -1)
            return Fail("physical mouse monitor is unavailable; kmboxNet may not be healthy");
        if (left != 0 || right != 0 || middle != 0 || side1 != 0 || side2 != 0)
            return Fail("physical mouse buttons are pressed; release them before running this test");

        const bool kLeftOk = conn.leftDown();
        const bool kRightOk = conn.rightDown();
        const bool kMiddleOk = conn.middleDown();
        const bool kSide1Ok = conn.side1Down();
        const bool kSide2Ok = conn.side2Down();

        if (!kLeftOk || !kRightOk || !kMiddleOk || !kSide1Ok || !kSide2Ok)
            return Fail("mixed button down command failed");
        std::cout << "[kmboxNet_live_smoke_test] mixed button down ok" << std::endl;

        if (!conn.move(0, 0))
            return Fail("move(0,0) while multiple buttons held failed");
        std::cout << "[kmboxNet_live_smoke_test] move(0,0) ok" << std::endl;

        conn.releaseAllButtons();
        if (!conn.isOpen())
            return Fail("connection closed unexpectedly after releaseAllButtons");
        std::cout << "[kmboxNet_live_smoke_test] mixed button release ok" << std::endl;

        if (!conn.move(0, 0))
            return Fail("move(0,0) after release failed");
        if (!conn.isOpen())
            return Fail("connection closed unexpectedly after final move");

        std::cout << "[kmboxNet_live_smoke_test] PASS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("unhandled exception: ") + e.what());
    }
    catch (...)
    {
        return Fail("unhandled unknown exception");
    }
}
