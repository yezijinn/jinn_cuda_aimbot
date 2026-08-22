#include "../include/serialport.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <future>

#ifdef _WIN32
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#pragma comment(lib, "setupapi.lib")
#else
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <libudev.h>
#include <errno.h>
#include <poll.h>
#endif

namespace makcu {

    SerialPort::SerialPort()
        : m_baudRate(115200)
        , m_timeout(100)
        , m_isOpen(false)
#ifdef _WIN32
        , m_handle(INVALID_HANDLE_VALUE)
#else
        , m_fd(-1)
#endif
    {
#ifdef _WIN32
        memset(&m_dcb, 0, sizeof(m_dcb));
        memset(&m_timeouts, 0, sizeof(m_timeouts));
#endif
    }

    SerialPort::~SerialPort() {
        close();
    }

    bool SerialPort::open(const std::string& port, uint32_t baudRate) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_isOpen) {
            // 必须走内部版本：此处已持有 m_mutex，而 std::mutex 不可重入，
            // 调用 public 的 close() 会二次加锁造成永久自死锁。
            // 该分支在「Device::connected 已被监控线程置 false、但串口仍处于打开状态」
            // 时可达（设备掉线后重连），原实现会让调用线程整体挂死。
            closeLocked();
        }

        m_portName = port;
        m_baudRate = baudRate;

        // Unified logic with platform abstraction
#ifdef _WIN32
        std::string devicePath = "\\\\.\\" + port;
#else
        std::string devicePath = "/dev/" + port;
#endif

        if (!platformOpen(devicePath)) {
            return false;
        }

        if (!platformConfigurePort()) {
            platformClose();
            return false;
        }

        m_isOpen = true;

        // Start high-performance listener thread (shared logic)
        m_stopListener = false;
        m_listenerThread = std::thread(&SerialPort::listenerLoop, this);

        return true;
    }

    void SerialPort::close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        closeLocked();
    }

    void SerialPort::closeLocked() {
        if (!m_isOpen) {
            return;
        }

        // Stop listener thread first
        m_stopListener.store(true, std::memory_order_release);

        // 先对外声明端口已关闭，再真正释放句柄：
        // sendCommand()/sendTrackedCommand() 以 m_isOpen 作为准入判据且不持 m_mutex，
        // 原顺序（先 platformClose 再置 false）会留出一个窗口——句柄已被 CloseHandle
        // 回收、m_isOpen 仍为 true——此时鼠标线程的 WriteFile 会作用在一个可能已被
        // 进程内其它 CreateFile 复用的句柄号上。提前置位可显著收窄该窗口，
        // 同时让 listenerLoop 的 m_isOpen 判据更早生效、加快退出。
        m_isOpen.store(false, std::memory_order_release);

        // 等待监听线程退出。
        // 原实现用 std::async + wait_for(1s) 伪装成「带超时的 join」，存在三重缺陷：
        //   1) std::async 返回的 future 在析构时本身就会阻塞到任务结束，超时保护形同虚设；
        //   2) 超时分支对一个正被另一线程 join() 的 std::thread 对象并发 detach()，属数据竞争(UB)；
        //   3) detach 成功后本函数继续 platformClose() 并可能进入 ~SerialPort，
        //      而被 detach 的线程仍在 listenerLoop 中访问 this → use-after-free。
        // listenerLoop 每轮都检查 m_stopListener/m_isOpen，platformRead 的总超时为 10ms，
        // 故退出时间有界（约 ≤10ms），直接 join 是安全且更快的做法。
        if (m_listenerThread.joinable()) {
            m_listenerThread.join();
        }

        // Cancel all pending commands with double-checked locking for safety
        // First pass: mark all commands as cancelled to prevent new promise operations
        std::vector<std::unique_ptr<PendingCommand>> commandsToCancel;
        {
            std::lock_guard<std::mutex> cmdLock(m_commandMutex);
            commandsToCancel.reserve(m_pendingCommands.size());
            for (auto& [id, cmd] : m_pendingCommands) {
                commandsToCancel.push_back(std::move(cmd));
            }
            m_pendingCommands.clear();
        }
        
        // Second pass: cancel commands outside of mutex to prevent deadlock
        for (auto& cmd : commandsToCancel) {
            try {
                cmd->promise.set_exception(std::make_exception_ptr(
                    std::runtime_error("Connection closed")));
            }
            catch (...) {
                // Promise already set or moved - safe to ignore
            }
        }

        // Platform-specific cleanup
        // （m_isOpen 已在本函数入口处置 false，见上方说明）
        platformClose();

        // Reset button state
        m_lastButtonMask.store(0, std::memory_order_release);
    }

    bool SerialPort::isOpen() const {
        return m_isOpen;
    }

    bool SerialPort::isActuallyConnected() const {
        if (!m_isOpen) {
            return false;
        }

#ifdef _WIN32
        // Windows: Check if handle is still valid
        // 一次性 load 到局部量后再使用：避免"检查时有效、使用时已被 close 置无效"的
        // TOCTOU 窗口。局部副本最坏情况只是操作一个刚被关闭的句柄（API 返回失败），
        // 而不会把 INVALID_HANDLE_VALUE 之外的野值传进内核。
        const HANDLE handle = m_handle.load(std::memory_order_acquire);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        
        // Try to get comm state to verify device is still there
        DCB dcb;
        return GetCommState(handle, &dcb) != 0;
#else
        // Linux: Check if file descriptor is still valid
        if (m_fd < 0) {
            return false;
        }
        
        // Use poll to check if device is still connected
        struct pollfd pfd;
        pfd.fd = m_fd;
        pfd.events = POLLERR | POLLHUP | POLLNVAL;
        pfd.revents = 0;
        
        int result = poll(&pfd, 1, 0);  // Non-blocking check
        
        if (result < 0) {
            return false;  // Error occurred
        }
        
        // If any error conditions are set, device is disconnected
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return false;
        }
        
        return true;
#endif
    }

    std::future<std::string> SerialPort::sendTrackedCommand(const std::string& command,
        bool expectResponse,
        std::chrono::milliseconds timeout) {
        // Check port status with atomic load to prevent race conditions
        if (!m_isOpen.load(std::memory_order_acquire)) {
            std::promise<std::string> promise;
            promise.set_exception(std::make_exception_ptr(
                std::runtime_error("Port not open")));
            return promise.get_future();
        }

        // Command length validation
        constexpr size_t MAX_COMMAND_LENGTH = 512;
        if (command.length() > MAX_COMMAND_LENGTH) {
            std::promise<std::string> promise;
            promise.set_exception(std::make_exception_ptr(
                std::runtime_error("Command too long (max " + std::to_string(MAX_COMMAND_LENGTH) + " chars)")));
            return promise.get_future();
        }

        int cmdId = generateCommandId();
        auto pendingCmd = std::make_unique<PendingCommand>(cmdId, command, expectResponse, timeout);
        auto future = pendingCmd->promise.get_future();

        // Store pending command (shared logic)
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_pendingCommands[cmdId] = std::move(pendingCmd);
        }

        // Send command with ID tracking (shared logic)
        std::string trackedCommand = expectResponse ?
            command + "#" + std::to_string(cmdId) + "\r\n" :
            command + "\r\n";

        // Unified write operation
        ssize_t bytesWritten = platformWrite(trackedCommand.c_str(), trackedCommand.length());
        
        if (bytesWritten != static_cast<ssize_t>(trackedCommand.length())) {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            auto it = m_pendingCommands.find(cmdId);
            if (it != m_pendingCommands.end()) {
                try {
                    std::string errorMsg = "Write failed";
                    if (bytesWritten < 0) {
                        errorMsg += " (" + getLastPlatformError() + ")";
                    } else {
                        errorMsg += " (partial write: " + std::to_string(bytesWritten) + 
                                   "/" + std::to_string(trackedCommand.length()) + " bytes)";
                    }
                    it->second->promise.set_exception(std::make_exception_ptr(
                        std::runtime_error(errorMsg)));
                }
                catch (...) {
                    // Promise already set
                }
                m_pendingCommands.erase(it);
            }
        }
        
        // Unified flush operation
        platformFlush();

        return future;
    }

    bool SerialPort::sendCommand(const std::string& command) {
        // Check port status with atomic load to prevent race conditions
        if (!m_isOpen.load(std::memory_order_acquire)) {
            return false;
        }

        // Command length validation
        constexpr size_t MAX_COMMAND_LENGTH = 512;
        if (command.length() > MAX_COMMAND_LENGTH) {
            #ifdef DEBUG
            std::cerr << "SerialPort: Command too long (" << command.length() << " > " << MAX_COMMAND_LENGTH << ")" << std::endl;
            #endif
            return false;
        }

        // 复用线程局部缓冲，避免每条命令都为 "km.move(x,y)\r\n"（约 20 字节，超出 SSO）
        // 触发一次堆分配；240Hz 下这是每秒数百次可完全消除的分配。
        thread_local std::string fullCommand;
        fullCommand.assign(command);
        fullCommand.append("\r\n");

        ssize_t bytesWritten = platformWrite(fullCommand.c_str(), fullCommand.length());

        // 原实现在每条命令后调用 platformFlush()（Windows 上即 FlushFileBuffers）。
        // 串口句柄上该调用是同步阻塞的：它会一直等到驱动的发送缓冲被硬件排空才返回，
        // 且句柄是非重叠(non-overlapped)模式，内核会把它与监听线程的 ReadFile 串行化。
        // 鼠标线程每次 move 都要付这份代价（最高 240Hz），是可测的输入延迟来源。
        // WriteFile 返回时数据已交付驱动，不会丢失，flush 仅影响「何时确认送达」，
        // 对 fire-and-forget 语义无意义，故移除。写入完整性判定保持不变。
        return bytesWritten == static_cast<ssize_t>(fullCommand.length());
    }

    void SerialPort::listenerLoop() {
        // Optimized read buffers (shared logic)
        std::vector<uint8_t> readBuffer(BUFFER_SIZE);
        std::vector<uint8_t> lineBuffer(LINE_BUFFER_SIZE);
        size_t linePos = 0;

        auto lastCleanup = std::chrono::steady_clock::now();
        constexpr auto cleanupInterval = std::chrono::milliseconds(50);

        while (!m_stopListener && m_isOpen.load()) {
            try {
                // 超时清理必须放在循环最顶部、位于任何 continue 之前。
                // 原实现把它放在循环体末尾，而下面两个早退分支（无数据可读 / 读取失败）都会
                // continue 跳过它。cleanupTimedOutCommands 是 m_pendingCommands 唯一的超时
                // 兜底机制，而"设备静默、拔线、固件卡死"这些最需要超时的场景，恰恰就是一直
                // 命中早退分支的场景 —— 结果是 pending 条目永不超时：
                //   · sendTrackedCommand 的调用方在 future.get() 上永久挂死（UI 线程直接假死）；
                //   · m_pendingCommands 无界增长，长时间运行后内存持续上涨。
                // 提到顶部后，无论本轮是否读到数据，超时检查都以固定 50ms 周期执行。
                auto now = std::chrono::steady_clock::now();
                if (now - lastCleanup > cleanupInterval) {
                    cleanupTimedOutCommands();
                    lastCleanup = now;
                }

                // Unified bytes available check
                size_t bytesAvailable = platformBytesAvailable();
                if (bytesAvailable == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    continue;
                }

                // Unified read operation
                size_t bytesToRead = std::min<size_t>(bytesAvailable, static_cast<size_t>(BUFFER_SIZE));
                ssize_t bytesRead = platformRead(readBuffer.data(), bytesToRead);
                
                if (bytesRead <= 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                
                // Shared byte processing logic
                for (ssize_t i = 0; i < bytesRead; ++i) {
                    uint8_t byte = readBuffer[i];

                    // Handle button data (non-printable characters < 32, except CR/LF)
                    if (byte < 32 && byte != 0x0D && byte != 0x0A) {
                        handleButtonData(byte);
                    }
                    else {
                        // Handle text response data
                        if (byte == 0x0A) { // Line feed
                            if (linePos > 0) {
                                std::string line(lineBuffer.begin(), lineBuffer.begin() + linePos);
                                linePos = 0;
                                if (!line.empty()) {
                                    processResponse(line);
                                }
                            }
                        }
                        else if (byte != 0x0D) { // Ignore carriage return
                            if (linePos < LINE_BUFFER_SIZE - 1) {
                                lineBuffer[linePos++] = byte;
                            } else {
                                // Buffer overflow protection - discard line and reset
                                #ifdef DEBUG
                                std::cerr << "SerialPort: Line buffer overflow, discarding data" << std::endl;
                                #endif
                                linePos = 0;
                            }
                        }
                    }
                }

                // 周期性超时清理已上移至循环顶部，此处不再重复执行。

            }
            catch (const std::exception& e) {
                // Log specific exception for debugging but continue running
                // In production, you might want to use a proper logging framework
                #ifdef DEBUG
                std::cerr << "SerialPort listener exception: " << e.what() << std::endl;
                #else
                (void)e;  // Release 构建下 e 未被引用，显式丢弃以消除 C4101 警告
                #endif
                
                // Brief pause to prevent tight exception loops
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                
                // Check if port is still open after exception
                if (!m_isOpen.load(std::memory_order_acquire)) {
                    // Port was closed, exit gracefully
                    break;
                }
            }
            catch (...) {
                // Unknown exception - be more cautious
                #ifdef DEBUG
                std::cerr << "SerialPort listener unknown exception" << std::endl;
                #endif
                
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                
                // Check if port is still open after unknown exception
                if (!m_isOpen.load(std::memory_order_acquire)) {
                    // Port was closed, exit gracefully
                    break;
                }
            }
        }
    }

    void SerialPort::handleButtonData(uint8_t data) {
        uint8_t lastMask = m_lastButtonMask.load();
        if (data == lastMask) {
            return; // No change
        }

        m_lastButtonMask.store(data);

        if (m_buttonCallback) {
            // Only process changed bits
            uint8_t changedBits = data ^ lastMask;
            for (int bit = 0; bit < 5; ++bit) {
                if (changedBits & (1 << bit)) {
                    bool isPressed = data & (1 << bit);
                    try {
                        m_buttonCallback(bit, isPressed);
                    }
                    catch (...) {
                        // Ignore callback exceptions
                    }
                }
            }
        }
    }

    void SerialPort::processResponse(const std::string& response) {
        // Remove ">>> " prefix if present
        std::string content = response;
        if (content.substr(0, 4) == ">>> ") {
            content = content.substr(4);
        }

        // Check for command ID correlation
        size_t hashPos = content.find('#');
        if (hashPos != std::string::npos) {
            // Extract command ID
            std::string idStr = content.substr(hashPos + 1);
            size_t colonPos = idStr.find(':');
            if (colonPos != std::string::npos) {
                try {
                    int cmdId = std::stoi(idStr.substr(0, colonPos));
                    std::string result = idStr.substr(colonPos + 1);

                    std::lock_guard<std::mutex> lock(m_commandMutex);
                    auto it = m_pendingCommands.find(cmdId);
                    if (it != m_pendingCommands.end()) {
                        try {
                            it->second->promise.set_value(result);
                        }
                        catch (...) {
                            // Promise already set
                        }
                        m_pendingCommands.erase(it);
                    }
                    return;
                }
                catch (...) {
                    // Failed to parse ID, treat as normal response
                }
            }
        }

        // 无 "#id:" 前缀的裸响应：按"最早发出且确实在等响应的命令"来配对。
        //
        // 原实现直接取 m_pendingCommands.begin()，注释写的是 "oldest pending command"，
        // 但 m_pendingCommands 是 std::unordered_map，begin() 返回的是首个非空哈希桶里的
        // 元素，其顺序由 hash(command_id) 决定，与插入先后完全无关。后果是响应张冠李戴：
        // 例如 connect() 里先发 km.buttons()（其 future 被丢弃、条目滞留在表中），再发
        // km.version()，设备回的版本号有很大概率被塞给 buttons 那条，于是 version() 拿不到
        // 值、只能等到超时抛异常，表现为"设备明明连上了却报握手失败"。
        // 这里改为线性扫描取 timestamp 最小者。pending 表在正常时序下只有个位数条目
        // （超时清理周期 50ms），线性扫描的开销可以忽略，且不改变任何对外接口。
        //
        // 同时跳过 expect_response == false 的条目：这类命令是 fire-and-forget，本就不该
        // 参与配对，否则会把别人的响应吞掉，让真正等待的命令饿死到超时。
        std::lock_guard<std::mutex> lock(m_commandMutex);
        auto oldest = m_pendingCommands.end();
        for (auto it = m_pendingCommands.begin(); it != m_pendingCommands.end(); ++it) {
            if (!it->second->expect_response) {
                continue;
            }
            if (oldest == m_pendingCommands.end() ||
                it->second->timestamp < oldest->second->timestamp) {
                oldest = it;
            }
        }

        if (oldest != m_pendingCommands.end()) {
            try {
                oldest->second->promise.set_value(content);
            }
            catch (...) {
                // Promise already set
            }
            m_pendingCommands.erase(oldest);
        }
    }

    void SerialPort::cleanupTimedOutCommands() {
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(m_commandMutex);
        auto it = m_pendingCommands.begin();
        while (it != m_pendingCommands.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second->timestamp);

            if (elapsed > it->second->timeout) {
                try {
                    it->second->promise.set_exception(std::make_exception_ptr(
                        std::runtime_error("Command timeout")));
                }
                catch (...) {
                    // Promise already set
                }
                it = m_pendingCommands.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    int SerialPort::generateCommandId() {
        return (m_commandCounter.fetch_add(1) % 10000) + 1;
    }


    void SerialPort::setButtonCallback(ButtonCallback callback) {
        m_buttonCallback = callback;
    }

    // Legacy compatibility methods
    bool SerialPort::setBaudRate(uint32_t baudRate) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_baudRate = baudRate;
        
        if (m_isOpen) {
            // Unified approach - reconfigure port with new baud rate
            return platformConfigurePort();
        }
        return true;
    }

    uint32_t SerialPort::getBaudRate() const {
        return m_baudRate;
    }

    std::string SerialPort::getPortName() const {
        return m_portName;
    }

    bool SerialPort::write(const std::vector<uint8_t>& data) {
        return sendCommand(std::string(data.begin(), data.end()));
    }

    bool SerialPort::write(const std::string& data) {
        return sendCommand(data);
    }

    std::vector<uint8_t> SerialPort::read(size_t maxBytes) {
        // This is a legacy method - not recommended for high performance
        std::vector<uint8_t> buffer;
        if (!m_isOpen || maxBytes == 0) {
            return buffer;
        }

        // Unified read operation
        buffer.resize(maxBytes);
        ssize_t bytesRead = platformRead(buffer.data(), maxBytes);
        if (bytesRead > 0) {
            buffer.resize(bytesRead);
        }
        else {
            buffer.clear();
        }

        return buffer;
    }

    std::string SerialPort::readString(size_t maxBytes) {
        auto data = read(maxBytes);
        return std::string(data.begin(), data.end());
    }

    size_t SerialPort::available() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isOpen) {
            return 0;
        }

        // Unified bytes available check
        return const_cast<SerialPort*>(this)->platformBytesAvailable();
    }

    bool SerialPort::flush() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isOpen) {
            return false;
        }

        // Unified flush operation
        return platformFlush();
    }

    void SerialPort::setTimeout(uint32_t timeoutMs) {
        m_timeout = timeoutMs;
        if (m_isOpen) {
            platformUpdateTimeouts();
        }
    }

    uint32_t SerialPort::getTimeout() const {
        return m_timeout;
    }

    std::vector<std::string> SerialPort::getAvailablePorts() {
        std::vector<std::string> ports;

#ifdef _WIN32
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char valueName[256];
            char data[256];
            DWORD valueNameSize, dataSize, dataType;
            DWORD index = 0;

            while (true) {
                valueNameSize = sizeof(valueName);
                dataSize = sizeof(data);

                LONG result = RegEnumValueA(hKey, index++, valueName, &valueNameSize,
                    nullptr, &dataType,
                    reinterpret_cast<BYTE*>(data), &dataSize);

                if (result == ERROR_NO_MORE_ITEMS) {
                    break;
                }

                if (result == ERROR_SUCCESS && dataType == REG_SZ) {
                    ports.emplace_back(data);
                }
            }

            RegCloseKey(hKey);
        }
#else
        // Linux implementation - scan /dev for tty devices
        DIR* dir = opendir("/dev");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name(entry->d_name);
                if (name.substr(0, 6) == "ttyUSB" || name.substr(0, 6) == "ttyACM") {
                    ports.push_back(name);
                }
            }
            closedir(dir);
        }
#endif

        std::sort(ports.begin(), ports.end());
        return ports;
    }

    std::vector<std::string> SerialPort::findMakcuPorts() {
        std::vector<std::string> makcuPorts;

#ifdef _WIN32
        auto allPorts = getAvailablePorts();
        HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS,
            nullptr, nullptr, DIGCF_PRESENT);
        if (deviceInfoSet == INVALID_HANDLE_VALUE) {
            return makcuPorts;
        }

        SP_DEVINFO_DATA deviceInfoData;
        deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData); i++) {
            char description[256] = { 0 };
            char portName[256] = { 0 };

            if (SetupDiGetDeviceRegistryPropertyA(deviceInfoSet, &deviceInfoData,
                SPDRP_DEVICEDESC, nullptr,
                reinterpret_cast<BYTE*>(description),
                sizeof(description), nullptr)) {
                std::string desc(description);

                if (desc.find("USB-Enhanced-SERIAL CH343") != std::string::npos ||
                    desc.find("USB-SERIAL CH340") != std::string::npos) {

                    HKEY hDeviceKey = SetupDiOpenDevRegKey(deviceInfoSet, &deviceInfoData,
                        DICS_FLAG_GLOBAL, 0,
                        DIREG_DEV, KEY_READ);
                    if (hDeviceKey != INVALID_HANDLE_VALUE) {
                        DWORD portNameSize = sizeof(portName);

                        if (RegQueryValueExA(hDeviceKey, "PortName", nullptr, nullptr,
                            reinterpret_cast<BYTE*>(portName),
                            &portNameSize) == ERROR_SUCCESS) {
                            std::string port(portName);
                            if (std::find(allPorts.begin(), allPorts.end(), port) != allPorts.end()) {
                                makcuPorts.emplace_back(port);
                            }
                        }
                        RegCloseKey(hDeviceKey);
                    }
                }
            }
        }

        SetupDiDestroyDeviceInfoList(deviceInfoSet);
#else
        // Linux implementation using udev to find MAKCU devices
        struct udev* udev = udev_new();
        if (!udev) {
            return makcuPorts;
        }
        
        struct udev_enumerate* enumerate = udev_enumerate_new(udev);
        udev_enumerate_add_match_subsystem(enumerate, "tty");
        udev_enumerate_scan_devices(enumerate);
        
        struct udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
        struct udev_list_entry* entry;
        
        udev_list_entry_foreach(entry, devices) {
            const char* path = udev_list_entry_get_name(entry);
            struct udev_device* dev = udev_device_new_from_syspath(udev, path);
            
            if (dev) {
                struct udev_device* parent = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
                if (parent) {
                    const char* idVendor = udev_device_get_sysattr_value(parent, "idVendor");
                    const char* idProduct = udev_device_get_sysattr_value(parent, "idProduct");
                    
                    // Check for MAKCU device (VID:PID = 1A86:55D3)
                    bool isMakcuDevice = false;
                    
                    // Primary check: VID/PID match
                    if (idVendor && idProduct && 
                        strcmp(idVendor, "1a86") == 0 && strcmp(idProduct, "55d3") == 0) {
                        isMakcuDevice = true;
                    }
                    
                    // Backup check: Description strings (like Windows implementation)
                    if (!isMakcuDevice) {
                        const char* product = udev_device_get_sysattr_value(parent, "product");
                        if (product) {
                            std::string productStr(product);
                            if (productStr.find("USB-Enhanced-SERIAL CH343") != std::string::npos ||
                                productStr.find("USB-SERIAL CH340") != std::string::npos) {
                                isMakcuDevice = true;
                            }
                        }
                    }
                    
                    if (isMakcuDevice) {
                        const char* devNode = udev_device_get_devnode(dev);
                        if (devNode) {
                            std::string portName = std::string(devNode).substr(5); // Remove "/dev/" prefix
                            makcuPorts.push_back(portName);
                        }
                    }
                }
                udev_device_unref(dev);
            }
        }
        
        udev_enumerate_unref(enumerate);
        udev_unref(udev);
#endif

        std::sort(makcuPorts.begin(), makcuPorts.end());
        makcuPorts.erase(std::unique(makcuPorts.begin(), makcuPorts.end()), makcuPorts.end());
        return makcuPorts;
    }

    // Platform abstraction helper implementations
    bool SerialPort::platformOpen(const std::string& devicePath) {
#ifdef _WIN32
        const HANDLE handle = CreateFileA(
            devicePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        // release 序发布：保证句柄数值对随后启动的监听线程可见
        m_handle.store(handle, std::memory_order_release);
        return handle != INVALID_HANDLE_VALUE;
#else
        m_fd = ::open(devicePath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        return m_fd >= 0;
#endif
    }

    void SerialPort::platformClose() {
#ifdef _WIN32
        // exchange 原子地"取出旧值并置为无效"，保证即使 platformClose 被并发调用两次，
        // CloseHandle 也只会对同一句柄执行一次。原先的"先判断再关闭再赋值"三步非原子，
        // 双重关闭会命中一个已被内核回收复用的句柄号，误关掉本进程里其他无关的对象
        // （典型是 D3D 资源或另一个串口），故障现象与串口毫无关联、极难定位。
        const HANDLE handle = m_handle.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
#else
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

    bool SerialPort::platformConfigurePort() {
#ifdef _WIN32
        m_dcb.DCBlength = sizeof(DCB);

        if (!GetCommState(m_handle.load(std::memory_order_acquire), &m_dcb)) {
            return false;
        }

        m_dcb.BaudRate = m_baudRate;
        m_dcb.ByteSize = 8;
        m_dcb.Parity = NOPARITY;
        m_dcb.StopBits = ONESTOPBIT;
        m_dcb.fBinary = TRUE;
        m_dcb.fParity = FALSE;
        m_dcb.fOutxCtsFlow = FALSE;
        m_dcb.fOutxDsrFlow = FALSE;
        m_dcb.fDtrControl = DTR_CONTROL_DISABLE;
        m_dcb.fDsrSensitivity = FALSE;
        m_dcb.fTXContinueOnXoff = FALSE;
        m_dcb.fOutX = FALSE;
        m_dcb.fInX = FALSE;
        m_dcb.fErrorChar = FALSE;
        m_dcb.fNull = FALSE;
        m_dcb.fRtsControl = RTS_CONTROL_DISABLE;
        m_dcb.fAbortOnError = FALSE;

        if (!SetCommState(m_handle.load(std::memory_order_acquire), &m_dcb)) {
            return false;
        }

        platformUpdateTimeouts();
        return true;
#else
        // Linux implementation using termios
        if (tcgetattr(m_fd, &m_oldTermios) != 0) {
            return false;
        }
        
        m_newTermios = m_oldTermios;
        
        // Configure serial port settings to match Windows DCB equivalent
        // Control flags - match Windows DCB settings
        m_newTermios.c_cflag &= ~PARENB;    // No parity (DCB.fParity = FALSE)
        m_newTermios.c_cflag &= ~CSTOPB;    // One stop bit (DCB.StopBits = ONESTOPBIT)
        m_newTermios.c_cflag &= ~CSIZE;     // Clear data size bits
        m_newTermios.c_cflag |= CS8;        // 8 data bits (DCB.ByteSize = 8)
        m_newTermios.c_cflag &= ~CRTSCTS;   // No hardware flow control (DCB.fRtsControl/fOutxCtsFlow = FALSE)
        m_newTermios.c_cflag |= CREAD | CLOCAL; // Enable receiver, ignore modem lines (DCB.fDtrControl = DISABLE)
        
        // Local flags - raw input processing like Windows binary mode
        m_newTermios.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // Raw input (DCB.fBinary = TRUE equivalent)
        m_newTermios.c_lflag &= ~(ECHOK | ECHONL | IEXTEN);      // Additional echo/processing disable
        
        // Output flags - raw output like Windows
        m_newTermios.c_oflag &= ~OPOST;     // Raw output (no post-processing)
        m_newTermios.c_oflag &= ~(ONLCR | OCRNL | ONOCR | ONLRET); // No line ending conversions
        
        // Input flags - match Windows flow control settings
        m_newTermios.c_iflag &= ~(IXON | IXOFF | IXANY); // No software flow control (DCB.fOutX/fInX = FALSE)
        m_newTermios.c_iflag &= ~(INLCR | ICRNL | IGNCR); // No line ending conversion
        m_newTermios.c_iflag &= ~(ISTRIP | INPCK);        // No parity stripping (DCB.fParity = FALSE)
        m_newTermios.c_iflag &= ~(BRKINT | IGNBRK);       // Break handling like Windows
        
        // Set timeouts - gaming-optimized to match Windows (10ms equivalent)
        m_newTermios.c_cc[VMIN] = 0;        // Non-blocking read
        m_newTermios.c_cc[VTIME] = 1;       // 0.1 second timeout (minimum granularity)
        
        // Set baud rate
        speed_t speed;
        switch (m_baudRate) {
            case 9600:   speed = B9600; break;
            case 19200:  speed = B19200; break;
            case 38400:  speed = B38400; break;
            case 57600:  speed = B57600; break;
            case 115200: speed = B115200; break;
            case 230400: speed = B230400; break;
            case 460800: speed = B460800; break;
            case 921600: speed = B921600; break;
            case 1000000: speed = B1000000; break;
            case 1152000: speed = B1152000; break;
            case 1500000: speed = B1500000; break;
            case 2000000: speed = B2000000; break;
            case 2500000: speed = B2500000; break;
            case 3000000: speed = B3000000; break;
            case 3500000: speed = B3500000; break;
            case 4000000: speed = B4000000; break;
            default:     speed = B115200; break;
        }
        
        cfsetispeed(&m_newTermios, speed);
        cfsetospeed(&m_newTermios, speed);
        
        if (tcsetattr(m_fd, TCSANOW, &m_newTermios) != 0) {
            return false;
        }
        
        // Flush any existing data
        tcflush(m_fd, TCIOFLUSH);
        
        return true;
#endif
    }

    void SerialPort::platformUpdateTimeouts() {
#ifdef _WIN32
        // Gaming-optimized timeouts - much faster than original
        m_timeouts.ReadIntervalTimeout = 1;          // 1ms between bytes
        m_timeouts.ReadTotalTimeoutConstant = 10;    // 10ms total read timeout
        m_timeouts.ReadTotalTimeoutMultiplier = 1;   // 1ms per byte
        m_timeouts.WriteTotalTimeoutConstant = 10;   // 10ms write timeout
        m_timeouts.WriteTotalTimeoutMultiplier = 1;  // 1ms per byte

        SetCommTimeouts(m_handle.load(std::memory_order_acquire), &m_timeouts);
#else
        // Linux gaming-optimized timeouts - match Windows performance
        if (m_isOpen && m_fd >= 0) {
            struct termios currentTermios;
            if (tcgetattr(m_fd, &currentTermios) == 0) {
                // Update timeout settings to match current m_timeout value
                // VTIME is in deciseconds (0.1s units), so convert from ms
                uint8_t vtime = std::min(255, std::max(1, static_cast<int>(m_timeout / 100)));
                currentTermios.c_cc[VTIME] = vtime;
                currentTermios.c_cc[VMIN] = 0;  // Non-blocking
                tcsetattr(m_fd, TCSANOW, &currentTermios);
            }
        }
#endif
    }

    ssize_t SerialPort::platformWrite(const void* data, size_t length) {
#ifdef _WIN32
        DWORD bytesWritten = 0;
        bool success = WriteFile(m_handle.load(std::memory_order_acquire), data, static_cast<DWORD>(length), &bytesWritten, nullptr);
        return success ? static_cast<ssize_t>(bytesWritten) : -1;
#else
        return ::write(m_fd, data, length);
#endif
    }

    ssize_t SerialPort::platformRead(void* buffer, size_t maxBytes) {
#ifdef _WIN32
        DWORD bytesRead = 0;
        bool success = ReadFile(m_handle.load(std::memory_order_acquire), buffer, static_cast<DWORD>(maxBytes), &bytesRead, nullptr);
        return success ? static_cast<ssize_t>(bytesRead) : -1;
#else
        return ::read(m_fd, buffer, maxBytes);
#endif
    }

    size_t SerialPort::platformBytesAvailable() {
#ifdef _WIN32
        COMSTAT comStat;
        DWORD errors;
        if (ClearCommError(m_handle.load(std::memory_order_acquire), &errors, &comStat)) {
            return comStat.cbInQue;
        }
        return 0;
#else
        int bytesAvailable = 0;
        if (ioctl(m_fd, FIONREAD, &bytesAvailable) >= 0) {
            return static_cast<size_t>(bytesAvailable);
        }
        return 0;
#endif
    }

    bool SerialPort::platformFlush() {
#ifdef _WIN32
        return FlushFileBuffers(m_handle.load(std::memory_order_acquire)) != 0;
#else
        return tcdrain(m_fd) == 0;
#endif
    }

    std::string SerialPort::getLastPlatformError() {
#ifdef _WIN32
        DWORD error = GetLastError();
        return "Windows error: " + std::to_string(error);
#else
        return "errno: " + std::to_string(errno);
#endif
    }

} // namespace makcu