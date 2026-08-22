#include <time.h>

#include <atomic>

#include "kmbox_net/kmboxNet.h"
#include "kmbox_net/HidTable.h"

#define monitor_ok    2
#define monitor_exit  0
SOCKET sockClientfd = 0;              // Mouse and keyboard network communication handle
SOCKET sockMonitorfd = 0;             // Monitor network communication handle
client_tx tx;                         // Data to send
client_tx rx;                         // Data to receive
SOCKADDR_IN addrSrv;
soft_mouse_t    softmouse;            // Software mouse data
soft_keyboard_t softkeyboard;         // Software keyboard data
// monitor_run 由监听线程写、鼠标线程读；非原子量允许编译器把读取提升到循环外，
// 使监控状态停留在旧值，故改为 std::atomic<int>（赋值/比较写法保持不变）。
static std::atomic<int> monitor_run{ 0 }; // Whether physical mouse/keyboard monitoring is running
static int mask_keyboard_mouse_flag = 0; // Mouse/keyboard block status
// 端口合法范围 1024~49151；有符号 short 在 32768~49151 段会变成负数，
// 既让 htons 绑定到错误端口，也会在 (port | 0xaa55 << 16) 里符号扩展污染高 16 位魔数。
static unsigned short monitor_port = 0;
// 监听线程句柄与退出请求。本工程中 kmNet_monitor 会被调用两次
// （KmboxNetConnection 构造函数一次、monitorThread 一次）；若不先回收旧线程
// 就重建 socket，旧线程退出时会再次 CloseSocket 同一个全局句柄——而此时该句柄
// 可能已被新线程复用，导致监控静默失效，且 monitor_run 会被旧线程清零。
static HANDLE monitorThreadHandle = nullptr;
static std::atomic<bool> monitorStopRequest{ false };
// 本模块对进程级 WSA 引用计数的持有标志。kmNet_init 成功 WSAStartup 后置 true，
// CleanupClientSocket 配对 WSACleanup 并复位；保证多次 init/close（如 reboot→close）
// 不会超减引用计数，避免击穿同进程内 udp_capture 等其它模块的 socket。
static bool g_wsaOwned = false;

namespace
{
constexpr DWORD kInitialReceiveTimeoutMs = 1000;
constexpr DWORD kCommandReceiveTimeoutMs = 300;
constexpr DWORD kMonitorReceiveTimeoutMs = 100;

bool IsSocketValid(SOCKET socket)
{
	return socket != 0 && socket != INVALID_SOCKET;
}

void CloseSocket(SOCKET& socket)
{
	if (IsSocketValid(socket))
		closesocket(socket);
	socket = 0;
}

bool SetReceiveTimeout(SOCKET socket, DWORD timeoutMs)
{
	return setsockopt(
		socket,
		SOL_SOCKET,
		SO_RCVTIMEO,
		reinterpret_cast<const char*>(&timeoutMs),
		sizeof(timeoutMs)) != SOCKET_ERROR;
}

bool SetSendTimeout(SOCKET socket, DWORD timeoutMs)
{
	return setsockopt(
		socket,
		SOL_SOCKET,
		SO_SNDTIMEO,
		reinterpret_cast<const char*>(&timeoutMs),
		sizeof(timeoutMs)) != SOCKET_ERROR;
}

bool SetNonBlocking(SOCKET socket)
{
	u_long nonBlocking = 1;
	return ioctlsocket(socket, FIONBIO, &nonBlocking) != SOCKET_ERROR;
}

int RecvFromWithTimeout(
	SOCKET socket,
	char* buffer,
	int length,
	int flags,
	sockaddr* from,
	int* fromLen,
	DWORD timeoutMs = kCommandReceiveTimeoutMs)
{
	if (!IsSocketValid(socket))
	{
		WSASetLastError(WSAENOTSOCK);
		return SOCKET_ERROR;
	}

	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(socket, &readSet);

	timeval timeout{};
	timeout.tv_sec = static_cast<long>(timeoutMs / 1000);
	timeout.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);

	const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
	if (ready <= 0)
	{
		if (ready == 0)
			WSASetLastError(WSAETIMEDOUT);
		return SOCKET_ERROR;
	}

	return ::recvfrom(socket, buffer, length, flags, from, fromLen);
}

void CleanupClientSocket()
{
	CloseSocket(sockClientfd);
	if (g_wsaOwned)
	{
		WSACleanup();
		g_wsaOwned = false;
	}
}

// 请求监听线程退出并回收其句柄。
// 监听线程每轮 select 的等待上限为 kMonitorReceiveTimeoutMs(100ms)，因此退出时间有界；
// 由线程自己关闭 sockMonitorfd，避免"调用线程关句柄、监听线程仍在 select"的复用竞态，
// 也避免旧线程在退出路径上二次关闭已被新线程占用的句柄。
// 返回 false 表示线程未在时限内退出；此时保留句柄并返回错误，调用方不得复用监听资源。
bool StopMonitorThread()
{
	if (monitorThreadHandle == nullptr)
	{
		// 没有在跑的监听线程；兜底清理可能残留的 socket（例如线程创建失败的路径）。
		if (IsSocketValid(sockMonitorfd))
			CloseSocket(sockMonitorfd);
		return true;
	}

	monitorStopRequest.store(true, std::memory_order_release);
	// 2000ms 远大于 100ms 的单轮超时上限，正常情况下会立即返回 WAIT_OBJECT_0。
	if (WaitForSingleObject(monitorThreadHandle, 2000) != WAIT_OBJECT_0)
	{
		// 线程异常滞留：不再等待，但也不强杀（TerminateThread 会泄漏其栈与 Winsock 引用计数）。
		// 保留句柄，保留 monitorStopRequest=true；下次 StopMonitorThread 会继续等待回收，
		// 且 kmNet_monitor 不会在这种状态下创建新监听线程，避免旧线程与新线程交叉关闭 socket。
		if (IsSocketValid(sockMonitorfd))
			CloseSocket(sockMonitorfd);
		return false;
	}
	CloseHandle(monitorThreadHandle);
	monitorThreadHandle = nullptr;
	monitorStopRequest.store(false, std::memory_order_release);
	return true;
}
}

#pragma pack(1)
typedef struct {
	unsigned char report_id;
	unsigned char buttons;		// 8 buttons available
	short x;					// -32767 to 32767
	short y;					// -32767 to 32767
	short wheel;				// -32767 to 32767
}standard_mouse_report_t;

typedef struct {
	unsigned char report_id;
	unsigned char buttons;      // 8 buttons control keys
	unsigned char data[10];     // Regular keys
} standard_keyboard_report_t;
#pragma pack()

// 监听线程私有的暂存缓冲：仅 ThreadListenProcess 写入，保持原有报文布局与偏移解析不变。
// 经全仓 grep 确认这两个符号无任何外部引用，故收敛为 static（原为外链接全局）。
static standard_mouse_report_t     hw_mouse;     // Hardware mouse message
static standard_keyboard_report_t  hw_keyboard;  // Hardware keyboard message

// ---------------------------------------------------------------------------
// 物理按键状态的跨线程发布镜像。
//
// 原实现由 ThreadListenProcess（监听线程）直接 memcpy 写 hw_mouse / hw_keyboard，
// 而 kmNet_monitor_mouse_*() / kmNet_monitor_keyboard() 由鼠标线程与键盘线程无同步读取：
//   1) 这是标准的数据竞争（UB）。实测最典型的后果是编译器把 `hw_mouse.buttons` 的读取
//      提升（hoist）到调用方的轮询循环之外——因为在编译器看来这块内存在循环内不可能被
//      改变——表现为「按住侧键不放却一直读到松开」或「松开后仍持续开火」的按键卡死。
//   2) 键盘 10 键数组在 memcpy 执行到一半时被读取，会得到新旧混合的按键集合。
//
// 修复方式：监听线程仍先写入上面的私有暂存（解析逻辑一字不改），再一次性发布到下列
// 原子镜像；所有查询函数只读镜像。鼠标按键 / 键盘修饰键各 1 字节，10 个常规键压进
// 一个 u64（低 8 键）+ 一个 u16（高 2 键），每个原子量的单次 load 即可保证其内部一致性。
// 全程无锁，开销与原实现同量级（一次 relaxed store / acquire load）。
static std::atomic<unsigned char>      g_monMouseButtons{ 0 };  // hw_mouse.buttons
static std::atomic<unsigned char>      g_monKbdModifiers{ 0 };  // hw_keyboard.buttons
static std::atomic<unsigned long long> g_monKbdKeysLo{ 0 };     // hw_keyboard.data[0..7]
static std::atomic<unsigned short>     g_monKbdKeysHi{ 0 };     // hw_keyboard.data[8..9]

// 把监听线程暂存区的内容发布到原子镜像。仅由监听线程调用。
static void PublishMonitorState()
{
	unsigned long long lo = 0;
	for (int i = 0; i < 8; ++i)
	{
		lo |= static_cast<unsigned long long>(
			static_cast<unsigned char>(hw_keyboard.data[i])) << (i * 8);
	}
	const unsigned short hi = static_cast<unsigned short>(
		static_cast<unsigned char>(hw_keyboard.data[8]) |
		(static_cast<unsigned int>(static_cast<unsigned char>(hw_keyboard.data[9])) << 8));

	g_monMouseButtons.store(hw_mouse.buttons, std::memory_order_relaxed);
	g_monKbdModifiers.store(hw_keyboard.buttons, std::memory_order_relaxed);
	g_monKbdKeysLo.store(lo, std::memory_order_relaxed);
	// 最后一次 store 用 release，与查询侧的 acquire 配对，保证前三个 store 对读者可见。
	g_monKbdKeysHi.store(hi, std::memory_order_release);
}

// 清空暂存与镜像。监听线程启动时调用：避免上一轮监听会话遗留的按键状态在
// monitor_run 置位后、首个回传帧到达前被读到，造成「刚开启监控就误判有键按下」。
static void ResetMonitorState()
{
	memset(&hw_mouse, 0, sizeof(hw_mouse));
	memset(&hw_keyboard, 0, sizeof(hw_keyboard));
	PublishMonitorState();
}

// Generate a random number between A and B
int myrand(int a, int b)
{
	int min = a < b ? a : b;
	int max = a > b ? a : b;
	// 官方实现含 a==b 保护（返回 a）；项目版此前遗漏，a==b 时 max-min==0，
	// rand() % 0 是未定义行为（除零）。补回与官方一致的保护。
	if (a == b) return a;
	return ((rand() % (max - min)) + min);
}

unsigned int StrToHex(char* pbSrc, int nLen)
{
	char h1, h2;
	unsigned char s1, s2;
	int i;
	// 越界读保护：MAC 必须是 nLen*2 个十六进制字符；不足则越界读（UB，且得到垃圾值），
	// 以及空指针，统一拒绝返回 0（合法 8 字符输入行为不变）。
	if (pbSrc == nullptr || nLen <= 0)
		return 0;
	for (int k = 0; k < nLen * 2; ++k) {
		if (pbSrc[k] == '\0')
			return 0;
	}
	unsigned int pbDest[16] = { 0 };
	for (i = 0; i < nLen; i++) {
		h1 = pbSrc[2 * i];
		h2 = pbSrc[2 * i + 1];
		s1 = (unsigned char)toupper((unsigned char)h1) - 0x30;
		if (s1 > 9)
			s1 -= 7;
		s2 = (unsigned char)toupper((unsigned char)h2) - 0x30;
		if (s2 > 9)
			s2 -= 7;
		pbDest[i] = s1 * 16 + s2;
	}
	return pbDest[0] << 24 | pbDest[1] << 16 | pbDest[2] << 8 | pbDest[3];
}

int NetRxReturnHandle(client_tx* rx, client_tx* tx)      // Received content
{
	if (rx->head.cmd != tx->head.cmd)
		return  err_net_cmd;    // Command code error
	if (rx->head.indexpts != tx->head.indexpts)
		return  err_net_pts;    // Timestamp error
	return 0;                   // No error, return 0
	//return  rx->head.rand;    // Actual return value
}

// ---------------------------------------------------------------------------
// 统一的「发送命令 → 等待并校验应答」路径。
//
// 原实现在 24 个命令函数里逐份复制了同一段收发代码，并共同带有三个缺陷：
//
//  1) sendto 的返回值被全部丢弃。发送失败（网线拔出 / 目标不可达 / 发送缓冲满 /
//     句柄失效）时代码仍然继续进入接收，白白阻塞一个完整的 300ms 超时窗口。
//     kmNet_mouse_move() 位于鼠标线程的每帧热路径，一旦盒子掉线就变成每帧 300ms，
//     整条瞄准链路事实上停摆，且用户侧只看到"卡死"而无任何错误提示。
//     现在发送失败立即返回 err_net_tx，耗时从 300ms 降到微秒级。
//
//  2) 应答长度未校验。rx 是**全局对象**，一个短于 sizeof(cmd_head_t) 的报文并不会
//     覆盖 head 区，NetRxReturnHandle() 读到的是**上一次应答残留的 cmd/indexpts**。
//     由于同类命令的 cmd 恒定、indexpts 又是单调递增，存在把"根本没有应答"误判成
//     success 的窗口，上层因此认为设备在线并继续下发指令。现在按最小帧长过滤。
//
//  3) 应答源地址未校验。命令 socket 未 connect，局域网内任意主机都能向其回包伪造
//     ACK。第 24 轮已为监听线程补了源地址校验，命令路径此前一直是缺口。
//
// 对合法应答（来自盒子本机、长度 >= sizeof(cmd_head_t)、cmd/indexpts 匹配）的行为
// 与原实现逐位一致；被丢弃的报文不会吃掉本次等待预算，循环在剩余时间内继续接收。
static int SendCommandAndAwaitAck(int length, DWORD timeoutMs = kCommandReceiveTimeoutMs)
{
	if (!IsSocketValid(sockClientfd))
		return err_creat_socket;

	const int sent = sendto(
		sockClientfd,
		(const char*)&tx,
		length,
		0,
		(struct sockaddr*)&addrSrv,
		sizeof(addrSrv));
	if (sent == SOCKET_ERROR || sent != length)
		return err_net_tx;

	// GetTickCount 每 49.7 天回绕一次；用无符号差值再转有符号求剩余量，回绕点附近同样成立。
	const DWORD deadlineTick = GetTickCount() + timeoutMs;
	for (;;)
	{
		const LONG remain = static_cast<LONG>(deadlineTick - GetTickCount());
		if (remain <= 0)
			return err_net_rx_timeout;

		SOCKADDR_IN from;
		memset(&from, 0, sizeof(from));
		int fromLen = static_cast<int>(sizeof(from));
		// 接收缓冲按 rx 的完整大小给出：Windows 的 recvfrom 在数据报长于缓冲时会直接
		// 返回 WSAEMSGSIZE 而非截断，原先固定写 1024 会让 1025~1040 字节的应答整帧失败。
		const int received = RecvFromWithTimeout(
			sockClientfd,
			(char*)&rx,
			static_cast<int>(sizeof(rx)),
			0,
			(struct sockaddr*)&from,
			&fromLen,
			static_cast<DWORD>(remain));
		if (received < 0)
			return err_net_rx_timeout;
		if (received < static_cast<int>(sizeof(cmd_head_t)))
			continue;   // 短包：head 区未被完整覆盖，其中的 cmd/indexpts 是上一帧残留值
		if (from.sin_addr.S_un.S_addr != addrSrv.sin_addr.S_un.S_addr)
			continue;   // 非盒子来源：拒绝伪造 ACK

		return NetRxReturnHandle(&rx, &tx);
	}
}


/*
Connect to kmboxNet box. The input parameters are:
ip   : The IP address of the box (displayed on the screen, e.g., 192.168.2.88)
port : Communication port number (displayed on the screen, e.g., 6234)
mac  : The MAC address of the box (displayed on the screen, e.g., 12345)
Return value: 0 means success, non-zero values refer to error codes
*/
int kmNet_init(char* ip, char* port, char* mac)
{
    // 参数校验：空指针、无效 IP、无效端口、无效 MAC 均在创建 socket 前拒绝，
    // 避免无效连接卡住上层状态（press/release 等），也不会浪费 WSAStartup 调用。
    if (ip == nullptr || port == nullptr || mac == nullptr)
        return err_creat_socket;
    if (inet_addr(ip) == INADDR_NONE)
        return err_creat_socket;
    {
        const int portVal = atoi(port);
        if (portVal <= 0 || portVal > 65535)
            return err_creat_socket;
    }
    if (mac[0] == '\0')
        return err_creat_socket;

	WORD wVersionRequested; WSADATA wsaData; int err;
	wVersionRequested = MAKEWORD(1, 1);
	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0)        return err_creat_socket;
	if (LOBYTE(wsaData.wVersion) != 1 || HIBYTE(wsaData.wVersion) != 1) {
		WSACleanup();
		sockClientfd = 0;
		return err_net_version;
	}
	g_wsaOwned = true;   // 声明本模块持有一次 WSA 引用，由 CleanupClientSocket 配对清理
	srand((unsigned)time(NULL));
	sockClientfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (!IsSocketValid(sockClientfd))
	{
		// 必须经 CleanupClientSocket 配对 WSAStartup 的引用计数；原实现直接 WSACleanup
		// 但 g_wsaOwned 仍为 true，会让后续 kmNet_close/再次 init 执行多余的 WSACleanup。
		CleanupClientSocket();
		return err_creat_socket;
	}
	if (!SetNonBlocking(sockClientfd) ||
		!SetReceiveTimeout(sockClientfd, kInitialReceiveTimeoutMs) ||
		!SetSendTimeout(sockClientfd, kCommandReceiveTimeoutMs))
	{
		CleanupClientSocket();
		return err_creat_socket;
	}
	memset(&addrSrv, 0, sizeof(addrSrv));
	addrSrv.sin_addr.S_un.S_addr = inet_addr(ip);
	addrSrv.sin_family = AF_INET;
	addrSrv.sin_port = htons(atoi(port)); // Port UUID[1] >> 16 high 16 bits
	tx.head.mac = StrToHex(mac, 4);         // Box MAC, fixed UUID[1]
	tx.head.rand = rand();                  // Random value. Can be used later for packet encryption. Reserved for now.
	tx.head.indexpts = 0;                   // Command statistics value
	tx.head.cmd = cmd_connect;              // Command
	memset(&softmouse, 0, sizeof(softmouse));       // Clear software mouse data
	memset(&softkeyboard, 0, sizeof(softkeyboard)); // Clear software keyboard data
	mask_keyboard_mouse_flag = 0;                   // Clear software mask mirror
	Sleep(20); // The first connection may take longer
	// 走统一收发路径：内部会校验 sendto 结果、应答最小帧长，并把应答源地址限定为盒子本机
	// （recvfrom 的 from 是输出参数，原实现一度直接传全局 addrSrv，会被应答源地址覆盖，
	//  若局域网内其它主机抢先回包，之后所有 sendto 都会发往错误目标，设备整体失控）。
	err = SendCommandAndAwaitAck(static_cast<int>(sizeof(cmd_head_t)), kInitialReceiveTimeoutMs);
	if (err != success)
	{
		CleanupClientSocket();
		return err;
	}
	if (!SetReceiveTimeout(sockClientfd, kCommandReceiveTimeoutMs) ||
		!SetSendTimeout(sockClientfd, kCommandReceiveTimeoutMs))
	{
		CleanupClientSocket();
		return err_creat_socket;
	}
	return success;
}

void kmNet_close()
{
	CleanupClientSocket();
}

/*
Move the mouse by x, y units. One-time move, no trajectory simulation, fastest speed.
Use this function when implementing your own trajectory movement.
Return value: 0 if successful, nonzero means error.
*/
int kmNet_mouse_move(short x, short y)
{
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // Command statistics value
	tx.head.cmd = cmd_mouse_move;    // Command
	tx.head.rand = rand();           // Random obfuscation value
	// 清掉上一个命令残留的 wheel/Bezier point，避免 move 包带上无关滚动/曲线数据。
	// 不清理 softmouse.button：按钮掩码需要在 move/button 混合指令间保持，否则
	// 按键按住时移动会意外松开。
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	softmouse.x = x;
	softmouse.y = y;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	// 位移量已在上一行 memcpy 进 tx，此处清零 softmouse 不影响待发送字节，
	// 提前到发送之前只是为了复用统一收发路径（与 kmNet_mouse_all 等函数的既有顺序一致）。
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	return SendCommandAndAwaitAck(length);
}



/*
Mouse left button control
isdown : 0 = release, 1 = press
Return value: 0 if successful, nonzero means error.
*/
int kmNet_mouse_left(int isdown)
{
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // Command statistics value
	tx.head.cmd = cmd_mouse_left;    // Command
	tx.head.rand = rand();           // Random obfuscation value
	// 按键命令不允许带上上次 move/wheel 的 x/y/wheel/point，否则会变成“按键+漂移”
	// 或“按键+滚轮”的混合报文，真实用户操作会冲突/抖动。
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	softmouse.button = (isdown ? (softmouse.button | 0x01) : (softmouse.button & (~0x01)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	return SendCommandAndAwaitAck(length);
}

/*
Mouse middle button control
isdown : 0 = release, 1 = press
Return value: 0 if successful, nonzero means error.
*/
int kmNet_mouse_middle(int isdown)
{
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // Command statistics value
	tx.head.cmd = cmd_mouse_middle;  // Command
	tx.head.rand = rand();           // Random obfuscation value
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	softmouse.button = (isdown ? (softmouse.button | 0x04) : (softmouse.button & (~0x04)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	return SendCommandAndAwaitAck(length);
}

/*
Mouse right button control
isdown : 0 = release, 1 = press
Return value: 0 if successful, nonzero means error.
*/
int kmNet_mouse_right(int isdown)
{
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // Command statistics value
	tx.head.cmd = cmd_mouse_right;   // Command
	tx.head.rand = rand();           // Random obfuscation value
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	softmouse.button = (isdown ? (softmouse.button | 0x02) : (softmouse.button & (~0x02)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	return SendCommandAndAwaitAck(length);
}

/*
Mouse side button 1 control
isdown : 0 = release, 1 = press
Return value: 0 if successful, nonzero means error.
*/
int kmNet_mouse_side1(int isdown)
{
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // Command statistics value
	tx.head.cmd = cmd_mouse_right;   // 官方 SDK 中 side1 复用了 cmd_mouse_right 通道
	tx.head.rand = rand();           // Random obfuscation value
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	softmouse.button = (isdown ? (softmouse.button | 0x08) : (softmouse.button & (~0x08)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	return SendCommandAndAwaitAck(length);
}

/*
Mouse side button 2 control
isdown : 0 = release, 1 = press
Return value: 0 if successful, nonzero means error.
*/
int kmNet_mouse_side2(int isdown)
{
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // Command statistics value
	tx.head.cmd = cmd_mouse_right;   // 官方 SDK 中 side2 同样复走 cmd_mouse_right 通道
	tx.head.rand = rand();                    // Random obfuscation value
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	softmouse.button = (isdown ? (softmouse.button | 0x10) : (softmouse.button & (~0x10)));
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	return SendCommandAndAwaitAck(length);
}

// Mouse wheel control
int kmNet_mouse_wheel(int wheel)
{
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // Command statistics value
	tx.head.cmd = cmd_mouse_wheel;   // Command
	tx.head.rand = rand();           // Random obfuscation value
	softmouse.x = 0;
	softmouse.y = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	softmouse.wheel = wheel;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.wheel = 0;
	softmouse.x = 0;
	softmouse.y = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	return SendCommandAndAwaitAck(length);
}


/*
Mouse full report control function
*/
int kmNet_mouse_all(int button, int x, int y, int wheel)
{
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;              // Command statistics value
	tx.head.cmd = cmd_mouse_wheel;   // Command
	tx.head.rand = rand();           // Random obfuscation value
	memset(softmouse.point, 0, sizeof(softmouse.point));
	softmouse.button = button;
	softmouse.x = x;
	softmouse.y = y;
	softmouse.wheel = wheel;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	return SendCommandAndAwaitAck(length);
}

/*
Move the mouse by x, y units. Simulate human-like movement of x, y units.
This avoids detection of abnormal mouse and keyboard behavior.
If you do not implement a movement curve, it is recommended to use this function.
This function will not cause jumps; it approaches the target using minimal steps.
It takes more time than kmNet_mouse_move.
'ms' specifies how many milliseconds the movement should take.
Note: do not set 'ms' too low, otherwise abnormal data may still be detected.
Try to imitate human operation. Actual time may be less than 'ms'.
*/
int kmNet_mouse_move_auto(int x, int y, int ms)
{
	if (!IsSocketValid(sockClientfd))       return err_creat_socket;
	tx.head.indexpts++;                  // Command statistics value
	tx.head.cmd = cmd_mouse_automove;    // Command
	tx.head.rand = ms;                   // Random obfuscation value (here: movement time in ms)
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	softmouse.x = x;
	softmouse.y = y;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.x = 0;                     // Clear
	softmouse.y = 0;                     // Clear
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	return SendCommandAndAwaitAck(length);
}


/*
Second-order Bezier curve control
x, y   : Target point coordinates
ms     : Time to fit this process (in milliseconds)
x1, y1 : Control point p1 coordinates
x2, y2 : Control point p2 coordinates
*/
int kmNet_mouse_move_beizer(int x, int y, int ms, int x1, int y1, int x2, int y2)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;               // Command statistics value
	tx.head.cmd = cmd_bazerMove;      // Command
	tx.head.rand = ms;                // Random obfuscation value
	softmouse.wheel = 0;
	softmouse.x = x;
	softmouse.y = y;
	softmouse.point[0] = x1;
	softmouse.point[1] = y1;
	softmouse.point[2] = x2;
	softmouse.point[3] = y2;
	memcpy(&tx.cmd_mouse, &softmouse, sizeof(soft_mouse_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
	softmouse.x = 0;
	softmouse.y = 0;
	softmouse.wheel = 0;
	memset(softmouse.point, 0, sizeof(softmouse.point));
	return SendCommandAndAwaitAck(length);
}


/*
Key down event.
If vk_key is between KEY_LEFTCONTROL and KEY_RIGHT_GUI, it's a control key.
Otherwise, it's a regular key.
For regular keys, the function tries to add vk_key to the queue. If the queue is full, the oldest key is removed.
*/
int kmNet_keydown(int vk_key)
{
	int i;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI) // Control key
	{
		switch (vk_key)
		{
		case KEY_LEFTCONTROL: softkeyboard.ctrl |= BIT0; break;
		case KEY_LEFTSHIFT:   softkeyboard.ctrl |= BIT1; break;
		case KEY_LEFTALT:     softkeyboard.ctrl |= BIT2; break;
		case KEY_LEFT_GUI:    softkeyboard.ctrl |= BIT3; break;
		case KEY_RIGHTCONTROL:softkeyboard.ctrl |= BIT4; break;
		case KEY_RIGHTSHIFT:  softkeyboard.ctrl |= BIT5; break;
		case KEY_RIGHTALT:    softkeyboard.ctrl |= BIT6; break;
		case KEY_RIGHT_GUI:   softkeyboard.ctrl |= BIT7; break;
		}
	}
	else
	{   // Regular key
		for (i = 0; i < 10; i++) // First, check if vk_key already exists in the queue
		{
			if (softkeyboard.button[i] == vk_key)
				goto KM_down_send; // vk_key already in the queue, just send
		}
		// vk_key not in the queue
		for (i = 0; i < 10; i++) // Traverse all data, add vk_key to the queue
		{
			if (softkeyboard.button[i] == 0)
			{   // vk_key already in the queue, just send
				softkeyboard.button[i] = vk_key;
				goto KM_down_send;
			}
		}
		// Queue is full, remove the first one
		// button 为 char[10]：源区 &button[1] 复制 10 字节会读到 button[10]，
		// 即越过结构体末尾 1 字节；且源与目的重叠，memcpy 属未定义行为。
		// 正确长度为 9，并改用 memmove。
		memmove(&softkeyboard.button[0], &softkeyboard.button[1], 9);
		softkeyboard.button[9] = vk_key;
	}
KM_down_send:
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;               // Command statistics value
	tx.head.cmd = cmd_keyboard_all;   // Command
	tx.head.rand = rand();            // Random obfuscation value
	memcpy(&tx.cmd_keyboard, &softkeyboard, sizeof(soft_keyboard_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_keyboard_t);
	return SendCommandAndAwaitAck(length);
}


int kmNet_keyup(int vk_key)
{
	int i;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI) // Control key
	{
		switch (vk_key)
		{
		case KEY_LEFTCONTROL: softkeyboard.ctrl &= ~BIT0; break;
		case KEY_LEFTSHIFT:   softkeyboard.ctrl &= ~BIT1; break;
		case KEY_LEFTALT:     softkeyboard.ctrl &= ~BIT2; break;
		case KEY_LEFT_GUI:    softkeyboard.ctrl &= ~BIT3; break;
		case KEY_RIGHTCONTROL:softkeyboard.ctrl &= ~BIT4; break;
		case KEY_RIGHTSHIFT:  softkeyboard.ctrl &= ~BIT5; break;
		case KEY_RIGHTALT:    softkeyboard.ctrl &= ~BIT6; break;
		case KEY_RIGHT_GUI:   softkeyboard.ctrl &= ~BIT7; break;
		}
	}
	else
	{   // Regular key
		for (i = 0; i < 10; i++) // First, check if vk_key is in the queue
		{
			if (softkeyboard.button[i] == vk_key) // vk_key found in the queue
			{
				// 同上：源区末字节为 button[i + (10 - i)] = button[10]，越界 1 字节且区域重叠。
				// 剩余待前移元素个数为 9 - i，改用 memmove。
				memmove(&softkeyboard.button[i], &softkeyboard.button[i + 1], static_cast<size_t>(9 - i));
				softkeyboard.button[9] = 0;
				goto KM_up_send;
			}
		}
	}
KM_up_send:
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;               // Command statistics value
	tx.head.cmd = cmd_keyboard_all;   // Command
	tx.head.rand = rand();            // Random obfuscation value
	memcpy(&tx.cmd_keyboard, &softkeyboard, sizeof(soft_keyboard_t));
	int length = sizeof(cmd_head_t) + sizeof(soft_keyboard_t);
	return SendCommandAndAwaitAck(length);
}


// Reboot the box
int kmNet_reboot(void)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;               // Command statistics value
	tx.head.cmd = cmd_reboot;         // Command
	tx.head.rand = rand();            // Random obfuscation value
	int length = sizeof(cmd_head_t);
	// 无论应答成功与否都要关闭本地 socket（盒子随即重启，连接必然失效）。
	err = SendCommandAndAwaitAck(length);
	CleanupClientSocket();
	return err;
}


// Listen to physical mouse and keyboard
//static HANDLE handle_listen = NULL;
DWORD WINAPI ThreadListenProcess(LPVOID lpParameter)
{
	WSADATA wsaData; int ret;
	if (WSAStartup(MAKEWORD(1, 1), &wsaData) != 0)
		return 0;
	sockMonitorfd = socket(AF_INET, SOCK_DGRAM, 0);  // Bind socket
	if (!IsSocketValid(sockMonitorfd) ||
		!SetNonBlocking(sockMonitorfd) ||
		!SetReceiveTimeout(sockMonitorfd, kMonitorReceiveTimeoutMs))
	{
		CloseSocket(sockMonitorfd);
		WSACleanup();
		return 0;
	}
	sockaddr_in servAddr;
	memset(&servAddr, 0, sizeof(servAddr));          // Fill every byte with 0
	servAddr.sin_family = PF_INET;                   // Use IPv4 address
	servAddr.sin_addr.s_addr = INADDR_ANY;           // Automatically obtain IP address
	servAddr.sin_port = htons(monitor_port);         // Listening port
	ret = bind(sockMonitorfd, (SOCKADDR*)&servAddr, sizeof(SOCKADDR));
	if (ret == SOCKET_ERROR)
	{
		CloseSocket(sockMonitorfd);
		WSACleanup();
		return 0;
	}
	SOCKADDR cliAddr;  // Client address info
	int nSize = sizeof(SOCKADDR);
	char buff[1024] = { 0 };   // Buffer
	// 设备回传帧固定为「标准鼠标报文 + 标准键盘报文」，原实现只判 ret > 0 就整帧 memcpy，
	// 而该 socket 绑定在 INADDR_ANY 上：任意本机/局域网进程发一个短包，就会把
	// buff 里未初始化的栈内容当作按键状态复制进 hw_mouse/hw_keyboard，
	// 使 kmNet_monitor_mouse_left() 等返回幻按键并触发误开火。此处按最小帧长校验。
	constexpr int kMonitorFrameBytes =
		static_cast<int>(sizeof(hw_mouse) + sizeof(hw_keyboard));
	// 先清空上一轮监听会话遗留的按键状态，再对外宣告 monitor_ok，
	// 否则「关闭监控 → 重新开启」的窗口内会读到旧的按下状态并误触发。
	ResetMonitorState();
	monitor_run = monitor_ok;
	while (!monitorStopRequest.load(std::memory_order_acquire) && IsSocketValid(sockMonitorfd)) {
		nSize = sizeof(SOCKADDR);
		int ret = RecvFromWithTimeout(sockMonitorfd, buff, 1024, 0, &cliAddr, &nSize, kMonitorReceiveTimeoutMs); // Read with timeout
		if (ret >= kMonitorFrameBytes)
		{
			// 仅接受来自已配置盒子 IP 的回传帧：该监听 socket 绑定在 INADDR_ANY 上，
			// 局域网任意主机都能向此端口发伪造帧（见上方注释）。校验源地址后只采纳
			// 盒子本机回传帧，杜绝伪造按键 → 误开火（来源非盒子则保持上一次有效状态）。
			const auto* fromAddr = reinterpret_cast<const sockaddr_in*>(&cliAddr);
			if (fromAddr->sin_addr.S_un.S_addr == addrSrv.sin_addr.S_un.S_addr)
			{
				memcpy(&hw_mouse, buff, sizeof(hw_mouse));                          // Physical mouse state
				memcpy(&hw_keyboard, &buff[sizeof(hw_mouse)], sizeof(hw_keyboard)); // Physical keyboard state
				// 解析完成后一次性发布到原子镜像，供鼠标线程 / 键盘线程无锁读取。
				PublishMonitorState();
			}
		}
		else if (ret >= 0)
		{
			// 长度不足的报文一律丢弃：保持上一次的有效状态，不引入伪造按键。
			continue;
		}
		else
		{
			if (WSAGetLastError() == WSAETIMEDOUT)
				continue;
			break;
		}
	}
	monitor_run = 0;
	CloseSocket(sockMonitorfd);
	WSACleanup();
	return 0;
}

// Enable mouse and keyboard monitoring. Port number must be in range 1024–49151
int kmNet_monitor(short port)
{
	int err;
	if (!IsSocketValid(sockClientfd))
	{
		// 命令 socket 失效时，停止监听仍应尽可能回收本地监听线程；
		// 否则析构/重连路径会残留 ThreadListenProcess 和 sockMonitorfd。
		if (port == 0)
			(void)StopMonitorThread();
		return err_creat_socket;
	}
	tx.head.indexpts++;              // Command statistics value
	tx.head.cmd = cmd_monitor;       // Command
	if (port) {
		// 以无符号语义处理端口，避免 32768~49151 段被解释为负数；
		// 0xaa55u 保证 << 16 在无符号域内进行（原 0xaa55 << 16 是有符号整型溢出，属 UB）。
		const unsigned short uport = static_cast<unsigned short>(port);
		monitor_port = uport;                       // The port used to listen for physical mouse and keyboard data
		tx.head.rand = static_cast<unsigned int>(uport) | (0xaa55u << 16); // Random obfuscation value
	}
	else
		tx.head.rand = 0;    // Random obfuscation value
	int length = sizeof(cmd_head_t);
	err = SendCommandAndAwaitAck(length);
	// 先请求旧监听线程退出并回收，再决定是否新建。
	// 原实现直接 CloseSocket(sockMonitorfd)：旧线程此刻正阻塞在该句柄的 select 上，
	// 且退出路径会再次 CloseSocket 同一全局变量——若新线程已把新 socket 写入该变量，
	// 就会被旧线程关掉，同时 monitor_run 被旧线程置 0，监控静默失效。
	if (!StopMonitorThread())
		return err_creat_socket;
	if (err != success)
		return err;
	if (port)
	{
		// 保存句柄用于后续 WaitForSingleObject + CloseHandle，避免内核对象泄漏。
		monitorThreadHandle = CreateThread(NULL, 0, ThreadListenProcess, NULL, 0, NULL);
		if (monitorThreadHandle == nullptr)
			return err_creat_socket;
		// 有界等待监听线程就绪：ThreadListenProcess 只有在 socket 创建、bind 成功、
		// 清空历史按键状态之后才会把 monitor_run 置为 monitor_ok。原实现固定 Sleep(10)
		// 就返回，一旦 bind 失败（端口被占用等）或线程调度延迟，调用方拿到的是
		// "命令已发出" 的假成功，而 kmNet_monitor_mouse_*() 全部返回 -1，
		// 上层键盘判定链（keyboard_listener → keyPressed → monitorXxx）据此整体失效。
		// 轮询到就绪或超时（500ms）后返回，返回值才真正反映"监听可用"。
		constexpr int kMonitorReadyPollMaxMs = 500;
		constexpr int kMonitorReadyPollStepMs = 10;
		for (int waited = 0;
			waited < kMonitorReadyPollMaxMs && monitor_run != monitor_ok;
			waited += kMonitorReadyPollStepMs)
		{
			Sleep(kMonitorReadyPollStepMs);
		}
		if (monitor_run != monitor_ok)
		{
			// 监听线程启动失败（bind/socket/WSAStartup 失败）：回收句柄与残留 socket，
			// 返回错误让上层决定是否重试，不再假装监听已开启。
			if (!StopMonitorThread())
				return err_creat_socket;
			return err_creat_socket;
		}
	}
	return success;
}


/*
Monitor the physical mouse left button state
Return value:
-1: Monitoring not enabled yet. You need to call kmNet_monitor(1) first.
 0: Physical mouse left button is released
 1: Physical mouse left button is pressed
*/
int kmNet_monitor_mouse_left()
{
	if (monitor_run != monitor_ok) return -1;
	return (g_monMouseButtons.load(std::memory_order_acquire) & 0x01) ? 1 : 0;
}

/*
Monitor the physical mouse middle button state
Return value:
-1: Monitoring not enabled yet. You need to call kmNet_monitor(1) first.
 0: Physical mouse middle button is released
 1: Physical mouse middle button is pressed
*/
int kmNet_monitor_mouse_middle()
{
	if (monitor_run != monitor_ok) return -1;
	return (g_monMouseButtons.load(std::memory_order_acquire) & 0x04) ? 1 : 0;
}

/*
Monitor the physical mouse right button state
Return value:
-1: Monitoring not enabled yet. You need to call kmNet_monitor(1) first.
 0: Physical mouse right button is released
 1: Physical mouse right button is pressed
*/
int kmNet_monitor_mouse_right()
{
	if (monitor_run != monitor_ok) return -1;
	return (g_monMouseButtons.load(std::memory_order_acquire) & 0x02) ? 1 : 0;
}

/*
Monitor the physical mouse side button 1 state
Return value:
-1: Monitoring not enabled yet. You need to call kmNet_monitor(1) first.
 0: Physical mouse side button 1 is released
 1: Physical mouse side button 1 is pressed
*/
int kmNet_monitor_mouse_side1()
{
	if (monitor_run != monitor_ok) return -1;
	return (g_monMouseButtons.load(std::memory_order_acquire) & 0x08) ? 1 : 0;
}


/*
Monitor the physical mouse side button 2 state
Return value:
-1: Monitoring not enabled yet. You need to call kmNet_monitor(1) first.
 0: Physical mouse side button 2 is released
 1: Physical mouse side button 2 is pressed
*/
int kmNet_monitor_mouse_side2()
{
	if (monitor_run != monitor_ok) return -1;
	return (g_monMouseButtons.load(std::memory_order_acquire) & 0x10) ? 1 : 0;
}


// Monitor the specified keyboard key state
int kmNet_monitor_keyboard(short  vkey)
{
	unsigned char vk_key = vkey & 0xff;
	if (monitor_run != monitor_ok) return -1;
	// 只读原子镜像，避免与监听线程 memcpy 写 hw_keyboard 形成数据竞争（UB）：
	// ① 修饰键字节；② 10 个常规键压进 u64(低8键) + u16(高2键)，与 PublishMonitorState
	//    打包布局一致；一次性 acquire 加载保证修饰键与常规键数组内部一致性。
	const unsigned char        modifiers = g_monKbdModifiers.load(std::memory_order_acquire);
	const unsigned long long   keysLo    = g_monKbdKeysLo.load(std::memory_order_acquire);
	const unsigned short       keysHi    = g_monKbdKeysHi.load(std::memory_order_acquire);
	if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI) // Control key
	{
		switch (vk_key)
		{
		case KEY_LEFTCONTROL: return  modifiers & BIT0 ? 1 : 0;
		case KEY_LEFTSHIFT:   return  modifiers & BIT1 ? 1 : 0;
		case KEY_LEFTALT:     return  modifiers & BIT2 ? 1 : 0;
		case KEY_LEFT_GUI:    return  modifiers & BIT3 ? 1 : 0;
		case KEY_RIGHTCONTROL:return  modifiers & BIT4 ? 1 : 0;
		case KEY_RIGHTSHIFT:  return  modifiers & BIT5 ? 1 : 0;
		case KEY_RIGHTALT:    return  modifiers & BIT6 ? 1 : 0;
		case KEY_RIGHT_GUI:   return  modifiers & BIT7 ? 1 : 0;
		}
	}
	else // Regular key
	{
		for (int i = 0; i < 10; i++)
		{
			// 还原 hw_keyboard.data[i]：低 8 键来自 keysLo 的 i*8 位，高 2 键来自 keysHi。
			const unsigned char k =
				(i < 8) ? static_cast<unsigned char>(keysLo >> (i * 8))
				        : static_cast<unsigned char>(keysHi >> ((i - 8) * 8));
			if (k == vk_key)
				return 1;
		}
	}
	return 0;
}


/*
Enable internal box debug printing and send to the specified port (for debugging)
*/
int kmNet_debug(short port, char enable)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_debug;              // Command
	// 原写法 `port | enable << 16`：short/char 均为有符号，enable 为负时
	// `enable << 16` 属未定义行为（负数左移），port 为负时还会符号扩展污染高 16 位。
	// 改为先无符号化再组合，对 enable∈{0,1}、port>0 的常规用法逐位等价。
	const unsigned int uport = static_cast<unsigned int>(static_cast<unsigned short>(port));
	const unsigned int uenable = static_cast<unsigned int>(static_cast<unsigned char>(enable));
	tx.head.rand = uport | (uenable << 16);   // Random obfuscation value
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);

}


// Block (mask) mouse left button
int kmNet_mask_mouse_left(int enable)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_mask_mouse;         // Command
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT0) : (mask_keyboard_mouse_flag &= ~BIT0); // Block mouse left button
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}

// Block (mask) mouse right button
int kmNet_mask_mouse_right(int enable)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_mask_mouse;         // Command
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT1) : (mask_keyboard_mouse_flag &= ~BIT1); // Block mouse right button
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}

// Block (mask) mouse middle button
int kmNet_mask_mouse_middle(int enable)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_mask_mouse;         // Command
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT2) : (mask_keyboard_mouse_flag &= ~BIT2); // Block mouse middle button
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}

// Block (mask) mouse side button 1
int kmNet_mask_mouse_side1(int enable)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_mask_mouse;         // Command
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT3) : (mask_keyboard_mouse_flag &= ~BIT3); // Block mouse side button 1
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}


// Block (mask) mouse side button 2
int kmNet_mask_mouse_side2(int enable)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_mask_mouse;         // Command
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT4) : (mask_keyboard_mouse_flag &= ~BIT4); // Block mouse side button 2
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}

// Block (mask) mouse X-axis
int kmNet_mask_mouse_x(int enable)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_mask_mouse;         // Command
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT5) : (mask_keyboard_mouse_flag &= ~BIT5); // Block mouse X-axis
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}

// Block (mask) mouse Y-axis
int kmNet_mask_mouse_y(int enable)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_mask_mouse;         // Command
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT6) : (mask_keyboard_mouse_flag &= ~BIT6); // Block mouse Y-axis
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}

// Block (mask) mouse wheel
int kmNet_mask_mouse_wheel(int enable)
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_mask_mouse;         // Command
	tx.head.rand = enable ? (mask_keyboard_mouse_flag |= BIT7) : (mask_keyboard_mouse_flag &= ~BIT7); // Block mouse wheel
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}


// Block (mask) the specified keyboard key
int kmNet_mask_keyboard(short vkey)
{
	BYTE v_key = vkey & 0xff;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_mask_mouse;         // Command
	tx.head.rand = (mask_keyboard_mouse_flag & 0xff) | (v_key << 8); // Mask keyboard vkey
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}


// Unblock the specified keyboard key
int kmNet_unmask_keyboard(short vkey)
{
	BYTE v_key = vkey & 0xff;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_unmask_all;         // Command
	tx.head.rand = (mask_keyboard_mouse_flag & 0xff) | (v_key << 8); // Unmask keyboard vkey
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}


// Unblock all previously set physical blocks
int kmNet_unmask_all()
{
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_unmask_all;         // Command
	mask_keyboard_mouse_flag = 0;
	tx.head.rand = mask_keyboard_mouse_flag;
	int length = sizeof(cmd_head_t);
	return SendCommandAndAwaitAck(length);
}


// Set configuration info (change IP and port)
int kmNet_setconfig(char* ip, unsigned short port)
{
    if (ip == nullptr)
        return err_creat_socket;
    if (inet_addr(ip) == INADDR_NONE)
        return err_creat_socket;
    if (port == 0)
        return err_creat_socket;

	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	tx.head.indexpts++;                   // Command statistics value
	tx.head.cmd = cmd_setconfig;          // Command
	tx.head.rand = inet_addr(ip);
	tx.u8buff.buff[0] = port >> 8;
	tx.u8buff.buff[1] = port >> 0;
	int length = sizeof(cmd_head_t) + 2;
	return SendCommandAndAwaitAck(length);
}


// Fill the entire LCD screen with the specified color. Use black for clearing the screen.
int kmNet_lcd_color(unsigned short rgb565)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	for (int y = 0; y < 40; y++)
	{
		tx.head.indexpts++;           // Command statistics value
		tx.head.cmd = cmd_showpic;    // Command
		tx.head.rand = 0 | y * 4;
		for (int c = 0; c < 512; c++)
			tx.u16buff.buff[c] = rgb565;
		int length = sizeof(cmd_head_t) + 1024;
		// 逐块校验应答：原实现只在整轮结束后拿最后一帧的 rx 与 tx 比对，中间块的
		// 命令码/序号错配会被静默吞掉；且设备一旦失联，仍会继续向其灌满剩余的
		// 1040 字节报文（40 块 × 300ms 超时 = 最长 12 秒卡在 UI 线程）。改为失败即返回。
		err = SendCommandAndAwaitAck(length);
		if (err != success)
			return err;
	}
	return success;

}

// Display a 128x80 image at the bottom
int kmNet_lcd_picture_bottom(unsigned char* buff_128_80)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	for (int y = 0; y < 20; y++)
	{
		tx.head.indexpts++;           // Command statistics value
		tx.head.cmd = cmd_showpic;    // Command
		tx.head.rand = 80 + y * 4;
		memcpy(tx.u8buff.buff, &buff_128_80[y * 1024], 1024);
		int length = sizeof(cmd_head_t) + 1024;
		// 逐块校验应答：原实现只在整轮结束后拿最后一帧的 rx 与 tx 比对，中间块的
		// 命令码/序号错配会被静默吞掉；且设备一旦失联，仍会继续向其灌满剩余的
		// 1040 字节报文（40 块 × 300ms 超时 = 最长 12 秒卡在 UI 线程）。改为失败即返回。
		err = SendCommandAndAwaitAck(length);
		if (err != success)
			return err;
	}
	return success;
}

// Display a 128x160 image at the bottom
int kmNet_lcd_picture(unsigned char* buff_128_160)
{
	int err;
	if (!IsSocketValid(sockClientfd))        return err_creat_socket;
	for (int y = 0; y < 40; y++)
	{
		tx.head.indexpts++;           // Command statistics value
		tx.head.cmd = cmd_showpic;    // Command
		tx.head.rand = y * 4;
		memcpy(tx.u8buff.buff, &buff_128_160[y * 1024], 1024);
		int length = sizeof(cmd_head_t) + 1024;
		// 逐块校验应答：原实现只在整轮结束后拿最后一帧的 rx 与 tx 比对，中间块的
		// 命令码/序号错配会被静默吞掉；且设备一旦失联，仍会继续向其灌满剩余的
		// 1040 字节报文（40 块 × 300ms 超时 = 最长 12 秒卡在 UI 线程）。改为失败即返回。
		err = SendCommandAndAwaitAck(length);
		if (err != success)
			return err;
	}
	return success;
}
