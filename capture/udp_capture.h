#ifndef UDP_CAPTURE_H
#define UDP_CAPTURE_H

#include "capture.h"

#include <opencv2/opencv.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

class UDPCapture : public IScreenCapture
{
public:
    UDPCapture(int width, int height, const std::string& ip = "0.0.0.0", int port = 1234);
    ~UDPCapture();

    cv::Mat GetNextFrameCpu() override;

    bool Initialize();
    void Cleanup();
    bool isInitialized() const { return initialized_.load(); }

    void SetUDPParams(const std::string& ip, int port);
    bool IsConnected() const { return is_connected_.load(); }
    int GetReceivedFrames() const { return received_frames_.load(); }
    int GetDroppedFrames() const { return dropped_frames_.load(); }

private:
    // MJPEG 增量扫描状态。
    // 一帧 JPEG 通常被拆成多个 UDP 数据报（单报文最大 65507 字节），原实现每收到
    // 一个数据报就从缓冲区头部重新全量搜索 SOI/EOI，一帧被拆成 K 段时总扫描量为
    // O(K·N)。由于 frame_data 在"消费一整帧"之前只追加不删除，已扫描过的下标是
    // 稳定的，因此可以把搜索进度记下来增量续扫，总量降到 O(N)。
    struct MjpegScanState
    {
        static constexpr size_t kNoPos = static_cast<size_t>(-1);

        size_t soi_pos{ kNoPos };   // 已定位到的 SOI(0xFFD8) 下标
        size_t soi_scan_from{ 0 };  // 下一次搜索 SOI 的起始下标
        size_t eoi_scan_from{ 0 };  // 下一次搜索 EOI(0xFFD9) 的起始下标

        void reset() { *this = MjpegScanState{}; }
    };

    void ReceiveThread();
    // consumed: 解析成功时输出本次已消费的字节数（JPEG 结束标记之后的位置），
    // 供调用方仅丢弃已消费部分、保留属于下一帧的残留数据。
    // state:    增量扫描状态；调用方在丢弃/清空 frame_data 后必须调用 state.reset()。
    //           传 nullptr 时退化为一次性全量扫描（行为与旧版一致）。
    bool ParseMJPEGFrame(const std::vector<uint8_t>& data, cv::Mat& frame,
                         size_t* consumed = nullptr, MjpegScanState* state = nullptr);

    int width_;
    int height_;
    std::string ip_;
    int port_;

    SOCKET socket_;
    sockaddr_in server_addr_;

    // 记录本对象是否成功执行过 WSAStartup。
    // WinSock 采用进程级引用计数，kmbox_net 等模块同样会调用 WSAStartup/WSACleanup；
    // 若此处在 WSAStartup 失败后仍调用 WSACleanup，会错误递减其它模块持有的引用计数，
    // 导致其它模块的套接字被提前失效，因此必须严格配对。
    bool wsa_initialized_ = false;

    std::atomic<bool> is_connected_;
    std::atomic<bool> initialized_;
    std::atomic<bool> should_stop_;
    std::atomic<int> received_frames_;
    std::atomic<int> dropped_frames_;

    std::thread receive_thread_;
    std::mutex frame_mutex_;
    std::queue<cv::Mat> frame_queue_;

    static const int MAX_FRAME_SIZE = 1024 * 1024;
    static const int MAX_QUEUE_SIZE = 5;
};

#endif // UDP_CAPTURE_H
