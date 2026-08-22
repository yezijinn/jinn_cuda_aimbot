#pragma once

// Export macros for shared library support across platforms
#ifdef _WIN32
    #ifdef MAKCU_EXPORTS
        #define MAKCU_API __declspec(dllexport)
    #elif defined(MAKCU_SHARED)
        #define MAKCU_API __declspec(dllimport)
    #else
        #define MAKCU_API
    #endif
#else
    #ifdef __GNUC__
        #define MAKCU_API __attribute__((visibility("default")))
    #else
        #define MAKCU_API
    #endif
#endif

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <future>
#include <unordered_map>
#include <thread>
#include <queue>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
// Define ssize_t for Windows compatibility
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fstream>
#include <regex>
#endif

namespace makcu {

    struct PendingCommand {
        int command_id;
        std::string command;
        std::promise<std::string> promise;
        std::chrono::steady_clock::time_point timestamp;
        bool expect_response;
        std::chrono::milliseconds timeout;

        PendingCommand(int id, const std::string& cmd, bool expect_resp, std::chrono::milliseconds to)
            : command_id(id), command(cmd), expect_response(expect_resp), timeout(to) {
            timestamp = std::chrono::steady_clock::now();
        }
    };

    class MAKCU_API SerialPort {
    public:
        SerialPort();
        ~SerialPort();

        bool open(const std::string& port, uint32_t baudRate);
        void close();
        bool isOpen() const;
        bool isActuallyConnected() const;

        bool setBaudRate(uint32_t baudRate);
        uint32_t getBaudRate() const;
        std::string getPortName() const;

        // High-performance command execution with tracking
        std::future<std::string> sendTrackedCommand(const std::string& command,
            bool expectResponse = false,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

        // Fast fire-and-forget commands
        bool sendCommand(const std::string& command);

        // Legacy methods for compatibility
        bool write(const std::vector<uint8_t>& data);
        bool write(const std::string& data);
        std::vector<uint8_t> read(size_t maxBytes = 1024);
        std::string readString(size_t maxBytes = 1024);

        size_t available() const;
        bool flush();

        // Optimized timeout control
        void setTimeout(uint32_t timeoutMs);
        uint32_t getTimeout() const;

        // Port enumeration
        static std::vector<std::string> getAvailablePorts();
        static std::vector<std::string> findMakcuPorts();

        // Button callback support
        using ButtonCallback = std::function<void(uint8_t, bool)>;
        void setButtonCallback(ButtonCallback callback);

    private:
        // close() 的实现体，要求调用方已持有 m_mutex。
        // open() 需要在持锁状态下关闭旧连接，而 std::mutex 不可重入，
        // 直接调用 public 的 close() 会自死锁，故拆出该内部版本。
        void closeLocked();

        std::string m_portName;
        uint32_t m_baudRate;
        uint32_t m_timeout;
        std::atomic<bool> m_isOpen;
        mutable std::mutex m_mutex;

#ifdef _WIN32
        // 原子句柄：close() 与监听线程的 ReadFile / 鼠标线程的 WriteFile 分属不同线程，
        // 原先的裸 HANDLE 既非原子也不在 m_mutex 保护下，构成数据竞争。更危险的是句柄复用：
        // CloseHandle 后内核可立即把同一数值分配给别的对象（另一个文件、套接字），
        // 此时仍在飞行中的 WriteFile 就会把鼠标指令写进无关对象里。
        // 改为 atomic<HANDLE> 后：读取点统一 load，关闭点用 exchange 原子取出并置为
        // INVALID_HANDLE_VALUE，保证 CloseHandle 只可能执行一次，且其他线程后续读到的
        // 必定是 INVALID_HANDLE_VALUE 而不是一个已失效的旧值。
        // HANDLE 即 void*，atomic<void*> 在 Win64 上是 lock-free 的，无额外开销。
        std::atomic<HANDLE> m_handle;
        DCB m_dcb;
        COMMTIMEOUTS m_timeouts;
#else
        int m_fd;
        struct termios m_oldTermios;
        struct termios m_newTermios;
#endif

        // Command tracking system
        std::atomic<int> m_commandCounter{ 0 };
        std::unordered_map<int, std::unique_ptr<PendingCommand>> m_pendingCommands;
        std::mutex m_commandMutex;

        // High-performance listener thread
        std::thread m_listenerThread;
        std::atomic<bool> m_stopListener{ false };

        // Button data processing
        ButtonCallback m_buttonCallback;
        std::atomic<uint8_t> m_lastButtonMask{ 0 };

        // Optimized parsing buffers
        static constexpr size_t BUFFER_SIZE = 4096;
        static constexpr size_t LINE_BUFFER_SIZE = 256;

        bool configurePort() { return platformConfigurePort(); }
        void updateTimeouts() { platformUpdateTimeouts(); }
        void listenerLoop();
        void processIncomingData();
        void handleButtonData(uint8_t data);
        void processResponse(const std::string& response);
        void cleanupTimedOutCommands();
        int generateCommandId();

        // Platform abstraction helpers for unified logic
        bool platformOpen(const std::string& devicePath);
        void platformClose();
        bool platformConfigurePort();
        void platformUpdateTimeouts();
        ssize_t platformWrite(const void* data, size_t length);
        ssize_t platformRead(void* buffer, size_t maxBytes);
        size_t platformBytesAvailable();
        bool platformFlush();
        std::string getLastPlatformError();

        // Disable copy
        SerialPort(const SerialPort&) = delete;
        SerialPort& operator=(const SerialPort&) = delete;
    };

} // namespace makcu