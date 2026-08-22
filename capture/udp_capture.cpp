#include "udp_capture.h"

#include <chrono>
#include <iostream>

UDPCapture::UDPCapture(int width, int height, const std::string& ip, int port)
    : width_(width)
    , height_(height)
    , ip_(ip)
    , port_(port)
    , socket_(INVALID_SOCKET)
    , is_connected_(false)
    , initialized_(false)
    , should_stop_(false)
    , received_frames_(0)
    , dropped_frames_(0)
{
    SetSourceDimensions(width_, height_);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "[UDP捕获] WSAStartup失败" << std::endl;
        return;
    }
    wsa_initialized_ = true;

    initialized_ = Initialize();
}

UDPCapture::~UDPCapture()
{
    Cleanup();
    // 仅在本对象成功初始化过 WinSock 时才释放，避免误减其它模块的进程级引用计数。
    if (wsa_initialized_)
    {
        WSACleanup();
        wsa_initialized_ = false;
    }
}

bool UDPCapture::Initialize()
{
    initialized_ = false;
    if (socket_ != INVALID_SOCKET)
        closesocket(socket_);

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET)
    {
        std::cerr << "[UDP捕获] 创建套接字失败: " << WSAGetLastError() << std::endl;
        return false;
    }

    int buffer_size = MAX_FRAME_SIZE;
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (char*)&buffer_size, sizeof(buffer_size)) == SOCKET_ERROR)
    {
        std::cerr << "[UDP捕获] 设置接收缓冲区大小失败: " << WSAGetLastError() << std::endl;
    }

    u_long mode = 1;
    if (ioctlsocket(socket_, FIONBIO, &mode) == SOCKET_ERROR)
    {
        std::cerr << "[UDP捕获] 设置非阻塞模式失败: " << WSAGetLastError() << std::endl;
    }

    memset(&server_addr_, 0, sizeof(server_addr_));
    server_addr_.sin_family = AF_INET;
    server_addr_.sin_port = htons(port_);
    const bool acceptAnySource = ip_.empty() || ip_ == "0.0.0.0";
    if (!acceptAnySource && inet_pton(AF_INET, ip_.c_str(), &server_addr_.sin_addr) <= 0)
    {
        std::cerr << "[UDP捕获] 无效的IP地址: " << ip_ << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }

    sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(port_);
    if (bind(socket_, (sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR)
    {
        std::cerr << "[UDP捕获] 绑定套接字失败: " << WSAGetLastError() << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }

    should_stop_ = false;
    is_connected_ = true;
    received_frames_ = 0;
    dropped_frames_ = 0;

    receive_thread_ = std::thread(&UDPCapture::ReceiveThread, this);

    std::cout << "[UDP捕获] 正在监听UDP " << ip_ << ":" << port_ << std::endl;
    initialized_ = true;
    return true;
}

void UDPCapture::Cleanup()
{
    should_stop_ = true;
    is_connected_ = false;
    initialized_ = false;

    if (receive_thread_.joinable())
    {
        receive_thread_.join();
    }

    if (socket_ != INVALID_SOCKET)
    {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

void UDPCapture::SetUDPParams(const std::string& ip, int port)
{
    if (ip_ != ip || port_ != port)
    {
        ip_ = ip;
        port_ = port;

        if (is_connected_)
        {
            Cleanup();
            initialized_ = Initialize();
        }
    }
}

cv::Mat UDPCapture::GetNextFrameCpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame_queue_.empty())
        return cv::Mat();

    cv::Mat frame = frame_queue_.front();
    frame_queue_.pop();
    return frame;
}

void UDPCapture::ReceiveThread()
{
    try
    {
        std::vector<uint8_t> buffer(MAX_FRAME_SIZE);
        std::vector<uint8_t> frame_data;
        // 增量扫描进度：frame_data 在"消费一整帧"之前只追加不删除，下标稳定，
        // 因此可跨数据报续扫，避免每报文重扫整个缓冲区。
        MjpegScanState scan_state;

        while (!should_stop_)
        {
            sockaddr_in from_addr;
            int from_len = sizeof(from_addr);

            int bytes_received = recvfrom(
                socket_,
                (char*)buffer.data(),
                (int)buffer.size(),
                0,
                (sockaddr*)&from_addr,
                &from_len
            );

            if (bytes_received == SOCKET_ERROR)
            {
                int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                std::cerr << "[UDP捕获] 接收错误: " << error << std::endl;
                break;
            }

            if (bytes_received <= 0)
                continue;

            if (server_addr_.sin_addr.s_addr != INADDR_ANY &&
                from_addr.sin_addr.s_addr != server_addr_.sin_addr.s_addr)
            {
                continue;
            }

            frame_data.insert(frame_data.end(), buffer.begin(), buffer.begin() + bytes_received);
            if (frame_data.size() > static_cast<size_t>(MAX_FRAME_SIZE) * 2)
            {
                frame_data.clear();
                scan_state.reset();
                continue;
            }

            cv::Mat frame;
            size_t consumed = 0;
            const bool decoded = ParseMJPEGFrame(frame_data, frame, &consumed, &scan_state);

            if (decoded && !frame.empty())
            {
                if (frame.cols != width_ || frame.rows != height_)
                    cv::resize(frame, frame, cv::Size(width_, height_));

                std::lock_guard<std::mutex> lock(frame_mutex_);
                while (frame_queue_.size() >= MAX_QUEUE_SIZE)
                {
                    frame_queue_.pop();
                    dropped_frames_++;
                }

                // frame 由 imdecode 独占创建，此处无其它引用，直接移动即可，省去一次全帧深拷贝。
                frame_queue_.push(std::move(frame));
                received_frames_++;
            }

            // 只要定位到了完整的 SOI..EOI 段就推进缓冲（无论解码成功与否）：
            // 1) 解码失败时若不推进，损坏段会永久滞留，导致后续所有帧都无法解析；
            // 2) 仅丢弃已消费字节，EOI 之后的残留属于下一帧起始数据，必须保留。
            if (consumed > 0)
            {
                if (consumed >= frame_data.size())
                    frame_data.clear();
                else
                    frame_data.erase(frame_data.begin(), frame_data.begin() + static_cast<std::ptrdiff_t>(consumed));

                // 缓冲区已前移，之前缓存的 SOI/EOI 下标全部失效，必须复位扫描进度。
                scan_state.reset();
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[UDP捕获] 接收线程崩溃: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[UDP捕获] 接收线程崩溃: 未知异常。" << std::endl;
    }
}

bool UDPCapture::ParseMJPEGFrame(const std::vector<uint8_t>& data, cv::Mat& frame,
                                 size_t* consumed, MjpegScanState* state)
{
    if (consumed)
        *consumed = 0;

    if (data.size() < 4)
        return false;

    // 未传入外部状态时使用局部状态，等价于旧的一次性全量扫描。
    MjpegScanState fallback;
    MjpegScanState& scan = state ? *state : fallback;

    // 扫描一对相邻字节时，末尾 1 字节可能与下一个数据报的首字节组成标记，
    // 因此续扫起点必须回退 1 字节，不能直接从 data.size() 开始。
    const auto resumePoint = [&data](size_t lowerBound) -> size_t
    {
        const size_t tail = data.empty() ? 0 : data.size() - 1;
        return tail > lowerBound ? tail : lowerBound;
    };

    // 1) 增量定位 SOI（0xFFD8）。一旦找到即缓存下标，后续数据报不再重复搜索。
    if (scan.soi_pos == MjpegScanState::kNoPos)
    {
        for (size_t i = scan.soi_scan_from; i + 1 < data.size(); ++i)
        {
            if (data[i] == 0xFF && data[i + 1] == 0xD8)
            {
                scan.soi_pos = i;
                break;
            }
        }

        if (scan.soi_pos == MjpegScanState::kNoPos)
        {
            scan.soi_scan_from = resumePoint(scan.soi_scan_from);
            return false;
        }

        scan.eoi_scan_from = scan.soi_pos + 2;
    }

    const size_t start_pos = scan.soi_pos;

    // 2) 增量定位 EOI（0xFFD9）。JPEG 熵编码段内的 0xFF 会被字节填充为 0xFF00，
    //    因此 0xFFD9 不会在压缩数据中误匹配。
    size_t end_pos = MjpegScanState::kNoPos;
    const size_t eoi_begin = scan.eoi_scan_from > start_pos + 2 ? scan.eoi_scan_from : start_pos + 2;
    for (size_t i = eoi_begin; i + 1 < data.size(); ++i)
    {
        if (data[i] == 0xFF && data[i + 1] == 0xD9)
        {
            end_pos = i + 2;
            break;
        }
    }

    if (end_pos == MjpegScanState::kNoPos)
    {
        scan.eoi_scan_from = resumePoint(eoi_begin);
        return false;
    }

    std::vector<uint8_t> jpeg_data(
        data.begin() + static_cast<std::ptrdiff_t>(start_pos),
        data.begin() + static_cast<std::ptrdiff_t>(end_pos));
    try
    {
        frame = cv::imdecode(jpeg_data, cv::IMREAD_COLOR);
        // 无论解码是否成功，SOI..EOI 这段数据都已处理完毕，必须报告为已消费，
        // 否则损坏的 JPEG 段会永久滞留在缓冲区，使后续所有帧都无法解析。
        if (consumed)
            *consumed = end_pos;
        return !frame.empty();
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[UDP捕获] JPEG解码错误: " << e.what() << std::endl;
        if (consumed)
            *consumed = end_pos;
        return false;
    }
}
